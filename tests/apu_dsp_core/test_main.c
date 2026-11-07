#include "apu_dsp_engine.h"
#include "apu_state.h"
#include "apu_dma_regs.h"
#include "apu_dsp_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSP56300_TEST_RAM_WORDS 4096u
#define DSP_TEST_START_FRAME (1u << 1)

static int check_dsp_trace_event_flags(void)
{
    if (!mcpx_dsp_trace_sample_due(1) ||
        !mcpx_dsp_trace_sample_due(60) ||
        !mcpx_dsp_trace_sample_due(120) ||
        mcpx_dsp_trace_sample_due(2) ||
        mcpx_dsp_trace_sample_due(59) ||
        mcpx_dsp_trace_sample_due(61)) return 8;
    if (!mcpx_dsp_trace_audio_sample_due(1) ||
        !mcpx_dsp_trace_audio_sample_due(188) ||
        mcpx_dsp_trace_audio_sample_due(2) ||
        mcpx_dsp_trace_audio_sample_due(187)) return 9;
    if (mcpx_dsp_trace_events(0, 0, 0) != 0) return 1;
    if (mcpx_dsp_trace_events(1u << 20, 0, 0) != MCPX_DSP_TRACE_SEN) return 2;
    if (mcpx_dsp_trace_events(0, 1u << 4, 0) != MCPX_DSP_TRACE_STACK_OVERFLOW) return 3;
    if (mcpx_dsp_trace_events(0, 1u << 5, 0) != MCPX_DSP_TRACE_STACK_UNDERFLOW) return 4;
    if (mcpx_dsp_trace_events(0, 0, 1u << 1) != MCPX_DSP_TRACE_STACK_IRQ) return 5;
    if (mcpx_dsp_trace_events((1u << 20), (1u << 4) | (1u << 5), 1u << 1) !=
        (MCPX_DSP_TRACE_SEN | MCPX_DSP_TRACE_STACK_OVERFLOW |
         MCPX_DSP_TRACE_STACK_UNDERFLOW | MCPX_DSP_TRACE_STACK_IRQ)) return 6;
    if (mcpx_dsp_trace_events(0, 0, 1u << 2) != 0) return 7;
    return 0;
}

int test_dsp_dma_transfer(void);

void xbox_SetAckTick(void (*fn)(void))
{
    (void)fn;
}

typedef struct TestPeripheral {
    uint32_t address;
    uint32_t value;
    unsigned reads;
    unsigned writes;
} TestPeripheral;

static uint32_t peripheral_read(void *opaque, uint32_t address)
{
    TestPeripheral *peripheral = (TestPeripheral *)opaque;
    peripheral->address = address;
    peripheral->reads++;
    return peripheral->value;
}

static void peripheral_write(void *opaque, uint32_t address, uint32_t value)
{
    TestPeripheral *peripheral = (TestPeripheral *)opaque;
    peripheral->address = address;
    peripheral->value = value;
    peripheral->writes++;
}

