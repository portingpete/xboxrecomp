/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "apu_dsp_trace.h"
#include "apu_xaudio2.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling
 *
 * xemu asserts the device's PCI line; here the line is whatever the runtime
 * connected with mcpx_apu_set_irq_line.
 * ============================================================ */

static void (*s_irq_line)(int asserted);

void mcpx_apu_set_irq_line(void (*line)(int asserted))
{
    s_irq_line = line;
}

/* The runtime calls these after entering guest ISR IRQL and releases the
 * device before leaving that level. The opaque device must outlive delivery;
 * stop/unregister service callbacks before shutting it down. The device mutex
 * is recursive, so the handler's own MMIO and VOICE_LOCK calls remain valid. */
void mcpx_apu_irq_service_begin(void *opaque)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    if (d) mcpx_apu_lock(d);
}

/* The same without waiting: the frame thread keeps the lock across frames, and
 * a caller blocked here at interrupt level holds up every other device. */
int mcpx_apu_irq_service_try(void *opaque)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    return !d || TryEnterCriticalSection(&d->lock.cs);
}

void mcpx_apu_irq_service_end(void *opaque)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    if (d) qemu_mutex_unlock(&d->lock);
}

static void update_irq(MCPXAPUState *d)
{
    /* RECOMP_APU_TRACE_IRQ: each change of the status/enable pair the line
     * is computed from, for the first few hundred. */
    static int trace = -1;
    static uint32_t last_ists = ~0u, last_ien = ~0u;
    static unsigned said;
    if (trace < 0) {
        trace = getenv("RECOMP_APU_TRACE_IRQ") != NULL;
    }
    if (trace && said < 300 &&
        (d->regs[NV_PAPU_ISTS] != last_ists || d->regs[NV_PAPU_IEN] != last_ien)) {
        said++;
        last_ists = d->regs[NV_PAPU_ISTS];
        last_ien = d->regs[NV_PAPU_IEN];
        fprintf(stderr, "[APU] irq: ISTS %08X IEN %08X FECTL %08X\n",
                last_ists, last_ien, d->regs[NV_PAPU_FECTL]);
    }

    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        pci_irq_assert(PCI_DEVICE(d));
        if (s_irq_line) {
            s_irq_line(1);
        }
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        pci_irq_deassert(PCI_DEVICE(d));
        if (s_irq_line) {
            s_irq_line(0);
        }
    }
}

/* Caller holds d->lock. A VP continuation may wait for a voice that the
 * newly trapped guest ISR must unlock; publish before releasing that lock
 * for the wait rather than waiting for the complete frame to return. */
