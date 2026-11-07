/*
 * MCPX APU DSP (GP/EP) execution, DMA, and frame scheduling.
 *
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"
#include "apu_dma_regs.h"
#include "apu_dsp_trace.h"

#include <stdlib.h>
#include <string.h>

#define DSP_FRAME_CYCLES 88875u
#define DSP_START_FRAME_INTERRUPT (1u << 1)
#define DSP_DMA_EOL_INTERRUPT (1u << 7)
#define DSP_PERIPHERAL_BASE 0xFFFF80u
#define DSP_PERIPHERAL_END 0x1000000u
#define DSP_DMA_FIFO_BOUND_MASK 0x00ffff00u

static uint32_t dsp_dma_memory_read(void *opaque, int space, uint32_t addr)
{
    DSPState *dsp = opaque;
    uint32_t value = 0;
    if (dsp && dsp->engine_ready && space >= MCPX_DSP_SPACE_X &&
        space <= MCPX_DSP_SPACE_P &&
        mcpx_dsp_engine_read(&dsp->engine, (McpxDspMemorySpace)space,
                             addr, &value))
        return value;
    return 0;
}

static void dsp_dma_memory_write(void *opaque, int space, uint32_t addr,
                                uint32_t value)
{
    DSPState *dsp = opaque;
    if (dsp && dsp->engine_ready && space >= MCPX_DSP_SPACE_X &&
        space <= MCPX_DSP_SPACE_P)
        mcpx_dsp_engine_write(&dsp->engine, (McpxDspMemorySpace)space,
                              addr, value);
}

static void dsp_dma_memory_read_run(void *opaque, int space, uint32_t addr,
                                    uint32_t *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        out[i] = dsp_dma_memory_read(opaque, space, addr + i);
}

static void dsp_dma_memory_write_run(void *opaque, int space, uint32_t addr,
                                     const uint32_t *values, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        dsp_dma_memory_write(opaque, space, addr + i, values[i]);
}

/* Resolve a scratch/FIFO logical address through the guest SGE table without
 * allowing an invalid table entry or page to wrap into another RAM region. */
static bool dsp_dma_sge_copy(DSPState *dsp, bool fifo, uint8_t *bytes,
                             uint32_t addr, size_t len, bool write_guest)
{
    MCPXAPUState *apu = dsp ? dsp->apu : NULL;
    uint32_t table_reg, max_reg;
    uint64_t ram_limit;
    if (!apu || !apu->ram_ptr) return false;
    if (dsp->is_gp) {
        table_reg = fifo ? NV_PAPU_GPFADDR : NV_PAPU_GPSADDR;
        max_reg = fifo ? NV_PAPU_GPFMAXSGE : NV_PAPU_GPSMAXSGE;
    } else {
        table_reg = fifo ? NV_PAPU_EPFADDR : NV_PAPU_EPSADDR;
        max_reg = fifo ? NV_PAPU_EPFMAXSGE : NV_PAPU_EPSMAXSGE;
    }
    ram_limit = MIN((uint64_t)apu->ram_size, (uint64_t)APU_PHYS_MASK + 1u);
    while (len) {
        uint64_t entry = (uint64_t)(apu->regs[table_reg] & APU_PHYS_MASK) +
                         (uint64_t)(addr / TARGET_PAGE_SIZE) * 8u;
        uint32_t page;
        size_t offset = addr % TARGET_PAGE_SIZE;
        size_t chunk = MIN(len, TARGET_PAGE_SIZE - offset);
        uint64_t physical;
        if (addr / TARGET_PAGE_SIZE > apu->regs[max_reg] || entry > ram_limit ||
            sizeof(page) > ram_limit - entry) return false;
        memcpy(&page, apu->ram_ptr + (size_t)entry, sizeof(page));
        physical = (uint64_t)(page & APU_PHYS_MASK) + offset;
        if (physical > ram_limit || chunk > ram_limit - physical) return false;
        if (write_guest) memcpy(apu->ram_ptr + (size_t)physical, bytes, chunk);
        else memcpy(bytes, apu->ram_ptr + (size_t)physical, chunk);
        addr += (uint32_t)chunk;
        bytes += chunk;
        len -= chunk;
    }
    return true;
}

static bool dsp_dma_scratch_rw(void *opaque, uint8_t *bytes, uint32_t addr,
                               size_t len, bool dir)
{
    DSPState *dsp = opaque;
    bool ok = dsp_dma_sge_copy(dsp, false, bytes, addr, len, dir);
    if (!ok && dsp) dsp->dma.error = true;
    return ok;
}