static int check_pc_and_p_write_invalidation(void)
{
    uint32_t x[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t y[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t p[DSP56300_TEST_RAM_WORDS] = {0};
    McpxDspEngine dsp;
    uint32_t pc = 0;

    p[0] = 0x0C0100; /* JMP $100 */
    if (!mcpx_dsp_engine_init(&dsp, x, DSP56300_TEST_RAM_WORDS,
                              y, DSP56300_TEST_RAM_WORDS,
                              p, DSP56300_TEST_RAM_WORDS))
        return 1;
    mcpx_dsp_engine_step(&dsp);
    if (!mcpx_dsp_engine_get_pc(&dsp, &pc) || pc != 0x100) {
        mcpx_dsp_engine_destroy(&dsp);
        return 2;
    }

    /* Replacing translated program memory must invalidate the JIT block. */
    mcpx_dsp_engine_write(&dsp, MCPX_DSP_SPACE_P, 0, 0x0C0200);
    mcpx_dsp_engine_reset(&dsp);
    mcpx_dsp_engine_step(&dsp);
    if (!mcpx_dsp_engine_get_pc(&dsp, &pc) || pc != 0x200) {
        mcpx_dsp_engine_destroy(&dsp);
        return 3;
    }
    mcpx_dsp_engine_destroy(&dsp);
    return 0;
}

static int check_memory_spaces_and_instances_are_independent(void)
{
    uint32_t x0[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t y0[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t p0[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t x1[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t y1[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t p1[DSP56300_TEST_RAM_WORDS] = {0};
    McpxDspEngine a, b;
    uint32_t value = 0;
    if (!mcpx_dsp_engine_init(&a, x0, DSP56300_TEST_RAM_WORDS,
                              y0, DSP56300_TEST_RAM_WORDS,
                              p0, DSP56300_TEST_RAM_WORDS) ||
        !mcpx_dsp_engine_init(&b, x1, DSP56300_TEST_RAM_WORDS,
                              y1, DSP56300_TEST_RAM_WORDS,
                              p1, DSP56300_TEST_RAM_WORDS))
        return 1;

    mcpx_dsp_engine_write(&a, MCPX_DSP_SPACE_X, 12, 0x123456);
    mcpx_dsp_engine_write(&a, MCPX_DSP_SPACE_Y, 12, 0x654321);
    if (!mcpx_dsp_engine_read(&a, MCPX_DSP_SPACE_X, 12, &value) ||
        value != 0x123456 ||
        !mcpx_dsp_engine_read(&a, MCPX_DSP_SPACE_Y, 12, &value) ||
        value != 0x654321 ||
        !mcpx_dsp_engine_read(&b, MCPX_DSP_SPACE_X, 12, &value) || value != 0) {
        mcpx_dsp_engine_destroy(&b);
        mcpx_dsp_engine_destroy(&a);
        return 2;
    }
    mcpx_dsp_engine_destroy(&b);
    mcpx_dsp_engine_destroy(&a);
    return 0;
}

static int check_peripheral_callbacks(void)
{
    uint32_t x[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t y[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t p[DSP56300_TEST_RAM_WORDS] = {0};
    TestPeripheral peripheral = {0};
    Dsp56300CallbackRegion callback = {
        .opaque = &peripheral,
        .read = peripheral_read,
        .write = peripheral_write
    };
    McpxDspEngine dsp;
    uint32_t value = 0;

    if (!mcpx_dsp_engine_init_with_peripherals(
            &dsp, x, DSP56300_TEST_RAM_WORDS,
            y, DSP56300_TEST_RAM_WORDS,
            p, DSP56300_TEST_RAM_WORDS,
            0xFFFF80, 0x1000000, &callback, &callback))
        return 1;
    peripheral.value = 0x00ABCDEF;
    if (!mcpx_dsp_engine_read(&dsp, MCPX_DSP_SPACE_X, 0xFFFFB0, &value) ||
        value != 0x00ABCDEF || peripheral.address != 0xFFFFB0 ||
        peripheral.reads != 1) {
        mcpx_dsp_engine_destroy(&dsp);
        return 2;
    }
    if (!mcpx_dsp_engine_write(&dsp, MCPX_DSP_SPACE_Y, 0xFFFFC4, 0x123456) ||
        peripheral.address != 0xFFFFC4 || peripheral.value != 0x123456 ||
        peripheral.writes != 1) {
        mcpx_dsp_engine_destroy(&dsp);
        return 3;
    }
    mcpx_dsp_engine_destroy(&dsp);
    return 0;
}

static int check_mixbuffer_alias(void)
{
    uint32_t x[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t y[DSP56300_TEST_RAM_WORDS] = {0};
    uint32_t p[DSP56300_TEST_RAM_WORDS] = {0};
    Dsp56300MemoryRegion x_regions[2] = {
        {
            .start = 0, .end = DSP56300_TEST_RAM_WORDS,
            .kind = DSP56300_REGION_BUFFER,
            .data = { .buffer = { .base = x, .offset = 0 } }
        },
        {
            .start = 0x1400, .end = 0x1800,
            .kind = DSP56300_REGION_BUFFER,
            .data = { .buffer = { .base = x, .offset = 0xC00 } }
        }
    };
    Dsp56300MemoryRegion y_regions[1] = {{
        .start = 0, .end = DSP56300_TEST_RAM_WORDS,
        .kind = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = y, .offset = 0 } }
    }};
    Dsp56300MemoryRegion p_regions[1] = {{
        .start = 0, .end = DSP56300_TEST_RAM_WORDS,
        .kind = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = p, .offset = 0 } }
    }};
    Dsp56300MemoryMap map = {
        .x_regions = x_regions, .x_count = 2,
        .y_regions = y_regions, .y_count = 1,
        .p_regions = p_regions, .p_count = 1
    };
    McpxDspEngine dsp;
    uint32_t value = 0;

    if (!mcpx_dsp_engine_init_with_map(&dsp, &map)) return 1;
    if (!mcpx_dsp_engine_write(&dsp, MCPX_DSP_SPACE_X, 0x1407, 0xABCD01) ||
        x[0xC07] != 0xABCD01 ||
        !mcpx_dsp_engine_read(&dsp, MCPX_DSP_SPACE_X, 0x1407, &value) ||
        value != 0xABCD01) {
        mcpx_dsp_engine_destroy(&dsp);
        return 2;
    }
    mcpx_dsp_engine_destroy(&dsp);
    return 0;
}

/* A program polling for the frame start leaves the frame early, without
 * looking halted; one that sees the bit moves on and keeps its whole budget. */
static int check_status_poll_leaves_the_frame(void)
{
    MCPXAPUState *apu = (MCPXAPUState *)calloc(1, sizeof(*apu));
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    int result = 0;
    if (!apu) return 1;

    mcpx_apu_dsp_init(apu);
    if (!apu->gp.dsp || !apu->gp.dsp->engine_ready) {
        result = 2;
        goto done;
    }
    apu->gp.dsp->core.pram[0] = 0x0A8581; /* JCLR #1,X:$FFFFC5,$0 */
    apu->gp.dsp->core.pram[1] = 0;
    apu->gp.dsp->core.pram[2] = 0x0C0002; /* JMP $2 */
    apu->gp.regs[NV_PAPU_GPRST] =
        NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    mcpx_apu_dsp_frame(apu, mixbins);
    if (apu->gp.dsp->core.is_idle || apu->gp.dsp->core.pc > 1 ||
        apu->gp.dsp->core.cycle_count == 0 ||
        apu->gp.dsp->core.cycle_count >= 8000u) {
        fprintf(stderr, "Polling frame: idle %d, PC %X, %u cycles\n",
                (int)apu->gp.dsp->core.is_idle, apu->gp.dsp->core.pc,
                apu->gp.dsp->core.cycle_count);
        result = 3;
        goto done;
    }
    apu->gp.dsp->interrupts |= DSP_TEST_START_FRAME;
    mcpx_apu_dsp_frame(apu, mixbins);
    if (apu->gp.dsp->core.pc != 2 ||
        apu->gp.dsp->core.cycle_count < 88000u) {
        fprintf(stderr, "Frame after the start bit: PC %X, %u cycles\n",
                apu->gp.dsp->core.pc, apu->gp.dsp->core.cycle_count);
        result = 4;
    }

done:
    mcpx_apu_dsp_finalize(apu);
    free(apu);
    return result;
}

static int check_apu_frame_runs_enabled_core(void)
{
    MCPXAPUState *apu = (MCPXAPUState *)calloc(1, sizeof(*apu));
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    int result = 0;
    if (!apu) return 1;

    mcpx_apu_dsp_init(apu);
    if (!apu->gp.dsp || !apu->gp.dsp->engine_ready) {
        result = 2;
        goto done;
    }
    apu->gp.dsp->core.pram[0] = 0x0C0000; /* JMP $0 */
    apu->gp.regs[NV_PAPU_GPRST] =
        NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    mcpx_apu_dsp_frame(apu, mixbins);
    if (apu->gp.dsp->core.pc != 0 ||
        apu->gp.dsp->core.cycle_count == 0) {
        result = 3;
        goto done;
    }

    /* A short halted frame must not donate its unused execution budget
     * to the following busy frame. HALT leaves PC at the JMP $2 loop. */
    mcpx_dsp_engine_reset(&apu->gp.dsp->engine);
    apu->gp.dsp->core.pram[0] = 0x08F484; /* MOVEP #1,X:$FFFFC4 */
    apu->gp.dsp->core.pram[1] = 1;
    apu->gp.dsp->core.pram[2] = 0x0C0002;
    mcpx_apu_dsp_frame(apu, mixbins);
    if (!apu->gp.dsp->core.is_idle ||
        apu->gp.dsp->core.cycle_count >= 88875u) {
        result = 4;
        goto done;
    }
    mcpx_apu_dsp_frame(apu, mixbins);
    if (apu->gp.dsp->core.cycle_count < 88870u ||
        apu->gp.dsp->core.cycle_count > 88880u) {
        fprintf(stderr, "Busy GP frame consumed %u cycles\n",
                apu->gp.dsp->core.cycle_count);
        result = 5;
        goto done;
    }

done:
    mcpx_apu_dsp_finalize(apu);
    free(apu);
    return result;
}

static int check_apu_dma_sge_bounds(void)
{
    MCPXAPUState *apu = (MCPXAPUState *)calloc(1, sizeof(*apu));
    DSPState *dsp;
    uint32_t page = 0x2000;
    int result = 0;
    if (!apu) return 1;
    apu->ram_size = 0x4000;
    apu->ram_ptr = (uint8_t *)calloc(1, apu->ram_size);
    if (!apu->ram_ptr) { free(apu); return 2; }
    mcpx_apu_dsp_init(apu);
    dsp = apu->gp.dsp;
    if (!dsp || !dsp->engine_ready) { result = 3; goto done; }

    apu->regs[NV_PAPU_GPSADDR] = 0x100;
    apu->regs[NV_PAPU_GPSMAXSGE] = 0;
    memcpy(apu->ram_ptr + 0x100, &page, sizeof(page));
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 0, NODE_POINTER_EOL);
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 1,
        (1u << 1) | (0xFu << 5) | (2u << 10) | (1u << 14));
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 2, 1);
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 3, 0x10);
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 4, 0);
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 5, 0);
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 6, 0xFFF);
    mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_X, 0x10, 0x123456);
    dsp_dma_write(&dsp->dma, DMA_NEXT_BLOCK, 0);
    dsp_dma_write(&dsp->dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    if (apu->ram_ptr[0x2000] != 0 || apu->ram_ptr[0x2001] != 0x56 ||
        apu->ram_ptr[0x2002] != 0x34 || apu->ram_ptr[0x2003] != 0x12 ||
        dsp->dma.error) { result = 4; goto done; }

    /* A physical page overlapping the backing boundary is rejected without
     * wrapping, and reports a DMA error rather than unpacking old bytes. */
    dsp_dma_reset(&dsp->dma);
    page = 0x4000;
    memcpy(apu->ram_ptr + 0x100, &page, sizeof(page));
    dsp_dma_write(&dsp->dma, DMA_NEXT_BLOCK, 0);
    dsp_dma_write(&dsp->dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    if (!dsp->dma.error ||
        !(dsp->dma.pending_interrupts & DMA_INTERRUPT_ERROR) ||
        apu->ram_ptr[0x3FFF] != 0) result = 5;

done:
    mcpx_apu_dsp_finalize(apu);
    free(apu->ram_ptr);
    free(apu);
    return result;
}

static int check_ep_fifo_survives_frame_mixdown(void)
{
    MCPXAPUState *apu = (MCPXAPUState *)calloc(1, sizeof(*apu));
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    int16_t expected[256][2];
    uint32_t page = 0x2000;
    int result = 0;
    if (!apu) return 1;
    apu->ram_size = 0x4000;
    apu->ram_ptr = (uint8_t *)calloc(1, apu->ram_size);
    if (!apu->ram_ptr) { free(apu); return 2; }
    mcpx_apu_dsp_init(apu);
    DSPState *ep = apu->ep.dsp;
    if (!ep || !ep->engine_ready) { result = 3; goto done; }

    apu->monitor.point = MCPX_APU_DEBUG_MON_EP;
    apu->regs[NV_PAPU_EPFADDR] = 0x100;
    apu->regs[NV_PAPU_EPFMAXSGE] = 0;
    apu->regs[NV_PAPU_EPOFBASE0] = 0;
    apu->regs[NV_PAPU_EPOFEND0] = 0x800;
    memcpy(apu->ram_ptr + 0x100, &page, sizeof(page));
    ep->core.xram[0] = NODE_POINTER_EOL;
    ep->core.xram[1] = NODE_CONTROL_DIRECTION | (1u << 10) | (1u << 14);
    ep->core.xram[2] = 512;
    ep->core.xram[3] = 0x100;
    for (unsigned i = 0; i < 256; ++i) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            expected[i][ch] = (int16_t)(1000 + 2 * i + ch);
            ep->core.xram[0x100 + 2 * i + ch] =
                (uint32_t)(uint16_t)expected[i][ch] << 8;
        }
    }
    for (unsigned i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        mixbins[0][i] = 0.25f;
        mixbins[1][i] = -0.25f;
    }
    /* Execute the real peripheral DMA kick from an enabled EP frame:
     * MOVEP #0,X:$FFFFD4; MOVEP #START,X:$FFFFD6; HALT; JMP $6.
     * The block differs from fallback mixbins, so a lost native slice is
     * observable even if all later ticks keep producing fallback audio. */
    ep->core.pram[0] = 0x08F494;
    ep->core.pram[1] = 0;
    ep->core.pram[2] = 0x08F496;
    ep->core.pram[3] = DMA_CONTROL_ACTION_START;
    ep->core.pram[4] = 0x08F484;
    ep->core.pram[5] = 1;
    ep->core.pram[6] = 0x0C0006;
    apu->ep.regs[NV_PAPU_EPRST] =
        NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    for (unsigned tick = 0; tick < 8; ++tick) {
        apu->ep_frame_div = tick;
        mcpx_apu_dsp_frame(apu, mixbins);
        if (memcmp(apu->monitor.frame_buf, expected, sizeof(expected))) {
            fprintf(stderr, "EP tick %u PC=%06X cycles=%u error=%u valid=%u"
                    " FIFOcur=%u sample=%d expected=%d\n", tick,
                    ep->core.pc, ep->core.cycle_count, ep->dma.error,
                    apu->monitor.ep_fifo_frame_valid,
                    apu->regs[NV_PAPU_EPOFCUR0],
                    apu->monitor.frame_buf[0][0], expected[0][0]);
            result = 4;
            goto done;
        }
        /* Native ownership lasts through the epoch even if execution stops. */
        apu->ep.regs[NV_PAPU_EPRST] = 0;
    }
    if (memcmp(apu->ram_ptr + page, expected, sizeof(expected))) {
        result = 5;
        goto done;
    }

    /* Consumption releases ownership: the next epoch can use fallback. */
    mcpx_apu_monitor_frame(apu);
    apu->ep_frame_div = 8;
    mcpx_apu_dsp_frame(apu, mixbins);
    for (unsigned i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        if (apu->monitor.frame_buf[i][0] != 8191 ||
            apu->monitor.frame_buf[i][1] != -8191) {
            result = 6;
            goto done;
        }
    }

    /* A capture on the final tick replaces earlier fallback slices as a
     * whole block too, including the default combined GP/EP monitor. */
    mcpx_dsp_engine_reset(&ep->engine);
    dsp_dma_reset(&ep->dma);
    apu->regs[NV_PAPU_EPOFCUR0] = 0;
    apu->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
    for (unsigned tick = 0; tick < 8; ++tick) {
        apu->ep_frame_div = 8 + tick;
        apu->ep.regs[NV_PAPU_EPRST] = tick == 7
            ? NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST : 0;
        mcpx_apu_dsp_frame(apu, mixbins);
    }
    if (memcmp(apu->monitor.frame_buf, expected, sizeof(expected))) {
        result = 8;
        goto done;
    }
    mcpx_apu_monitor_frame(apu);

    /* EP output cannot replace the user's VP debug monitoring buffer. */
    mcpx_dsp_engine_reset(&ep->engine);
    dsp_dma_reset(&ep->dma);
    apu->regs[NV_PAPU_EPOFCUR0] = 0;
    apu->ep.regs[NV_PAPU_EPRST] =
        NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    apu->monitor.point = MCPX_APU_DEBUG_MON_VP;
    memset(apu->monitor.frame_buf, 0x5A, sizeof(apu->monitor.frame_buf));
    apu->ep_frame_div = 16;
    mcpx_apu_dsp_frame(apu, mixbins);
    for (unsigned i = 0; i < 256; ++i) {
        if (apu->monitor.frame_buf[i][0] != 0x5A5A ||
            apu->monitor.frame_buf[i][1] != 0x5A5A) {
            result = 7;
            goto done;
        }
    }

