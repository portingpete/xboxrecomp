/*
 * MCPX APU Audio Emulation - Public API
 *
 * Extracted from xemu (LGPL v2+), adapted for standalone use.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MCPXAPUState MCPXAPUState;

/* Initialize the APU emulation.
 * ram_ptr: pointer to the base of Xbox physical RAM (64MB).
 * Returns the APU state, or NULL on failure. */
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);

/* Initialize with the actual readable physical backing length. DSP bootstrap
 * bounds every SGE and source-page read against this length and the decoded
 * APU aperture. The original initializer retains its 64MB contract. */
MCPXAPUState *mcpx_apu_init_standalone_sized(uint8_t *ram_ptr, size_t ram_size);

/* Opt in to a title's custom stereo DSP input pair for dry fallback output.
 * Bins must be distinct and in 10..31. Returns 1 on success, or 0 for invalid
 * arguments without changing the existing pair. New instances have no pair.
 * Native EP PCM takes precedence; RECOMP_APU_MIXDOWN_ALL=0 also excludes this
 * pair. The caller must keep d alive while setting its pair. */
int mcpx_apu_set_stereo_fallback_pair(MCPXAPUState *d,
                                      unsigned int left_bin,
                                      unsigned int right_bin);

/* Shut down and free the APU state. */
void mcpx_apu_shutdown(MCPXAPUState *d);

/* Where the APU's interrupt goes. Called with 1 while an enabled interrupt is
 * pending (NV_PAPU_ISTS GINTSTS) and 0 once it is not -- on hardware, the PCI
 * INTA line. The runtime connects it to the kernel's vector for the APU
 * (xbox_SetInterruptLine(XBOX_APU_VECTOR, ...)); unset, nothing is raised. */
void mcpx_apu_set_irq_line(void (*line)(int asserted));
/* Complete guest ISR serialization callbacks. opaque is a live MCPXAPUState;
 * call begin after entering ISR IRQL and end before leaving it. Recursive
 * device MMIO is allowed. Stop/unregister delivery before device shutdown. */
void mcpx_apu_irq_service_begin(void *opaque);
int mcpx_apu_irq_service_try(void *opaque);
void mcpx_apu_irq_service_end(void *opaque);

/* Print the front end's state and every listed voice (for a hang report). */
void mcpx_apu_dump_state(void);
/* The frame thread's host thread id, 0 before it starts. */
unsigned long mcpx_apu_frame_thread_id(void);

/* MMIO read from APU register space (addr is offset from 0xFE800000). */
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);

/* MMIO write to APU register space (addr is offset from 0xFE800000). */
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

/* Play a 440Hz test tone through the APU pipeline to verify audio output.
 * Directly programs a voice without going through DirectSound. */
void mcpx_apu_play_test_tone(MCPXAPUState *d);

/* ============================================================
 * Software mixer - DirectSound buffer bridge
 *
 * DirectSound buffers register here. The APU frame thread
 * mixes active buffers directly into the waveOut output,
 * bypassing the VP hardware voice pipeline.
 * ============================================================ */

#define APU_MIXER_MAX_VOICES 64

typedef struct APUMixerVoice {
    volatile int     active;       /* 1 = playing, 0 = stopped */
    volatile int     looping;      /* 1 = loop, 0 = one-shot */
    const int16_t   *pcm_data;     /* Pointer to 16-bit PCM samples */
    uint32_t         pcm_bytes;    /* Total buffer size in bytes */
    uint32_t         num_channels;  /* 1=mono, 2=stereo */
    uint32_t         sample_rate;   /* e.g. 22050, 44100, 48000 */
    uint64_t         play_offset; /* Source frame position, 16 fractional bits */
    float            volume;       /* 0.0 to 1.0 */
} APUMixerVoice;

/* Allocate a mixer voice slot. Returns slot index or -1 if full. */
int apu_mixer_alloc_voice(void);

/* Free a mixer voice slot. */
void apu_mixer_free_voice(int slot);

/* Get pointer to a mixer voice for configuration. */
APUMixerVoice *apu_mixer_get_voice(int slot);

/* Seek in PCM bytes (rounded down to a whole frame). Returns 0 if invalid. */
int apu_mixer_set_position(int slot, uint32_t byte_offset);

/* Snapshot playback state under the mixer lock; output pointers may be NULL. */
void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping);

/* Start/resume or stop playback without changing the current position. */
void apu_mixer_play(int slot, int looping);
void apu_mixer_stop(int slot);

#ifdef __cplusplus
}
#endif