static void dsp_capture_ep_fifo_chunk(DSPState *dsp, const uint8_t *bytes,
                                      size_t len)
{
    const size_t frame_size = sizeof(dsp->ep_fifo_frame_buf);

    /* One complete block owns the monitor epoch. Keep the first frame
     * independent of DMA descriptor boundaries and discard its tail. */
    if (dsp->ep_fifo_frame_ready) return;
    len = MIN(len, frame_size - dsp->ep_fifo_frame_bytes_used);

    memcpy((uint8_t *)dsp->ep_fifo_frame_buf +
               dsp->ep_fifo_frame_bytes_used,
           bytes, len);
    dsp->ep_fifo_frame_bytes_used += len;
    if (dsp->ep_fifo_frame_bytes_used == frame_size)
        dsp->ep_fifo_frame_ready = true;
}

static bool dsp_dma_fifo_rw(void *opaque, uint8_t *bytes, unsigned int index,
                            size_t len, bool dir)
{
    DSPState *dsp = opaque;
    MCPXAPUState *apu = dsp ? dsp->apu : NULL;
    uint32_t base_reg, end_reg, cur_reg, cur, end;
    uint8_t *original_bytes = bytes;
    size_t original_len = len;
    if (!apu || index >= (dir ? 4u : 2u)) {
        if (dsp) dsp->dma.error = true;
        return false;
    }
    if (dsp->is_gp) {
        base_reg = (dir ? NV_PAPU_GPOFBASE0 : NV_PAPU_GPIFBASE0) + 0x10u * index;
        end_reg = (dir ? NV_PAPU_GPOFEND0 : NV_PAPU_GPIFEND0) + 0x10u * index;
        cur_reg = (dir ? NV_PAPU_GPOFCUR0 : NV_PAPU_GPIFCUR0) + 0x10u * index;
    } else {
        base_reg = (dir ? NV_PAPU_EPOFBASE0 : NV_PAPU_EPIFBASE0) + 0x10u * index;
        end_reg = (dir ? NV_PAPU_EPOFEND0 : NV_PAPU_EPIFEND0) + 0x10u * index;
        cur_reg = (dir ? NV_PAPU_EPOFCUR0 : NV_PAPU_EPIFCUR0) + 0x10u * index;
    }
    cur = apu->regs[cur_reg] & 0x00ffffffu;
    end = apu->regs[end_reg] & DSP_DMA_FIFO_BOUND_MASK;
    if (cur >= end) { dsp->dma.error = true; return false; }
    while (len) {
        size_t chunk = len;
        bool wraps = cur < end && len >= (size_t)(end - cur);
        if (cur < end) chunk = MIN(chunk, (size_t)(end - cur));
        if (!dsp_dma_sge_copy(dsp, true, bytes, cur, chunk, dir)) {
            dsp->dma.error = true;
            return false;
        }
        bytes += chunk;
        len -= chunk;
        cur = wraps ? (apu->regs[base_reg] & DSP_DMA_FIFO_BOUND_MASK)
                    : cur + (uint32_t)chunk;
    }
    apu->regs[cur_reg] = cur;
    bool ep_fifo0_output = !dsp->is_gp && dir && index == 0;
    bool ep_frame_was_ready = dsp->ep_fifo_frame_ready;
    if (ep_fifo0_output && apu->trace_dsp_state) {
        apu->dsp_trace_ep_fifo_writes++;
        apu->dsp_trace_ep_fifo_bytes += original_len;
    }
    if (ep_fifo0_output) {
        dsp_capture_ep_fifo_chunk(dsp, original_bytes, original_len);
        if (!ep_frame_was_ready && dsp->ep_fifo_frame_ready) {
            if (apu->trace_dsp_state)
                apu->dsp_trace_ep_fifo_captured++;
            if (apu->monitor.point == MCPX_APU_DEBUG_MON_EP ||
                apu->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP) {
                memcpy(apu->monitor.frame_buf, dsp->ep_fifo_frame_buf,
                       sizeof(apu->monitor.frame_buf));
                apu->monitor.ep_fifo_frame_valid = true;
            }
        }
    }
    return true;
}

/* Program-memory readback and the boot ROM's initial scratch transfer.
 * The reset transition, SGE layout and 0x800-word transfer follow xemu's
 * hw/xbox/mcpx/apu/dsp/gp_ep.c and dsp_c.c (commit 478b4f4). This models the
 * boot transfer only; it does not execute DSP instructions or effects. */
#define APU_DSP_BOOT_WORDS 0x800u
#define APU_DSP_BOOT_PAGES (APU_DSP_BOOT_WORDS * 4u / TARGET_PAGE_SIZE)

typedef char apu_dsp_boot_fits_pram[
    APU_DSP_BOOT_WORDS <= DSP_PRAM_SIZE ? 1 : -1];