done:
    mcpx_apu_dsp_finalize(apu);
    free(apu->ram_ptr);
    free(apu);
    return result;
}

static MCPXAPUState *ep_fifo_test_fixture(void)
{
    MCPXAPUState *apu = (MCPXAPUState *)calloc(1, sizeof(*apu));
    const uint32_t pages[4] = { 0x2000, 0, 0x4000, 0 };
    if (!apu) return NULL;
    apu->ram_size = 0x8000;
    apu->ram_ptr = (uint8_t *)calloc(1, apu->ram_size);
    if (!apu->ram_ptr) { free(apu); return NULL; }
    qemu_mutex_init(&apu->lock);
    mcpx_apu_dsp_init(apu);
    apu->trace_dsp_state = true;
    apu->monitor.point = MCPX_APU_DEBUG_MON_EP;
    apu->regs[NV_PAPU_EPFADDR] = 0x100;
    apu->regs[NV_PAPU_EPFMAXSGE] = 1;
    apu->regs[NV_PAPU_EPOFEND0] = 0x2000;
    /* Both reset-release bootstraps need two valid 4096-byte SGE pages. */
    apu->regs[NV_PAPU_EPSADDR] = apu->regs[NV_PAPU_GPSADDR] = 0x100;
    apu->regs[NV_PAPU_EPSMAXSGE] = apu->regs[NV_PAPU_GPSMAXSGE] = 1;
    memcpy(apu->ram_ptr + 0x100, pages, sizeof(pages));
    return apu;
}