void mcpx_apu_publish_pending_irq(MCPXAPUState *d)
{
    if (d->set_irq) {
        update_irq(d);
        d->set_irq = false;
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

extern volatile LONG g_apu_pio_waiters;
void mcpx_apu_pio_drain(MCPXAPUState *d);

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    /* A register read is atomic and has no side effect: it must not wait for
     * the frame thread, which keeps d->lock across whole frames. */
    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    /* Ask for a hand-off, as PIO methods do, instead of waiting up to eight
     * frames for the next throttle. */
    InterlockedIncrement(&g_apu_pio_waiters);
    mcpx_apu_lock(d);
    InterlockedDecrement(&g_apu_pio_waiters);
    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
    qemu_mutex_unlock(&d->lock);
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

/* Aggregate EP blocks without changing their sample rate or chronology. */
static struct {
    int16_t samples[WAVEOUT_BUF_SAMPLES][2];
    int used;
    uint64_t waited_packets, timed_out_packets, dropped_samples, gated_waits;
} g_monitor_packet;

/* A one-shot software-mixer voice can finish between output packets. Flush
 * its final partial packet only after native production is gated. Submit one
 * silence packet to prime XA2 when it retained one buffer; a second silent
 * submission is allowed only to retry an observed failed Start. */
static struct {
    bool pending;
    bool prime_pending;
    bool start_retry_pending;
} g_mixer_output_tail;

static bool mixer_output_tail_pending(void)
{
    return g_mixer_output_tail.pending;
}

static void mixer_output_tail_reset(void)
{
    memset(&g_mixer_output_tail, 0, sizeof(g_mixer_output_tail));
}

static void mixer_output_tail_packet_submitted(int accepted, int xa2_active,
                                               unsigned queued_buffers,
                                               uint64_t start_failed_delta,
                                               int prime_packet)
{
    if (!g_mixer_output_tail.pending) return;
    if (!accepted || g_mixer_active_count || g_test_tone.active) {
        mixer_output_tail_reset();
        return;
    }
    /* xa2_submit_samples owns accepted PCM even when Start fails. A single
     * later silent submit retries Start on those already queued buffers; its
     * own error/acceptance ends this bounded recovery attempt. */
    if (start_failed_delta && !g_mixer_output_tail.start_retry_pending) {
        g_mixer_output_tail.prime_pending = true;
        g_mixer_output_tail.start_retry_pending = true;
        return;
    }
    if (g_mixer_output_tail.prime_pending) {
        /* A data-bearing packet that leaves one retained buffer still needs
         * a prime, even if the old buffer drained during submission. A
         * zero-only prime ends this bounded attempt. */
        if (xa2_active && !prime_packet && queued_buffers == 1) {
            g_mixer_output_tail.start_retry_pending = false;
            return;
        }
        mixer_output_tail_reset();
        return;
    }
    /* A zero queue means the accepted source packet has already drained. Do
     * not leave an unstarted silence buffer behind; only one retained packet
     * needs a second packet to start XA2. */
    if (xa2_active && queued_buffers == 1) {
        g_mixer_output_tail.prime_pending = true;
        return;
    }
    mixer_output_tail_reset();
}

/* A catch-up burst must not throw away chronological PCM just because the
 * healthy device still owns all three ring slots. Wait outside the APU lock
 * in short, interruptible slices. A stopped device cannot freeze gameplay. */
static int monitor_submit_xaudio2(MCPXAPUState *d, const int16_t *samples,
                                  int sample_count)
{
    const int64_t deadline = qemu_clock_get_us(QEMU_CLOCK_REALTIME) + 64000;
    bool waited = false;
    for (;;) {
        if (qatomic_read(&d->exiting) ||
            (d->pause_requested && !g_test_tone.active && !g_mixer_active_count &&
             !mixer_output_tail_pending())) {
            g_monitor_packet.dropped_samples += sample_count;
            return XA2_SUBMIT_ERROR;
        }
        int result = xa2_submit_samples(samples, sample_count);
        if (result != XA2_SUBMIT_BACKPRESSURE) {
            if (result != XA2_SUBMIT_ACCEPTED)
                g_monitor_packet.dropped_samples += sample_count;
            return result;
        }
        if (!waited) {
            ++g_monitor_packet.waited_packets;
            waited = true;
        }
        int64_t remaining = deadline - qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (remaining <= 0) {
            ++g_monitor_packet.timed_out_packets;
            g_monitor_packet.dropped_samples += sample_count;
            return XA2_SUBMIT_ERROR;
        }
        int wait_ms = remaining > 2000 ? 2 : (int)((remaining + 999) / 1000);
        qemu_cond_timedwait(&d->cond, &d->lock, wait_ms);
    }
}

static void monitor_submit_packet(MCPXAPUState *d, int packet_samples,
                                  int prime_packet)
{
    int xa2_active = xa2_is_active();
    int accepted = 0;
    unsigned queued_buffers = 0;
    uint64_t start_failed_delta = 0;

    if (xa2_active) {
        Xa2OutputStats before;
        xa2_get_output_stats(&before);
        int result = monitor_submit_xaudio2(
            d, (const int16_t *)g_monitor_packet.samples, packet_samples);
        Xa2OutputStats stats;
        xa2_get_output_stats(&stats);
        accepted = result == XA2_SUBMIT_ACCEPTED;
        queued_buffers = stats.queued_buffers;
        start_failed_delta = stats.start_failed - before.start_failed;
    } else if (g_waveout.initialized) {
        int idx = g_waveout.next_buf;
        WAVEHDR *hdr = &g_waveout.hdrs[idx];
        /* Never overwrite a packet still owned by waveOut. */
        if (!(hdr->dwFlags & WHDR_INQUEUE) || (hdr->dwFlags & WHDR_DONE)) {
            memcpy(g_waveout.bufs[idx], g_monitor_packet.samples,
                   packet_samples * 2 * sizeof(int16_t));
            hdr->dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
                g_waveout.frames_written++;
                accepted = 1;
            }
        }
    }

    g_monitor_packet.used = 0;
    mixer_output_tail_packet_submitted(accepted, xa2_active, queued_buffers,
                                       start_failed_delta, prime_packet);
}

/* Finish a pending host packet without advancing the guest's EP/VP timeline. */
static void mixer_output_tail_flush(MCPXAPUState *d)
{
    if (!g_mixer_output_tail.pending || g_mixer_active_count || g_test_tone.active)
        return;

    int xa2_active = xa2_is_active();
    int packet_samples = xa2_active ? xa2_get_buffer_size() : WAVEOUT_BUF_SAMPLES;
    if ((!xa2_active && !g_waveout.initialized) ||
        packet_samples < MIXER_FRAME_SAMPLES ||
        packet_samples > WAVEOUT_BUF_SAMPLES ||
        packet_samples % MIXER_FRAME_SAMPLES) {
        g_monitor_packet.used = 0;
        mixer_output_tail_reset();
        return;
    }

    int prime_packet = 0;
    if (g_mixer_output_tail.prime_pending && g_monitor_packet.used == 0) {
        if (!xa2_active) {
            mixer_output_tail_reset();
            return;
        }
        Xa2OutputStats stats;
        xa2_get_output_stats(&stats);
        /* A drained source packet needs no silent startup buffer. Two queued
         * packets mean the bounded call below is retrying a failed Start. */
        if (stats.queued_buffers == 0 || stats.queued_buffers >= 3) {
            mixer_output_tail_reset();
            return;
        }
        memset(g_monitor_packet.samples, 0,
               packet_samples * 2 * sizeof(int16_t));
        g_monitor_packet.used = packet_samples;
        prime_packet = 1;
    } else if (g_monitor_packet.used > 0 &&
               g_monitor_packet.used < packet_samples) {
        memset(g_monitor_packet.samples + g_monitor_packet.used, 0,
               (packet_samples - g_monitor_packet.used) * 2 * sizeof(int16_t));
        g_monitor_packet.used = packet_samples;
    } else if (g_monitor_packet.used != packet_samples) {
        mixer_output_tail_reset();
        return;
    }

    monitor_submit_packet(d, packet_samples, prime_packet);
}

/* Append one mixed monitor block without changing native EP chronology. A
 * partial tail remains queued until the frame loop gates native production. */
static void mixer_output_tail_collect_frame(MCPXAPUState *d,
                                            int16_t frame_buf[][2],
                                            int packet_samples,
                                            bool mixer_was_active)
{
    if (mixer_was_active && !g_mixer_active_count)
        g_mixer_output_tail.pending = true;

    memcpy(g_monitor_packet.samples + g_monitor_packet.used, frame_buf,
           MIXER_FRAME_SAMPLES * 2 * sizeof(int16_t));
    memset(frame_buf, 0, MIXER_FRAME_SAMPLES * 2 * sizeof(int16_t));
    g_monitor_packet.used += MIXER_FRAME_SAMPLES;
    if (g_monitor_packet.used < packet_samples) return;
    if (g_monitor_packet.used != packet_samples) {
        g_monitor_packet.used = 0;
        mixer_output_tail_reset();
        return;
    }
    monitor_submit_packet(d, packet_samples, 0);
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.ep_fifo_frame_valid = false;
    memset(&g_monitor_packet, 0, sizeof(g_monitor_packet));
    mixer_output_tail_reset();
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    memset(&g_monitor_packet, 0, sizeof(g_monitor_packet));
    mixer_output_tail_reset();
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) return;
    if (d->trace_output) {
        int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (!d->output_started_us) {
            d->output_started_us = now;
            fprintf(stderr, "[AUDIO] start-us=%lld rate=48000 block-samples=%d\n",
                    (long long)now, MIXER_FRAME_SAMPLES);
        }
        if (++d->output_blocks % 938 == 0) {
            Xa2OutputStats stats;
            xa2_get_output_stats(&stats);
            fprintf(stderr, "[AUDIO] elapsed-us=%lld produced-samples=%llu native-ticks=%llu dry-ticks=%llu overloaded=%llu limited=%llu clipped=%llu source-nonzero=%llu silent-blocks=%llu muted=%d mixer-active=%d submitted=%llu played=%llu queued=%u queue-full=%llu submit-failed=%llu start-failed=%llu stop-failed=%llu empty-queue=%llu reprimes=%llu critical-errors=%u critical-status=%08X recovery-attempts=%llu recoveries=%llu recovery-failed=%llu discarded-samples=%llu rebases=%llu backpressure-packets=%llu backpressure-timeouts=%llu producer-dropped=%llu mmcss=%u gated-waits=%llu\n",
                    (long long)(now - d->output_started_us),
                    (unsigned long long)(d->output_blocks * MIXER_FRAME_SAMPLES),
                    (unsigned long long)d->output_native_ticks,
                    (unsigned long long)d->output_dry_ticks,
                    (unsigned long long)d->output_overloaded_samples,
                    (unsigned long long)d->output_limited_samples,
                    (unsigned long long)d->output_clipped_samples,
                    (unsigned long long)d->output_nonzero_samples,
                    (unsigned long long)d->output_silent_blocks, g_audio_muted,
                    (int)g_mixer_active_count,
                    (unsigned long long)stats.submitted_samples,
                    (unsigned long long)stats.played_samples, stats.queued_buffers,
                    (unsigned long long)stats.queue_full,
                    (unsigned long long)stats.submit_failed,
                    (unsigned long long)stats.start_failed,
                    (unsigned long long)stats.stop_failed,
                    (unsigned long long)stats.empty_queue,
                    (unsigned long long)stats.reprimes,
                    stats.critical_errors, (unsigned)stats.critical_error_status,
                    (unsigned long long)stats.recovery_attempts,
                    (unsigned long long)stats.recoveries,
                    (unsigned long long)stats.recovery_failed,
                    (unsigned long long)stats.discarded_samples,
                    (unsigned long long)d->output_rebases,
                    (unsigned long long)g_monitor_packet.waited_packets,
                    (unsigned long long)g_monitor_packet.timed_out_packets,
                    (unsigned long long)g_monitor_packet.dropped_samples,
                    stats.mmcss_registered,
                    (unsigned long long)g_monitor_packet.gated_waits);
        }
    }
    d->monitor.ep_fifo_frame_valid = false;

    /* EP FIFO writes can be split across DMA descriptors. Consume only a
     * complete block at the output boundary and discard any partial tail. */
    if (d->ep.dsp) {
        d->ep.dsp->ep_fifo_frame_bytes_used = 0;
        d->ep.dsp->ep_fifo_frame_ready = false;
    }

    /* The DSP has filled eight chronological 32-sample slices. Keep that
     * native 256-sample block, and advance software voices exactly once at
     * 48 kHz. Rendering an entire host packet here both discarded the EP
     * output and ran the software mixer four/eight times too fast. */
    if (g_test_tone.active) {
        for (int i = 0; i < MIXER_FRAME_SAMPLES; i++) {
            int16_t sample = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
            d->monitor.frame_buf[i][0] = sample;
            d->monitor.frame_buf[i][1] = sample;
            g_test_tone.phase += g_test_tone.phase_inc;
            if (g_test_tone.phase >= 2.0 * M_PI) g_test_tone.phase -= 2.0 * M_PI;
        }
    }
    int mixer_was_active = g_mixer_active_count != 0;
    if (mixer_was_active) mixer_output_tail_reset();
    mixer_render(d->monitor.frame_buf, MIXER_FRAME_SAMPLES);
    if (mixer_was_active && !g_mixer_active_count) {
        g_mixer_output_tail.pending = true;
    }
    if (g_audio_muted) {
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        /* Also silence earlier, not-yet-submitted slices of this packet. */
        memset(g_monitor_packet.samples, 0, sizeof(g_monitor_packet.samples));
    }
    if (d->trace_output) {
        unsigned nonzero = 0;
        for (int i = 0; i < MIXER_FRAME_SAMPLES; ++i)
            for (int channel = 0; channel < 2; ++channel)
                nonzero += d->monitor.frame_buf[i][channel] != 0;
        d->output_nonzero_samples += nonzero;
        if (!nonzero) ++d->output_silent_blocks;
    }

    if (d->trace_dsp_state) {
        uint64_t block = ++d->dsp_trace_audio_blocks;
        if (mcpx_dsp_trace_audio_sample_due(block)) {
            unsigned int nonzero = 0, peak = 0;
            uint64_t square_sum = 0;
            for (int i = 0; i < MIXER_FRAME_SAMPLES; ++i) {
                for (int channel = 0; channel < 2; ++channel) {
                    int sample = d->monitor.frame_buf[i][channel];
                    unsigned int magnitude = (unsigned int)(sample < 0 ? -sample : sample);
                    if (magnitude) nonzero++;
                    if (magnitude > peak) peak = magnitude;
                    square_sum += (uint64_t)magnitude * magnitude;
                }
            }
            fprintf(stderr, "[DSPTRACE] event=audio-block block=%llu nonzero=%u/%u peak=%u mean-square=%llu ep-fifo-writes=%llu ep-fifo-bytes=%llu ep-fifo-captured=%llu monitor-point=%u\n",
                    (unsigned long long)block, nonzero,
                    (unsigned int)(MIXER_FRAME_SAMPLES * 2), peak,
                    (unsigned long long)(square_sum / (MIXER_FRAME_SAMPLES * 2)),
                    (unsigned long long)d->dsp_trace_ep_fifo_writes,
                    (unsigned long long)d->dsp_trace_ep_fifo_bytes,
                    (unsigned long long)d->dsp_trace_ep_fifo_captured,
                    (unsigned int)d->monitor.point);
        }
    }

    int packet_samples = xa2_is_active() ? xa2_get_buffer_size() : WAVEOUT_BUF_SAMPLES;
    if ((!xa2_is_active() && !g_waveout.initialized) ||
        packet_samples < MIXER_FRAME_SAMPLES || packet_samples > WAVEOUT_BUF_SAMPLES ||
        packet_samples % MIXER_FRAME_SAMPLES) {
        g_monitor_packet.used = 0;
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        mixer_output_tail_reset();
        return;
    }
    mixer_output_tail_collect_frame(d, d->monitor.frame_buf, packet_samples,
                                    mixer_was_active);
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

static void throttle(MCPXAPUState *d)
{
    if (d->ep_frame_div % 8) {
        return;
    }

    /* An overloaded DSP can stay behind every deadline, so the timed wait
     * below may never release the device mutex. Register MMIO, IRQ service
     * and shutdown are not PIO waiters and cannot use the per-frame PIO
     * handoff. Service contention independently of pacing; the frame loop
     * rechecks exit/pause/front-end state on return. */
    mcpx_apu_handoff(d);

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (mcpx_apu_rebase_deadline(&d->next_frame_time_us,
                               &d->next_frame_time_fraction, now_us))
        ++d->output_rebases;

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    mcpx_apu_advance_deadline(&d->next_frame_time_us,
                            &d->next_frame_time_fraction);

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_pio_drain(d);
    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

/* The frame thread keeps d->lock across up to eight frames between its waits
 * (throttle waits once per eight), and a title thread writing a PIO method
 * waited for all of them: about a quarter of BLACK's main thread in busy
 * scenes. Between frames, a waiting writer gets the lock: the method still
 * runs at once, in order, as before, after at most one frame instead of
 * eight. RECOMP_APU_PIO_HANDOFF=0 keeps the lock across the frames. */
extern volatile LONG g_apu_pio_waiters;

static void mcpx_apu_pio_handoff(MCPXAPUState *d)
{
    static int on = -1;
    int turns = 0;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_PIO_HANDOFF");
        on = !(e && *e == '0');
    }
    if (!on || !InterlockedCompareExchange(&g_apu_pio_waiters, 0, 0))
        return;
    qemu_mutex_unlock(&d->lock);
    /* bounded: a title writing methods back to back must not starve the audio */
    while (InterlockedCompareExchange(&g_apu_pio_waiters, 0, 0) && turns++ < 64)
        SwitchToThread();
    qemu_mutex_lock(&d->lock);
}

/* How the frame thread spends its frames, for a hang report: running the
 * voice processor, or only the monitor because the front end is off, halted
 * or trapped. */
volatile uint64_t g_apu_frames_full, g_apu_frames_monitor;

/* The frame thread's host id, for a hang report (0 before it starts). */
unsigned long mcpx_apu_frame_thread_id(void)
{
#if defined(_WIN32)
    if (g_state && g_state->apu_thread.thread)
        return GetThreadId(g_state->apu_thread.thread);
#endif
    return 0;
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);

    /* The submission thread owns the output graph and its COM apartment.
     * Startup waits for this thread's idle signal after initialization. */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count &&
            !mixer_output_tail_pending()) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Interrupts, as xemu's frame thread raises them: a front end that
         * is off, trapped or halted has something to report, and a frame
         * that wrote a voice notifier set set_irq. Either way the line is
         * recomputed from ISTS and IEN here, outside any register access. */
        {
            int xcm = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                               NV_PAPU_SECTL_XCNTMODE);
            uint32_t fe = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            if (xcm == NV_PAPU_SECTL_XCNTMODE_OFF ||
                (fe & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
                (fe & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
                d->set_irq = true;
            }
            if (d->set_irq) {
                update_irq(d);
                d->set_irq = false;
            }
        }

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        /* A front-end trap is serviced by the guest ISR. Advancing a silent
         * tick here creates a global gap and keeps that ISR behind our lock.
         * Preserve this EP epoch and let the guest clear the trap instead.
         * Explicit host voices/test tone still need output when the APU is off. */
        if (!apu_active && !g_test_tone.active && !g_mixer_active_count &&
            !mixer_output_tail_pending()) {
            if (d->trace_output) ++g_monitor_packet.gated_waits;
            qemu_cond_timedwait(&d->cond, &d->lock, 5);
            continue;
        }

        throttle(d);
        if (qatomic_read(&d->exiting) ||
            (d->pause_requested && !g_test_tone.active && !g_mixer_active_count &&
             !mixer_output_tail_pending()))
            continue;
        /* The condition wait in throttle can release the lock too. Recheck
         * guest control writes before consuming any source samples. */
        xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                           NV_PAPU_SECTL_XCNTMODE);
        fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                     !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                     !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);
        if (!apu_active && !g_test_tone.active && !g_mixer_active_count &&
            !mixer_output_tail_pending())
            continue;

        if (!apu_active && !g_test_tone.active && !g_mixer_active_count &&
            mixer_output_tail_pending()) {
            mixer_output_tail_flush(d);
            continue;
        }

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            g_apu_frames_full++;
            se_frame(d);
            mcpx_apu_pio_handoff(d);
        } else {
            g_apu_frames_monitor++;
            /* No DSP wrote this tick. Preserve earlier native slices if
             * the APU halted in the middle of the eight-tick EP epoch. */
            int offset = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            if (!d->monitor.ep_fifo_frame_valid)
                memset(d->monitor.frame_buf + offset, 0,
                       NUM_SAMPLES_PER_FRAME * sizeof(d->monitor.frame_buf[0]));
            /* Lightweight: just monitor frame (test tone + software mixer) */
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }
    }

    mcpx_apu_monitor_finalize(d);
    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);
    mcpx_apu_dsp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    return mcpx_apu_init_standalone_sized(ram_ptr, 64u * 1024u * 1024u);
}

MCPXAPUState *mcpx_apu_init_standalone_sized(uint8_t *ram_ptr, size_t ram_size)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;
    d->ram_size = ram_size;
    /* Preserve the hook's presence semantics, including "0" and empty values.
     * Cache before publishing the initialized APU to the hook or frame thread. */
    d->trace_mmio = getenv("RECOMP_APU_TRACE") != NULL;
    {
        const char *trace = getenv("RECOMP_APU_TRACE_OUTPUT");
        d->trace_output = trace && strcmp(trace, "1") == 0;
    }

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);
    qemu_cond_init(&d->lock_handoff_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    mcpx_apu_lock(d);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_dsp_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    } else if (addr >= 0x30000 && addr < 0x40000) {
        if (is_write) mcpx_apu_dsp_mmio_write(d, addr - 0x30000, val, size, true);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        if (is_write) mcpx_apu_dsp_mmio_write(d, addr - 0x50000, val, size, false);
    }
    /* Other regions retain their zero/ignored fallback. */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        return mcpx_apu_dsp_mmio_read(d, addr - 0x30000, size, true);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        return mcpx_apu_dsp_mmio_read(d, addr - 0x50000, size, false);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    mcpx_apu_lock(d);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        mcpx_apu_lock(g_state);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