static void dsp_boot_trace(MCPXAPUState *d, bool is_gp, const uint32_t *image,
                           uint32_t table, uint32_t max_sge, const char *reason)
{
    unsigned int which = is_gp ? 0 : 1, i;
    uint32_t checksum = 2166136261u;
    if (!d->trace_dsp_boot || d->dsp_boot_trace_events[which] >= 4) return;
    d->dsp_boot_trace_events[which]++;
    if (!image) {
        fprintf(stderr, "[APU] %s DSP bootstrap rejected: %s"
                        " (SGE=%08X max=%u RAM=%zu)\n",
                is_gp ? "GP" : "EP", reason, table, max_sge, d->ram_size);
        return;
    }
    /* FNV-1a over the loaded little-endian, 24-bit-masked image bytes. */
    for (i = 0; i < APU_DSP_BOOT_WORDS * 4; i++) {
        checksum = (checksum ^ ((const uint8_t *)image)[i]) * 16777619u;
    }
    fprintf(stderr, "[APU] %s DSP bootstrap: SGE=%08X max=%u"
                    " words=%u checksum=%08X word0..7="
                    "%06X,%06X,%06X,%06X,%06X,%06X,%06X,%06X\n",
            is_gp ? "GP" : "EP", table, max_sge, APU_DSP_BOOT_WORDS, checksum,
            image[0], image[1], image[2], image[3],
            image[4], image[5], image[6], image[7]);
}

static bool dsp_bootstrap(MCPXAPUState *d, DSPState *dsp, bool is_gp)
{
    uint32_t image[APU_DSP_BOOT_WORDS];
    size_t pages[APU_DSP_BOOT_PAGES];
    uint64_t limit = MIN((uint64_t)d->ram_size,
                         (uint64_t)APU_PHYS_MASK + 1u);
    uint32_t table = qatomic_read(&d->regs[is_gp ? NV_PAPU_GPSADDR
                                                : NV_PAPU_EPSADDR]);
    uint32_t max_sge = qatomic_read(&d->regs[is_gp ? NV_PAPU_GPSMAXSGE
                                                  : NV_PAPU_EPSMAXSGE]);
    unsigned int i;

    if (!d->ram_ptr) {
        dsp_boot_trace(d, is_gp, NULL, table, max_sge, "no RAM");
        return false;
    }

    /* Decode each physical address once, then do widened offset arithmetic.
     * Re-masking after adding an offset could wrap an invalid read into RAM.
     * max_sge is inclusive. SGE +4 control is unused by the boot transfer. */
    for (i = 0; i < APU_DSP_BOOT_PAGES; ++i) {
        uint64_t entry = (uint64_t)(table & APU_PHYS_MASK) + (uint64_t)i * 8u;
        uint32_t page;
        uint64_t physical;
        if (i > max_sge || entry > limit || sizeof(page) > limit - entry) {
            dsp_boot_trace(d, is_gp, NULL, table, max_sge, "SGE outside backing");
            return false;
        }
        memcpy(&page, d->ram_ptr + (size_t)entry, sizeof(page));
        physical = page & APU_PHYS_MASK;
        if (physical > limit || TARGET_PAGE_SIZE > limit - physical) {
            dsp_boot_trace(d, is_gp, NULL, table, max_sge, "page outside backing");
            return false;
        }
        pages[i] = (size_t)physical;
    }

    /* No PRAM changes until every descriptor and source range is valid.
     * The device mutex excludes both MMIO readers and the frame thread. */
    for (i = 0; i < APU_DSP_BOOT_PAGES; ++i) {
        memcpy((uint8_t *)image + i * TARGET_PAGE_SIZE,
               d->ram_ptr + pages[i], TARGET_PAGE_SIZE);
    }
    for (i = 0; i < APU_DSP_BOOT_WORDS; ++i) image[i] &= 0x00ffffffu;
    memcpy(dsp->core.pram, image, sizeof(image));
    memset((void *)dsp->core.pram_opcache, 0, sizeof(dsp->core.pram_opcache));
    mcpx_dsp_engine_invalidate_cache(&dsp->engine);
    dsp_boot_trace(d, is_gp, image, table, max_sge, NULL);
    return true;
}

static uint64_t dsp_cycles_total(const DSPState *dsp)
{
    return dsp->cycles_base +
        (dsp->engine_ready ? dsp56300_cycle_count(dsp->engine.jit) : 0);
}

/* A program that only reads the interrupt status, always the same value, is
 * waiting for the next frame: nothing it can see changes before then. BLACK's
 * EP spends every frame here, about 17000 peripheral callbacks each. Leave the
 * frame through the HALT path after a few reads; dsp_run_frame takes the HALT
 * back, so the frame start logic sees a program that never stopped. */
#define DSP_IDLE_POLLS 16u

static void dsp_note_status_poll(DSPState *dsp, uint32_t value)
{
    if (value & DSP_START_FRAME_INTERRUPT) return;
    if (dsp->idle_polls && value != dsp->idle_poll_value) dsp->idle_polls = 0;
    dsp->idle_poll_value = value;
    if (++dsp->idle_polls >= DSP_IDLE_POLLS &&
        !dsp56300_halt_requested(dsp->engine.jit)) {
        dsp56300_set_halt_requested(dsp->engine.jit, true);
        dsp->idle_parked = true;
    }
}