static void ep_fifo_test_destroy(MCPXAPUState *apu)
{
    mcpx_apu_dsp_finalize(apu);
    qemu_mutex_destroy(&apu->lock);
    free(apu->ram_ptr);
    free(apu);
}

static void ep_fifo_expected_pcm(int16_t expected[256][2], unsigned first)
{
    for (unsigned i = 0; i < 256; ++i)
        for (unsigned ch = 0; ch < 2; ++ch)
            expected[i][ch] = (int16_t)(1000 + first + 2 * i + ch);
}

static int ep_fifo_expect(int condition, const char *name)
{
    if (!condition) fprintf(stderr, "EP FIFO regression: %s\n", name);
    return !condition;
}

static int write_ep_fifo_test_chunk(DSPState *ep, unsigned first,
                                    unsigned bytes)
{
    uint32_t descriptor[7] = {
        NODE_POINTER_EOL,
        NODE_CONTROL_DIRECTION | (1u << 10) | (1u << 14),
        bytes / sizeof(int16_t),
        0x100,
        0,
        0,
        0xFFF
    };
    memcpy(ep->core.xram, descriptor, sizeof(descriptor));
    /* Format 1 emits the independent PCM ramp, rather than comparing only
     * against guest RAM written by this same DMA implementation. */
    for (unsigned i = 0; i < bytes / sizeof(int16_t); ++i)
        ep->core.xram[0x100 + i] =
            (uint32_t)(uint16_t)(1000 + first + i) << 8;
    dsp_dma_write(&ep->dma, DMA_NEXT_BLOCK, 0);
    dsp_dma_write(&ep->dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    if (ep->dma.error) return 1;
    dsp_dma_completion_seen(&ep->dma);
    return 0;
}

static int check_ep_fifo_partial_blocks_are_assembled(void)
{
    MCPXAPUState *apu = ep_fifo_test_fixture();
    DSPState *ep;
    int16_t expected[256][2];
    int result = 0;

    if (!apu) return 1;
    ep = apu->ep.dsp;
    if (!ep || !ep->engine_ready) { result = 3; goto done; }
    for (unsigned chunk = 0; chunk < 7; ++chunk) {
        if (write_ep_fifo_test_chunk(ep, chunk * 64, 128)) { result = 4; goto done; }
    }
    if (ep->ep_fifo_frame_ready) { result = 5; goto done; }
    apu->ep_frame_div = 7;
    mcpx_apu_monitor_frame(apu);
    if (ep->ep_fifo_frame_bytes_used != 0) { result = 6; goto done; }

    for (unsigned chunk = 7; chunk < 15; ++chunk) {
        if (write_ep_fifo_test_chunk(ep, chunk * 64, 128)) { result = 7; goto done; }
    }
    if (!ep->ep_fifo_frame_ready || !apu->monitor.ep_fifo_frame_valid) {
        result = 8;
        goto done;
    }
    ep_fifo_expected_pcm(expected, 7 * 64);
    if (memcmp(apu->monitor.frame_buf, expected, sizeof(expected))) {
        fprintf(stderr, "EP fragment mismatch: frame=%d,%d captured=%d,%d expected=%d,%d cur=%u used=%zu\n",
                apu->monitor.frame_buf[0][0], apu->monitor.frame_buf[0][1],
                ep->ep_fifo_frame_buf[0][0], ep->ep_fifo_frame_buf[0][1],
                expected[0][0], expected[0][1], apu->regs[NV_PAPU_EPOFCUR0],
                ep->ep_fifo_frame_bytes_used);
        result = 9;
        goto done;
    }
    apu->ep_frame_div = 7;
    mcpx_apu_monitor_frame(apu);
    if (ep->ep_fifo_frame_ready || apu->monitor.ep_fifo_frame_valid) {
        result = 10;
        goto done;
    }

    for (unsigned chunk = 15; chunk < 22; ++chunk) {
        if (write_ep_fifo_test_chunk(ep, chunk * 64, 128)) { result = 11; goto done; }
    }
    apu->ep_frame_div = 7;
    mcpx_apu_monitor_frame(apu);
    if (ep->ep_fifo_frame_ready || ep->ep_fifo_frame_bytes_used != 0)
        result = 12;
    if (apu->dsp_trace_ep_fifo_writes != 22 ||
        apu->dsp_trace_ep_fifo_bytes != 22 * 128 ||
        apu->dsp_trace_ep_fifo_captured != 1)
        result = 13;

done:
    ep_fifo_test_destroy(apu);
    return result;
}

static int check_ep_fifo_descriptor_splits_preserve_pcm(void)
{
    static const struct {
        const char *name;
        unsigned count, chunks[12];
    } cases[] = {
        { "768+512", 2, {768, 512} },
        { "768+256+256", 3, {768, 256, 256} },
        { "1024+256", 2, {1024, 256} },
        { "1280 single", 1, {1280} },
        { "12x128", 12, {128,128,128,128,128,128,128,128,128,128,128,128} }
    };
    const McpxApuDebugMonitorPoint points[] = {
        MCPX_APU_DEBUG_MON_EP, MCPX_APU_DEBUG_MON_GP_OR_EP
    };
    const int16_t silence[256][2] = {{0}};
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    int failures = 0;
    for (unsigned monitor = 0; monitor < 2; ++monitor) {
        for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            MCPXAPUState *apu = ep_fifo_test_fixture();
            int16_t expected[256][2];
            unsigned bytes = 0;
            if (!apu) return 1;
            DSPState *ep = apu->ep.dsp;
            if (!ep || !ep->engine_ready) { ep_fifo_test_destroy(apu); return 2; }
            apu->monitor.point = points[monitor];
            ep_fifo_expected_pcm(expected, 0);
            for (unsigned i = 0; i < cases[c].count; ++i) {
                failures += ep_fifo_expect(
                    !write_ep_fifo_test_chunk(ep, bytes / 2, cases[c].chunks[i]),
                    "descriptor-split DMA succeeds");
                bytes += cases[c].chunks[i];
            }
            int correct = apu->monitor.ep_fifo_frame_valid && ep->ep_fifo_frame_ready &&
                ep->ep_fifo_frame_bytes_used == sizeof(expected) &&
                !memcmp(apu->monitor.frame_buf, expected, sizeof(expected)) &&
                !memcmp(ep->ep_fifo_frame_buf, expected, sizeof(expected));
            if (!correct) fprintf(stderr,
                "EP split %s monitor=%u frame-first=%d collector-first=%d expected=%d\n",
                cases[c].name, (unsigned)points[monitor], apu->monitor.frame_buf[0][0],
                ep->ep_fifo_frame_buf[0][0], expected[0][0]);
            failures += ep_fifo_expect(correct,
                "first 1024 PCM bytes are independent of descriptor splits and overflow");
            failures += ep_fifo_expect(apu->dsp_trace_ep_fifo_writes == cases[c].count &&
                apu->dsp_trace_ep_fifo_bytes == bytes && apu->dsp_trace_ep_fifo_captured == 1,
                "extra epoch bytes are counted without publishing another block");
            apu->ep_frame_div = 7;
            mcpx_apu_monitor_frame(apu);
            failures += ep_fifo_expect(!ep->ep_fifo_frame_ready &&
                ep->ep_fifo_frame_bytes_used == 0 && !apu->monitor.ep_fifo_frame_valid &&
                !memcmp(apu->monitor.frame_buf, silence, sizeof(silence)),
                "consumed monitor epoch releases and clears native PCM");
            apu->ep_frame_div = 8;
            mcpx_apu_dsp_frame(apu, mixbins);
            failures += ep_fifo_expect(!apu->monitor.ep_fifo_frame_valid &&
                !memcmp(apu->monitor.frame_buf, silence, sizeof(silence)),
                "next empty epoch uses silent fallback without stale native PCM");
            ep_fifo_test_destroy(apu);
        }
    }
    return failures;
}

static int check_ep_fifo_reset_drops_old_pcm(int complete)
{
    MCPXAPUState *apu = ep_fifo_test_fixture();
    int16_t expected[256][2];
    const int16_t silence[256][2] = {{0}};
    unsigned old_bytes = complete ? 1024 : 512;
    uint32_t boot_word;
    int failures = 0;
    if (!apu) return 1;
    DSPState *ep = apu->ep.dsp;
    if (!ep || !ep->engine_ready) { ep_fifo_test_destroy(apu); return 2; }
    failures += ep_fifo_expect(!write_ep_fifo_test_chunk(ep, 0, old_bytes),
                               "pre-reset EP FIFO DMA succeeds");
    ep_fifo_expected_pcm(expected, 0);
    /* Resetting GP must not discard an independent EP partial/full block. */
    mcpx_apu_dsp_mmio_write(apu, NV_PAPU_GPRST, 0, 4, true);
    mcpx_apu_dsp_mmio_write(apu, NV_PAPU_GPRST, 3, 4, true);
    failures += ep_fifo_expect(ep->ep_fifo_frame_bytes_used == old_bytes &&
        ep->ep_fifo_frame_ready == (complete != 0) &&
        apu->monitor.ep_fifo_frame_valid == (complete != 0) &&
        (!complete || !memcmp(apu->monitor.frame_buf, expected, sizeof(expected))),
        "GP reset-release preserves EP assembler and monitor ownership");

    mcpx_apu_dsp_mmio_write(apu, NV_PAPU_EPRST, 0, 4, false);
    if (ep->ep_fifo_frame_bytes_used || ep->ep_fifo_frame_ready ||
        apu->monitor.ep_fifo_frame_valid)
        fprintf(stderr, "EP reset %s left used=%zu ready=%u valid=%u\n",
            complete ? "complete" : "partial", ep->ep_fifo_frame_bytes_used,
            ep->ep_fifo_frame_ready, apu->monitor.ep_fifo_frame_valid);
    failures += ep_fifo_expect(!ep->ep_fifo_frame_bytes_used &&
        !ep->ep_fifo_frame_ready && !apu->monitor.ep_fifo_frame_valid,
        "EP reset assertion discards collector bytes and native ownership");
    failures += ep_fifo_expect(!memcmp(apu->monitor.frame_buf, silence, sizeof(silence)),
                               "EP reset cannot leave discarded native PCM in the monitor");
    memcpy(&boot_word, apu->ram_ptr + 0x2000, sizeof(boot_word));
    mcpx_apu_dsp_mmio_write(apu, NV_PAPU_EPRST, 3, 4, false);
    failures += ep_fifo_expect(apu->ep.regs[NV_PAPU_EPRST] == 3 &&
        ep->core.pram[0] == (boot_word & 0x00FFFFFFu),
        "EP reset-release successfully bootstraps through valid EPS SGE pages");
    failures += ep_fifo_expect(!ep->ep_fifo_frame_bytes_used &&
        !ep->ep_fifo_frame_ready && !apu->monitor.ep_fifo_frame_valid,
        "EP reset-release retains an empty collector");
    if (complete) {
        failures += ep_fifo_expect(!write_ep_fifo_test_chunk(ep, 1024, 1024),
                                   "fresh full block after reset succeeds");
    } else {
        failures += ep_fifo_expect(!write_ep_fifo_test_chunk(ep, 1024, 512),
                                   "fresh first fragment after reset succeeds");
        failures += ep_fifo_expect(ep->ep_fifo_frame_bytes_used == 512 &&
            !ep->ep_fifo_frame_ready && !apu->monitor.ep_fifo_frame_valid,
            "post-reset partial block cannot combine with pre-reset bytes");
        failures += ep_fifo_expect(!write_ep_fifo_test_chunk(ep, 1280, 512),
                                   "fresh second fragment after reset succeeds");
    }
    ep_fifo_expected_pcm(expected, 1024);
    if (memcmp(apu->monitor.frame_buf, expected, sizeof(expected)))
        fprintf(stderr, "EP reset %s fresh frame-first=%d expected=%d\n",
            complete ? "complete" : "partial", apu->monitor.frame_buf[0][0], expected[0][0]);
    failures += ep_fifo_expect(ep->ep_fifo_frame_ready && apu->monitor.ep_fifo_frame_valid &&
        !memcmp(apu->monitor.frame_buf, expected, sizeof(expected)) &&
        !memcmp(ep->ep_fifo_frame_buf, expected, sizeof(expected)),
        "reset publishes fresh PCM containing only post-reset samples");
    failures += ep_fifo_expect(apu->dsp_trace_ep_fifo_writes == (complete ? 2 : 3) &&
        apu->dsp_trace_ep_fifo_bytes == old_bytes + 1024 &&
        apu->dsp_trace_ep_fifo_captured == (complete ? 2 : 1),
        "reset preserves cumulative write counters and counts fresh captures");
    apu->ep_frame_div = 7;
    mcpx_apu_monitor_frame(apu);
    failures += ep_fifo_expect(!ep->ep_fifo_frame_ready && !ep->ep_fifo_frame_bytes_used &&
        !apu->monitor.ep_fifo_frame_valid &&
        !memcmp(apu->monitor.frame_buf, silence, sizeof(silence)),
        "post-reset consumption leaves no stale native frame");
    ep_fifo_test_destroy(apu);
    return failures;
}