static uint32_t dsp_read_peripheral(void *opaque, uint32_t address)
{
    DSPState *dsp = (DSPState *)opaque;
    if (address != 0xFFFFC5) dsp->idle_polls = 0;
    switch (address) {
    case 0xFFFFB0: return dsp->timer_ctl;
    case 0xFFFFB2: return dsp->timer_period;
    case 0xFFFFB3:
        /* The current frame counter is validated for GP. Keep the EP timer
         * dormant until its per-frame accounting matches hardware. */
        if (dsp->is_gp && (dsp->timer_ctl & 1))
            return (uint32_t)(dsp_cycles_total(dsp) - dsp->timer_base) & 0xFFFFFFu;
        return 0;
    case 0xFFFFC5: {
        uint32_t status = dsp->interrupts | dsp->dma.pending_interrupts |
                          (dsp->dma.eol ? DSP_DMA_EOL_INTERRUPT : 0);
        dsp_note_status_poll(dsp, status);
        return status;
    }
    case 0xFFFFD4: return dsp_dma_read(&dsp->dma, DMA_NEXT_BLOCK);
    case 0xFFFFD5: return dsp_dma_read(&dsp->dma, DMA_START_BLOCK);
    case 0xFFFFD6: return dsp_dma_read(&dsp->dma, DMA_CONTROL);
    case 0xFFFFD7: return dsp_dma_read(&dsp->dma, DMA_CONFIGURATION);
    default: return 0;
    }
}

static void dsp_write_peripheral(void *opaque, uint32_t address, uint32_t value)
{
    DSPState *dsp = (DSPState *)opaque;
    dsp->idle_polls = 0;
    switch (address) {
    case 0xFFFFB0:
        dsp->timer_ctl = value;
        break;
    case 0xFFFFB1:
        dsp->timer_base = dsp_cycles_total(dsp);
        break;
    case 0xFFFFB2:
        dsp->timer_period = value;
        break;
    case 0xFFFFC4:
        if (value & 1) {
            dsp56300_set_halt_requested(dsp->engine.jit, true);
            dsp->halted_since_reset = true;
            if (dsp->frame_starts_pending)
                dsp->interrupts |= DSP_START_FRAME_INTERRUPT;
        }
        break;
    case 0xFFFFC5:
        dsp->interrupts &= ~value;
        if ((value & DSP_START_FRAME_INTERRUPT) &&
            dsp->frame_starts_pending && dsp->halted_since_reset)
            dsp->frame_starts_pending--;
        dsp->dma.pending_interrupts &= ~value;
        if (value & DSP_DMA_EOL_INTERRUPT) {
            dsp_dma_completion_seen(&dsp->dma);
            dsp->dma.eol = false;
        }
        break;
    case 0xFFFFD4:
        dsp_dma_write(&dsp->dma, DMA_NEXT_BLOCK, value);
        break;
    case 0xFFFFD5:
        dsp_dma_write(&dsp->dma, DMA_START_BLOCK, value);
        break;
    case 0xFFFFD6:
        dsp_dma_write(&dsp->dma, DMA_CONTROL, value);
        break;
    case 0xFFFFD7:
        dsp_dma_write(&dsp->dma, DMA_CONFIGURATION, value);
        break;
    default:
        break;
    }
}

static bool dsp_engine_init(DSPState *dsp)
{
    Dsp56300MemoryRegion x_regions[3], y_regions[2], p_regions[1];
    Dsp56300CallbackRegion peripheral = {
        .opaque = dsp,
        .read = dsp_read_peripheral,
        .write = dsp_write_peripheral
    };
    Dsp56300MemoryMap map = {0};
    uint32_t x_count = 0, y_count = 0;
    const uint32_t x_words = dsp->is_gp ? DSP_XRAM_SIZE : 0x0C00u;
    const uint32_t y_words = dsp->is_gp ? DSP_YRAM_SIZE : 0x0100u;

    x_regions[x_count++] = (Dsp56300MemoryRegion){
        .start = 0, .end = x_words,
        .kind = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = dsp->core.xram, .offset = 0 } }
    };
    if (dsp->is_gp) {
        x_regions[x_count++] = (Dsp56300MemoryRegion){
            .start = 0x1400, .end = 0x1800,
            .kind = DSP56300_REGION_BUFFER,
            .data = { .buffer = { .base = dsp->core.xram, .offset = 0x0C00 } }
        };
    }
    x_regions[x_count++] = (Dsp56300MemoryRegion){
        .start = DSP_PERIPHERAL_BASE, .end = DSP_PERIPHERAL_END,
        .kind = DSP56300_REGION_CALLBACK,
        .data = { .callback = peripheral }
    };
    y_regions[y_count++] = (Dsp56300MemoryRegion){
        .start = 0, .end = y_words,
        .kind = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = dsp->core.yram, .offset = 0 } }
    };
    y_regions[y_count++] = (Dsp56300MemoryRegion){
        .start = DSP_PERIPHERAL_BASE, .end = DSP_PERIPHERAL_END,
        .kind = DSP56300_REGION_CALLBACK,
        .data = { .callback = peripheral }
    };
    p_regions[0] = (Dsp56300MemoryRegion){
        .start = 0, .end = DSP_PRAM_SIZE,
        .kind = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = dsp->core.pram, .offset = 0 } }
    };
    map.x_regions = x_regions;
    map.x_count = x_count;
    map.y_regions = y_regions;
    map.y_count = y_count;
    map.p_regions = p_regions;
    map.p_count = 1;
    return mcpx_dsp_engine_init_with_map(&dsp->engine, &map);
}

static void dsp_reset_core(DSPState *dsp)
{
    dsp->core.pc = 0;
    dsp->core.cycle_count = 0;
    dsp->save_cycles = 0;
    dsp->interrupts = 0;
    dsp->frame_starts_pending = 0;
    dsp->idle_polls = 0;
    dsp->idle_parked = false;
    dsp->timer_ctl = 0;
    dsp->timer_period = 0;
    dsp->timer_base = 0;
    dsp->cycles_base = 0;
    dsp->halted_since_reset = false;
    dsp_dma_reset(&dsp->dma);
    dsp->dma_fallback_logged = false;
    dsp->trace_frame_count = 0;
    dsp->trace_events_logged = 0;
    dsp->ep_fifo_frame_bytes_used = 0;
    dsp->ep_fifo_frame_ready = false;
    if (!dsp->is_gp && dsp->apu) {
        /* Every EP reset path releases native ownership, including reset
         * MMIO and bootstrap. Do not replay a discarded native PCM block. */
        if (dsp->apu->monitor.ep_fifo_frame_valid)
            memset(dsp->apu->monitor.frame_buf, 0,
                   sizeof(dsp->apu->monitor.frame_buf));
        dsp->apu->monitor.ep_fifo_frame_valid = false;
    }
    if (dsp->engine_ready) mcpx_dsp_engine_reset(&dsp->engine);
}

static void dsp_run_frame(DSPState *dsp, bool start_frame)
{
    Dsp56300State state;
    if (!dsp || !dsp->engine_ready) return;

    if (start_frame) {
        dsp->frame_starts_pending++;
        if (dsp56300_halt_requested(dsp->engine.jit))
            dsp->interrupts |= DSP_START_FRAME_INTERRUPT;
    }
    dsp->cycles_base += DSP_FRAME_CYCLES;
    /* The JIT adds to its saved budget. A peripheral HALT leaves positive
     * unused cycles there; discard those between hardware frames, while
     * retaining a completed instruction's negative cycle overshoot. State
     * synchronization is safe here, outside the execution callback. */
    dsp56300_get_state(dsp->engine.jit, &state);
    if (state.cycle_budget > 0) state.cycle_budget = 0;
    state.cycle_count = 0;
    state.halt_requested = false;
    dsp56300_set_state(dsp->engine.jit, &state);
    dsp->idle_polls = 0;
    dsp56300_run(dsp->engine.jit, (int32_t)DSP_FRAME_CYCLES);
    if (dsp->idle_parked) {
        dsp->idle_parked = false;
        dsp56300_set_halt_requested(dsp->engine.jit, false);
    }
    dsp56300_get_state(dsp->engine.jit, &state);
    if (state.power_state != 0)
        dsp56300_set_halt_requested(dsp->engine.jit, true);

    dsp->core.pc = state.pc;
    dsp->core.cycle_count = dsp56300_cycle_count(dsp->engine.jit);
    dsp->core.is_idle = dsp56300_halt_requested(dsp->engine.jit);
    memcpy(dsp->core.registers, state.registers, sizeof(state.registers));
    memcpy(dsp->core.stack, state.stack, sizeof(state.stack));

    if (dsp->apu->trace_dsp_state) {
        const uint64_t trace_frame = ++dsp->trace_frame_count;
        uint32_t omr = state.registers[DSP56300_REG_OMR];
        uint32_t sp = state.registers[DSP56300_REG_SP];
        uint32_t sc = state.registers[DSP56300_REG_SC];
        uint32_t events = mcpx_dsp_trace_events(omr, sp,
                                                state.interrupts.pending_bits[0]);
        const char *core = dsp->is_gp ? "GP" : "EP";
        if (mcpx_dsp_trace_sample_due(trace_frame)) {
            fprintf(stderr, "[DSPTRACE] core=%s event=frame-sample frame=%llu PC=%06X OMR=%06X SP=%02X SC=%02X\n",
                    core, (unsigned long long)trace_frame, state.pc, omr, sp, sc);
        }
        if ((events & MCPX_DSP_TRACE_SEN) &&
            !(dsp->trace_events_logged & MCPX_DSP_TRACE_SEN)) {
            fprintf(stderr, "[DSPTRACE] core=%s event=SEN-enabled frame=%llu PC=%06X OMR=%06X SP=%02X SC=%02X\n",
                    core, (unsigned long long)trace_frame, state.pc, omr, sp, sc);
            dsp->trace_events_logged |= MCPX_DSP_TRACE_SEN;
        }
        uint32_t stack_events = events & MCPX_DSP_TRACE_STACK_MASK &
                                ~dsp->trace_events_logged;
        if (stack_events) {
            fprintf(stderr, "[DSPTRACE] core=%s event=stack-status flags=%02X frame=%llu PC=%06X OMR=%06X SP=%02X SC=%02X pending=%016llX\n",
                    core, stack_events, (unsigned long long)trace_frame, state.pc, omr, sp, sc,
                    (unsigned long long)state.interrupts.pending_bits[0]);
            dsp->trace_events_logged |= stack_events;
        }
    }
}