int main(void)
{
    int result = check_dsp_trace_event_flags();
    if (result) {
        fprintf(stderr, "DSP trace event test failed (%d)\n", result);
        return result;
    }
    result = check_pc_and_p_write_invalidation();
    if (result) {
        fprintf(stderr, "DSP PC/P-memory test failed (%d)\n", result);
        return result;
    }
    result = check_memory_spaces_and_instances_are_independent();
    if (result) {
        fprintf(stderr, "DSP memory isolation test failed (%d)\n", result);
        return result + 10;
    }
    result = check_peripheral_callbacks();
    if (result) {
        fprintf(stderr, "DSP peripheral callback test failed (%d)\n", result);
        return result + 20;
    }
    result = check_mixbuffer_alias();
    if (result) {
        fprintf(stderr, "DSP mixbuffer alias test failed (%d)\n", result);
        return result + 30;
    }
    result = check_apu_frame_runs_enabled_core();
    if (result) {
        fprintf(stderr, "APU DSP frame scheduling test failed (%d)\n", result);
        return result + 40;
    }
    result = check_status_poll_leaves_the_frame();
    if (result) {
        fprintf(stderr, "DSP status poll test failed (%d)\n", result);
        return result + 45;
    }
    result = test_dsp_dma_transfer();
    if (result) {
        fprintf(stderr, "DSP DMA transfer test failed (%d)\n", result);
        return result + 50;
    }
    result = check_apu_dma_sge_bounds();
    if (result) {
        fprintf(stderr, "APU DMA SGE bounds test failed (%d)\n", result);
        return result + 60;
    }
    result = check_ep_fifo_survives_frame_mixdown();
    if (result) {
        fprintf(stderr, "EP FIFO output ownership test failed (%d)\n", result);
        return result + 70;
    }
    result = check_ep_fifo_partial_blocks_are_assembled();
    if (result) {
        fprintf(stderr, "EP FIFO partial assembly test failed (%d)\n", result);
        return result + 80;
    }
    /* Run all three new arms even on failure, so a negative control proves
     * descriptor independence and each reset boundary separately. */
    int ep_failures = 0;
    result = check_ep_fifo_descriptor_splits_preserve_pcm();
    if (result) {
        fprintf(stderr, "EP FIFO descriptor-split test failed (%d)\n", result);
        ep_failures |= 1;
    }
    result = check_ep_fifo_reset_drops_old_pcm(0);
    if (result) {
        fprintf(stderr, "EP FIFO partial reset test failed (%d)\n", result);
        ep_failures |= 2;
    }
    result = check_ep_fifo_reset_drops_old_pcm(1);
    if (result) {
        fprintf(stderr, "EP FIFO complete reset test failed (%d)\n", result);
        ep_failures |= 4;
    }
    if (ep_failures) return 90 + ep_failures;
    puts("DSP core C API tests passed");
    return 0;
}