static bool dsp_mmio_valid(hwaddr addr, unsigned int size)
{
    return size == 4 && !(addr & 3) &&
           ((addr >= NV_PAPU_GPPMEM &&
             addr < NV_PAPU_GPPMEM + DSP_PRAM_SIZE * 4u) ||
            addr == NV_PAPU_GPRST);
}

uint64_t mcpx_apu_dsp_mmio_read(MCPXAPUState *d, hwaddr addr,
                               unsigned int size, bool is_gp)
{
    DSPState *dsp;
    uint32_t *regs, result;
    if (!d || !dsp_mmio_valid(addr, size)) return 0;
    dsp = is_gp ? d->gp.dsp : d->ep.dsp;
    if (!dsp) return 0;
    regs = is_gp ? d->gp.regs : d->ep.regs;
    /* An aligned 32-bit read: it must not wait for the frame thread, which keeps
     * d->lock across whole frames. */
    result = addr == NV_PAPU_GPRST ? regs[addr]
                                  : dsp->core.pram[(addr - NV_PAPU_GPPMEM) / 4];
    return result;
}

void mcpx_apu_dsp_mmio_write(MCPXAPUState *d, hwaddr addr, uint64_t val,
                            unsigned int size, bool is_gp)
{
    DSPState *dsp;
    uint32_t *regs;
    if (!d || !dsp_mmio_valid(addr, size)) return;
    dsp = is_gp ? d->gp.dsp : d->ep.dsp;
    if (!dsp) return;
    regs = is_gp ? d->gp.regs : d->ep.regs;
    mcpx_apu_lock(d);
    if (addr == NV_PAPU_GPRST) {
        const uint32_t enabled = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
        uint32_t old = regs[addr], value = (uint32_t)val;
        if ((value & enabled) != enabled) {
            /* On-chip RAM survives reset; execution state does not. */
            dsp_reset_core(dsp);
        } else if ((old & enabled) != enabled) {
            if (dsp_bootstrap(d, dsp, is_gp)) dsp_reset_core(dsp);
        }
        /* Preserve the full reset register, including unmodeled bits. A
         * failed transfer leaves old PRAM intact and still records this
         * reset write; another load requires a new reset-release edge. */
        regs[addr] = value;
    } else {
        if (dsp->engine_ready) {
            mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_P,
                                  (addr - NV_PAPU_GPPMEM) / 4,
                                  (uint32_t)val);
        } else {
            dsp->core.pram[(addr - NV_PAPU_GPPMEM) / 4] =
                (uint32_t)val & 0x00ffffffu;
        }
        dsp->core.pram_opcache[(addr - NV_PAPU_GPPMEM) / 4] = NULL;
    }
    qemu_mutex_unlock(&d->lock);
}

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* Include the dry speaker and 3D submixes when native EP output is absent.
 * RECOMP_APU_MIXDOWN_ALL=0 retains the diagnostic front-only fallback. The
 * historical variable name does not make effect sends into speakers. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

int mcpx_apu_set_stereo_fallback_pair(MCPXAPUState *d,
                                      unsigned int left_bin,
                                      unsigned int right_bin)
{
    if (!d || left_bin < 10 || left_bin >= NUM_MIXBINS ||
        right_bin < 10 || right_bin >= NUM_MIXBINS || left_bin == right_bin)
        return 0;
    uint32_t pair = (left_bin + 1u) | ((right_bin + 1u) << 8);
#ifdef _MSC_VER
    _InterlockedExchange((long volatile *)&d->stereo_fallback_pair, (long)pair);
#else
    __atomic_store_n(&d->stereo_fallback_pair, pair, __ATOMIC_RELAXED);
#endif
    return 1;
}

static uint32_t stereo_fallback_pair_read(MCPXAPUState *d)
{
#ifdef _MSC_VER
    return (uint32_t)_InterlockedCompareExchange(
        (long volatile *)&d->stereo_fallback_pair, 0, 0);
#else
    return __atomic_load_n(&d->stereo_fallback_pair, __ATOMIC_RELAXED);
#endif
}

static void dsp_ack_frame(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        /* The doorbell is named as the title sees it, a contiguous-window
         * VA (0x83218810 is ebx at DirectSound's spin); ram_ptr is physical
         * address 0, so the window bit goes, as it does for every other
         * address the APU follows. */
        uint32_t *slot = (uint32_t *)(d->ram_ptr + (s_dsp_ack[i] & APU_PHYS_MASK));
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

extern void xbox_SetAckTick(void (*fn)(void));
static MCPXAPUState *s_ack_state;

/* The doorbell, cleared on every turn of the kernel's register-acknowledge loop as well as once per audio frame: a
 * title that writes a command and then spins on it should not wait for the next frame. */
static void dsp_ack_poll(void)
{
    if (s_ack_state) dsp_ack_frame(s_ack_state);
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->fallback_limiter_gain = 1.0;
    s_ack_state = d;
    if (!getenv("RECOMP_APU_DSP_ACK_FRAME_ONLY")) xbox_SetAckTick(dsp_ack_poll);
    const char *trace = getenv("RECOMP_APU_TRACE_DSP_BOOT");
    d->trace_dsp_boot = trace && strcmp(trace, "1") == 0;
    trace = getenv("RECOMP_APU_TRACE_DSP_STATE");
    d->trace_dsp_state = trace && strcmp(trace, "1") == 0;
    if (d->trace_dsp_state)
        fprintf(stderr, "[APU] DSP state trace enabled (frame-boundary samples; transient events may be missed)\n");
    /* GP and EP have independent registers, memories, and DSP engines. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;
    if (d->gp.dsp) {
        d->gp.dsp->apu = d;
        d->gp.dsp->engine_ready = dsp_engine_init(d->gp.dsp);
        d->gp.dsp->dma.mem_opaque = d->gp.dsp;
        d->gp.dsp->dma.mem_read = dsp_dma_memory_read;
        d->gp.dsp->dma.mem_write = dsp_dma_memory_write;
        d->gp.dsp->dma.mem_read_run = dsp_dma_memory_read_run;
        d->gp.dsp->dma.mem_write_run = dsp_dma_memory_write_run;
        d->gp.dsp->dma.rw_opaque = d->gp.dsp;
        d->gp.dsp->dma.scratch_rw = dsp_dma_scratch_rw;
        d->gp.dsp->dma.fifo_rw = dsp_dma_fifo_rw;
        d->gp.dsp->dma.is_gp = true;
    }
    if (d->ep.dsp) {
        d->ep.dsp->apu = d;
        d->ep.dsp->engine_ready = dsp_engine_init(d->ep.dsp);
        d->ep.dsp->dma.mem_opaque = d->ep.dsp;
        d->ep.dsp->dma.mem_read = dsp_dma_memory_read;
        d->ep.dsp->dma.mem_write = dsp_dma_memory_write;
        d->ep.dsp->dma.mem_read_run = dsp_dma_memory_read_run;
        d->ep.dsp->dma.mem_write_run = dsp_dma_memory_write_run;
        d->ep.dsp->dma.rw_opaque = d->ep.dsp;
        d->ep.dsp->dma.scratch_rw = dsp_dma_scratch_rw;
        d->ep.dsp->dma.fifo_rw = dsp_dma_fifo_rw;
        d->ep.dsp->dma.is_gp = false;
    }

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (GP=%s EP=%s; "
                    "direct-mixdown output fallback active)\n",
            d->gp.dsp && d->gp.dsp->engine_ready ? "DSP56300" : "unavailable",
            d->ep.dsp && d->ep.dsp->engine_ready ? "DSP56300" : "unavailable");
}

void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    if (!d) return;
    if (d->gp.dsp) {
        dsp_dma_destroy(&d->gp.dsp->dma);
        if (d->gp.dsp->engine_ready)
            mcpx_dsp_engine_destroy(&d->gp.dsp->engine);
        free(d->gp.dsp);
        d->gp.dsp = NULL;
    }
    if (d->ep.dsp) {
        dsp_dma_destroy(&d->ep.dsp->dma);
        if (d->ep.dsp->engine_ready)
            mcpx_dsp_engine_destroy(&d->ep.dsp->engine);
        free(d->ep.dsp);
        d->ep.dsp = NULL;
    }
    if (s_ack_state == d) s_ack_state = NULL;
}

void mcpx_apu_dsp_reset(MCPXAPUState *d)
{
    if (!d) return;
    d->fallback_limiter_gain = 1.0;
    if (d->gp.dsp) dsp_reset_core(d->gp.dsp);
    if (d->ep.dsp) dsp_reset_core(d->ep.dsp);
    d->monitor.ep_fifo_frame_valid = false;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Run GP effects and EP encoding for this 32-sample tick. A complete
     * native EP PCM block owns the eight-tick monitor epoch; otherwise
     * direct mixdown supplies each tick's fallback stereo slice. */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
    /* Both channels use one atomic configuration snapshot for this tick. */
    uint32_t fallback_pair = stereo_fallback_pair_read(d);

    if (mixbins && d->gp.dsp && d->gp.dsp->engine_ready &&
        (d->gp.regs[NV_PAPU_GPRST] &
         (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST)) ==
        (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST)) {
        for (int b = 0; b < NUM_MIXBINS; ++b) {
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
                d->gp.dsp->core.xram[0x0C00 + b * NUM_SAMPLES_PER_FRAME + i] =
                    float_to_24b(mixbins[b][i]);
            }
        }

        dsp_run_frame(d->gp.dsp, true);
        g_dbg.gp.cycles = d->gp.dsp->core.cycle_count;
    }
    if (d->ep.dsp && d->ep.dsp->engine_ready &&
        (d->ep.regs[NV_PAPU_EPRST] &
         (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST)) ==
        (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST)) {
        dsp_run_frame(d->ep.dsp, (d->ep_frame_div % 8) == 0);
        g_dbg.ep.cycles = d->ep.dsp->core.cycle_count;
    }

    /* Retain the compatibility handshake for command behavior beyond the
     * modeled DSP execution and DMA path. */
    dsp_ack_frame(d);

    if (d->trace_output && d->monitor.ep_fifo_frame_valid)
        ++d->output_native_ticks;
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP ||
        d->monitor.ep_fifo_frame_valid)
        d->fallback_limiter_gain = 1.0;
    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP &&
        !d->monitor.ep_fifo_frame_valid) {
        if (d->trace_output) ++d->output_dry_ticks;
        bool wide = mcpx_apu_mixdown_all();
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* DirectSound speaker bins are FL/FR/C/LFE/BL/BR (0..5);
             * 6..9 are the 3D front/back L/R submixes. Keep their handedness
             * and fold the two non-directional channels equally into stereo.
             *
             * 10 is the I3DL2 reverb send, 11..30 are effect sends and 31 is
             * a submix input. They are inputs to DSP processing, not extra
             * speakers: summing them by parity made a right-hand source's
             * reverb send play dry on the left, duplicated voices and caused
             * premature clipping. A title can explicitly designate a custom
             * stereo input pair for its dry fallback. Other DSP inputs stay
             * excluded, and native EP PCM remains authoritative above. */
            float left, right;
            if (wide) {
                const float center_gain = 0.7071067811865476f;
                float center = center_gain * (mixbins[2][i] + mixbins[3][i]);
                left = mixbins[0][i] + mixbins[4][i] +
                       mixbins[6][i] + mixbins[8][i] + center;
                right = mixbins[1][i] + mixbins[5][i] +
                        mixbins[7][i] + mixbins[9][i] + center;
                if (fallback_pair) {
                    left += mixbins[(fallback_pair & 0xffu) - 1u][i];
                    right += mixbins[((fallback_pair >> 8) & 0xffu) - 1u][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            /* Peak limiting belongs after the complete dry sum and before
             * PCM quantization. Link both channels so overlap peaks retain
             * their stereo image. Quiet audio has unity gain until an actual
             * overload; attack is immediate and release has a 100 ms time
             * constant at 48 kHz. Retain gain across 32-sample ticks without
             * buffering or adding latency. Native EP/VP output bypasses it. */
            if (!isfinite(left)) left = 0.0f;
            if (!isfinite(right)) right = 0.0f;
            double peak = fmax(fabs((double)left), fabs((double)right));
            double gain = d->fallback_limiter_gain;
            if (!isfinite(gain) || gain <= 0.0 || gain > 1.0) gain = 1.0;
            if (d->trace_output) {
                if (left > 1.0f || left < -1.0f) ++d->output_overloaded_samples;
                if (right > 1.0f || right < -1.0f) ++d->output_overloaded_samples;
            }
            const double release = 0.9997916883665486; /* exp(-1 / 4800) */
            double required = peak > 1.0 ? 1.0 / peak : 1.0;
            gain = fmin(required, 1.0 - (1.0 - gain) * release);
            d->fallback_limiter_gain = gain;
            if (d->trace_output && gain < 1.0) {
                d->output_limited_samples += left != 0.0f;
                d->output_limited_samples += right != 0.0f;
            }
            left = (float)((double)left * gain);
            right = (float)((double)right * gain);
            /* Retain a final safety clamp for floating-point roundoff. The
             * clipped counter measures only post-limit hard saturation. */
            if (d->trace_output) {
                if (left > 1.0f || left < -1.0f) ++d->output_clipped_samples;
                if (right > 1.0f || right < -1.0f) ++d->output_clipped_samples;
            }
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

}
