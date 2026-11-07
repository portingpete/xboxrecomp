/*
 * Kelvin: the NV2A command front end -- the PFIFO DMA pusher, object lookup,
 * and the GPU thread that runs them.
 *
 * The pusher follows xemu's pfifo_run_pusher (hw/xbox/nv2a/pfifo.c): it walks
 * the channel's pushbuffer from DMA_GET to DMA_PUT, following jumps, calls and
 * returns, keeps an unfinished method header's state across the end of a
 * segment, translates object handles for methods 0x180..0x1FC through RAMHT,
 * and only then advances DMA_GET. GET therefore says what the GPU has actually
 * consumed, which is what D3D's ring-space and fence logic rely on.
 *
 * Enabled with RECOMP_NV2A_KELVIN=1. It replaces the ack thread's GET=PUT
 * echo and the survey/executor in src/kernel/nv2a_pb_*.c for that run.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 * (xemu PFIFO/RAMHT/DMA-object logic this follows)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <windows.h>
#include <intrin.h>

#include "kelvin_int.h"
#include "kelvin.h"
#include "kelvin_workers.h"
#include "kelvin_render.h"
#include "kelvin_gpu.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

KelvinState g_kelvin;

static volatile LONG s_active;
static HANDLE s_thread;

/* Presentation metadata is ordered by the exact pushbuffer parameter VA,
 * independently of the title/GPU threads' timing. The title must register
 * before PUT exposes that word. Unmarked flips always keep normal policy.
 * This lock is never held while acquiring the register-model lock. */
#define KELVIN_FRAME_ASPECT_CAPACITY 16
typedef struct FrameAspect {
    uint32_t va, width, height;
} FrameAspect;
static FrameAspect s_frame_aspects[KELVIN_FRAME_ASPECT_CAPACITY];
static SRWLOCK s_frame_aspects_lock = SRWLOCK_INIT;
static volatile LONG s_frame_aspects_count;
/* Only the GPU pusher writes these; FLIP_STALL immediately passes them by
 * value to the renderer, which drains its workers before publication. */
uint32_t g_kelvin_frame_aspect_w, g_kelvin_frame_aspect_h;

int kelvin_mark_frame_aspect(uint32_t flip_data_va,
                            uint32_t aspect_w, uint32_t aspect_h)
{
    unsigned i, n;
    int marked = 0;
    if ((flip_data_va & 3u) || flip_data_va < KELVIN_PHYS_WINDOW ||
        (uint64_t)flip_data_va + 4u >
            (uint64_t)KELVIN_PHYS_WINDOW + KELVIN_PHYS_SIZE ||
        !aspect_w || !aspect_h || aspect_w > 8192u || aspect_h > 8192u)
        return 0;
    AcquireSRWLockExclusive(&s_frame_aspects_lock);
    n = (unsigned)s_frame_aspects_count;
    for (i = 0; i < n; ++i) {
        if (s_frame_aspects[i].va == flip_data_va) {
            /* Ring reuse while a marker is unresolved violates the caller
             * contract. Discard the ambiguous old entry and reject the new
             * one, rather than attributing either frame to the other. */
            s_frame_aspects[i] = s_frame_aspects[n - 1u];
            InterlockedExchange(&s_frame_aspects_count, (LONG)(n - 1u));
            goto done;
        }
    }
    if (n < KELVIN_FRAME_ASPECT_CAPACITY) {
        s_frame_aspects[n].va = flip_data_va;
        s_frame_aspects[n].width = aspect_w;
        s_frame_aspects[n].height = aspect_h;
        InterlockedExchange(&s_frame_aspects_count, (LONG)(n + 1u));
        marked = 1;
    }
done:
    ReleaseSRWLockExclusive(&s_frame_aspects_lock);
    return marked;
}

static void frame_aspect_for_method(uint32_t parameter_va, uint32_t method)
{
    unsigned i, n;
    g_kelvin_frame_aspect_w = g_kelvin_frame_aspect_h = 0;
    /* Ordinary gameplay has no metadata and takes no additional lock. */
#ifdef _MSC_VER
    if (!ReadAcquire(&s_frame_aspects_count)) return;
#else
    if (!__atomic_load_n(&s_frame_aspects_count, __ATOMIC_ACQUIRE)) return;
#endif
    AcquireSRWLockExclusive(&s_frame_aspects_lock);
    n = (unsigned)s_frame_aspects_count;
    for (i = 0; i < n; ++i) {
        if (s_frame_aspects[i].va == parameter_va) {
            /* Consume even if a malformed/replaced packet now names a
             * different method, so its marker cannot survive a ring wrap. */
            if (method == NV097_FLIP_STALL) {
                g_kelvin_frame_aspect_w = s_frame_aspects[i].width;
                g_kelvin_frame_aspect_h = s_frame_aspects[i].height;
            }
            s_frame_aspects[i] = s_frame_aspects[n - 1u];
            InterlockedExchange(&s_frame_aspects_count, (LONG)(n - 1u));
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_frame_aspects_lock);
}

/* Split-screen view marks, oldest first. A mark whose word the pusher passed without matching (none should) is
 * dropped when a later one matches. */
#define KELVIN_VIEW_CAPACITY 16
typedef struct ViewMark {
    uint32_t va;
    uint64_t rect;
    int mode;
} ViewMark;
static ViewMark s_view_marks[KELVIN_VIEW_CAPACITY];
static SRWLOCK s_view_marks_lock = SRWLOCK_INIT;
static volatile LONG s_view_marks_count;

static void view_thunk(uintptr_t rect, uintptr_t mode)
{
    kgpu_set_view((int)(mode & 0xFF), (int)(mode >> 8), (int)(rect & 0xFFFF), (int)((rect >> 16) & 0xFFFF),
                  (int)((rect >> 32) & 0xFFFF), (int)((rect >> 48) & 0xFFFF));
}

int kelvin_mark_view(uint32_t va, int mode, int slot, int x0, int y0, int x1, int y1)
{
    int marked = 0;
    if (!kgpu_active() || (va & 3u) || va < KELVIN_PHYS_WINDOW ||
        (uint64_t)va + 4u > (uint64_t)KELVIN_PHYS_WINDOW + KELVIN_PHYS_SIZE ||
        x0 < 0 || y0 < 0 || x1 > 0xFFFF || y1 > 0xFFFF || x1 <= x0 || y1 <= y0)
        return 0;
    AcquireSRWLockExclusive(&s_view_marks_lock);
    if (s_view_marks_count < KELVIN_VIEW_CAPACITY) {
        ViewMark *m = &s_view_marks[s_view_marks_count];
        m->va = va;
        m->rect = (uint64_t)x0 | (uint64_t)y0 << 16 | (uint64_t)x1 << 32 | (uint64_t)y1 << 48;
        m->mode = (mode & 0xFF) | (slot & 0xFF) << 8;
        InterlockedExchange(&s_view_marks_count, s_view_marks_count + 1);
        marked = 1;
    }
    ReleaseSRWLockExclusive(&s_view_marks_lock);
    return marked;
}

static void view_for_word(uint32_t va)
{
    LONG i, n, k;
#ifdef _MSC_VER
    if (!ReadAcquire(&s_view_marks_count)) return;
#else
    if (!__atomic_load_n(&s_view_marks_count, __ATOMIC_ACQUIRE)) return;
#endif
    AcquireSRWLockExclusive(&s_view_marks_lock);
    n = s_view_marks_count;
    for (i = n - 1; i >= 0 && s_view_marks[i].va != va; --i) {}
    if (i >= 0) {
        for (k = 0; k <= i; ++k) {
            if (s_view_marks[k].va != va) continue;
            if (kgpu_async_active()) kgpu_defer_call(view_thunk, (uintptr_t)s_view_marks[k].rect, (uintptr_t)s_view_marks[k].mode);
            else view_thunk((uintptr_t)s_view_marks[k].rect, (uintptr_t)s_view_marks[k].mode);
        }
        for (k = i + 1; k < n; ++k) s_view_marks[k - i - 1] = s_view_marks[k];
        InterlockedExchange(&s_view_marks_count, n - i - 1);
    }
    ReleaseSRWLockExclusive(&s_view_marks_lock);
}

/* ---- guest memory ---------------------------------------------------------- */

uint8_t *kelvin_guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

uint8_t *kelvin_aperture(uint32_t offset)
{
    return kelvin_guest(KELVIN_APERTURE_VA + offset);
}

uint32_t kelvin_aperture_r32(uint32_t offset)
{
    return *(volatile uint32_t *)kelvin_aperture(offset);
}

static void aperture_w32(uint32_t offset, uint32_t v)
{
    *(volatile uint32_t *)kelvin_aperture(offset) = v;
}

uint32_t kelvin_pramin_r32(uint32_t offset)
{
    if (offset > KELVIN_PRAMIN_SIZE - 4) {
        return 0;
    }
    return kelvin_aperture_r32(KELVIN_PRAMIN_BASE + offset);
}

/* ---- unimplemented-feature accounting -------------------------------------- */

static const char *const s_unsupported_names[KU_COUNT] = {
    "method on unmodelled object class", "Kelvin method without handler",
    "fixed-function lighting", "fixed-function texgen",
    "fixed-function skinning", "fixed-function fog generation",
    "point primitives", "polygon mode (point/line)",
    "texture shader mode", "texture colour format", "3D texture",
    "cube texture", "texture border", "shadow-map texture",
    "surface colour format", "swizzled render target", "float depth buffer",
    "logic op", "window clip", "zpass reports",
    "unresolvable DMA context", "image blit operation", "convolution filter",
    "HILO dot mapping", "specular power reconstruction",
};

void kelvin_unsupported(int what, const char *detail, uint32_t value)
{
    uint64_t n;

    if (what < 0 || what >= KU_COUNT) {
        return;
    }
    /* Raster threads report too (texture and dot-mapping modes met per
     * fragment), so the count is atomic. */
    n = (uint64_t)InterlockedIncrement64(
        (volatile LONG64 *)&g_kelvin.stats.unsupported[what]);
    if (n == 1 || n == 1000 || n == 1000000) {
        fprintf(stderr, "[KELVIN] not implemented: %s (%s, 0x%08X) x%llu\n",
                s_unsupported_names[what], detail ? detail : "", value,
                (unsigned long long)n);
        fflush(stderr);
    }
}

/* ---- objects ------------------------------------------------------------- */

bool kelvin_ramht_lookup(uint32_t handle, uint32_t *instance,
                         unsigned int *engine)
{
    uint32_t ramht = kelvin_aperture_r32(KELVIN_PFIFO_BASE + NV_PFIFO_RAMHT);
    uint32_t size = 1u << (GET_MASK(ramht, NV_PFIFO_RAMHT_SIZE) + 12);
    uint32_t base = GET_MASK(ramht, NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;
    uint32_t chid = GET_MASK(kelvin_aperture_r32(KELVIN_PFIFO_BASE +
                                                 NV_PFIFO_CACHE1_PUSH1),
                             NV_PFIFO_CACHE1_PUSH1_CHID);
    unsigned int bits = ctz32(size) - 1;
    uint32_t hash = 0, h = handle, entry_handle, entry_context;

    while (h) {
        hash ^= h & ((1u << bits) - 1);
        h >>= bits;
    }
    hash ^= chid << (bits - 4);
    if (hash * 8 >= size) {
        return false;
    }
    entry_handle = kelvin_pramin_r32(base + hash * 8);
    entry_context = kelvin_pramin_r32(base + hash * 8 + 4);
    if (entry_handle != handle || !(entry_context & NV_RAMHT_STATUS)) {
        return false;
    }
    if (instance) {
        *instance = (entry_context & NV_RAMHT_INSTANCE) << 4;
    }
    if (engine) {
        *engine = (entry_context & NV_RAMHT_ENGINE) >> 16;
    }
    return true;
}

uint32_t kelvin_dma_limit(uint32_t instance)
{
    return kelvin_pramin_r32(instance + 4);
}

uint32_t kelvin_dma_va(uint32_t instance, uint32_t offset, uint32_t bytes)
{
    uint32_t flags = kelvin_pramin_r32(instance);
    uint32_t limit = kelvin_pramin_r32(instance + 4);
    uint32_t frame = kelvin_pramin_r32(instance + 8);
    uint64_t base = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST);
    uint64_t phys = base + offset;

    if (bytes && (uint64_t)offset + bytes - 1 > limit) {
        return 0;
    }
    if (phys + bytes > KELVIN_PHYS_SIZE) {
        return 0;
    }
    return KELVIN_PHYS_WINDOW + (uint32_t)phys;
}

/* ---- the DMA pusher --------------------------------------------------------- */

typedef struct Pusher {
    bool started;
    uint32_t get;                  /* offset in the pushbuffer DMA object */
    uint32_t method, subchannel, count;
    bool non_inc;
    uint32_t subroutine;           /* return offset */
    bool in_subroutine;
    uint32_t dma_instance;         /* pushbuffer DMA object, 0 = physical */
} Pusher;

static Pusher s_push;

/* The pushbuffer DMA object as kelvin_dma_va reads it, read once each time
 * the pusher takes the lock rather than for every word. */
typedef struct PushMap {
    uint64_t base;
    uint32_t limit;
} PushMap;

static void push_map(PushMap *m)
{
    if (s_push.dma_instance) {
        uint32_t flags = kelvin_pramin_r32(s_push.dma_instance);
        uint32_t frame = kelvin_pramin_r32(s_push.dma_instance + 8);
        m->limit = kelvin_pramin_r32(s_push.dma_instance + 4);
        m->base = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST);
    }
}

/* kelvin_dma_va(dma_instance, offset, 4), or the physical window. */
static uint32_t pushbuffer_va(const PushMap *m, uint32_t offset)
{
    if (s_push.dma_instance) {
        uint64_t phys = m->base + offset;
        if ((uint64_t)offset + 3 > m->limit || phys + 4 > KELVIN_PHYS_SIZE) {
            return 0;
        }
        return KELVIN_PHYS_WINDOW + (uint32_t)phys;
    }
    return offset < KELVIN_PHYS_SIZE ? KELVIN_PHYS_WINDOW + offset : 0;
}

/* Words the pusher reads per hold of the lock at most. */
#define KELVIN_WORDS_PER_LOCK 256

/* Words read between publications of GET: 1 KiB. */
#define KELVIN_GET_EVERY 256

static void publish_get_raw(uint32_t get)
{
    aperture_w32(KELVIN_USER_DMA_GET, get);
    aperture_w32(KELVIN_PFIFO_BASE + NV_PFIFO_CACHE1_DMA_GET, get);
}

/* With asynchronous GPU submission the final GET of a run (at PUT, which reads as "the GPU is idle") is published by
 * the GPU thread behind the draws before it, while the pusher goes on publishing GET as it reads. Each publication has
 * a sequence number and a late one that a newer one has overtaken is dropped, so GET never goes back. */
static LONG s_pub_seq;
static volatile LONG s_pub_applied, s_pub_lock;

static void publish_get_seq(LONG seq, uint32_t get)
{
    while (InterlockedExchange(&s_pub_lock, 1)) YieldProcessor();
    if ((LONG)(seq - s_pub_applied) > 0) {
        s_pub_applied = seq;
        publish_get_raw(get);
    }
    InterlockedExchange(&s_pub_lock, 0);
}

static void publish_get_thunk(uintptr_t seq, uintptr_t get)
{
    publish_get_seq((LONG)seq, (uint32_t)get);
}

static void publish_get(uint32_t get)
{
    publish_get_seq(++s_pub_seq, get);
}

/* GET at PUT: once everything the pusher handed to the GPU thread has been executed. */
static void publish_get_idle(uint32_t get)
{
    LONG seq = ++s_pub_seq;
    if (kgpu_async_active()) kgpu_defer_call(publish_get_thunk, (uintptr_t)seq, (uintptr_t)get);
    else publish_get_seq(seq, get);
}

static bool flip_stall_complete(KelvinState *pg)
{
    return PG_GET_MASK(NV_PGRAPH_SURFACE, NV_PGRAPH_SURFACE_READ_3D)
           != PG_GET_MASK(NV_PGRAPH_SURFACE, NV_PGRAPH_SURFACE_WRITE_3D);
}

/* How long a flip stall may wait for the ISR before the pusher completes it
 * itself. Without RECOMP_VBLANK no ISR ever runs, and a stall that nothing can
 * end is a hang; with it, the ISR normally answers within a frame. */
static DWORD s_flip_timeout_ms = 100;

/* Whether the engine may not take the next method, as xemu's puller decides
 * it: PGRAPH's FIFO access switched off by the title (its interrupt handler
 * does that while it works), a notify or context switch waiting for the
 * title to acknowledge it, or a flip stall. Lock held. */
static bool pusher_stalled(KelvinState *pg)
{
    static DWORD stall_start;

    if (!(pgraph_reg_r(pg, NV_PGRAPH_FIFO) & NV_PGRAPH_FIFO_ACCESS) ||
        pg->waiting_for_nop || pg->waiting_for_context_switch) {
        return true;
    }
    if (!pg->waiting_for_flip) {
        return false;
    }
    if (flip_stall_complete(pg)) {
        pg->waiting_for_flip = false;
        stall_start = 0;
        return false;
    }
    if (!stall_start) {
        stall_start = GetTickCount();
        return true;
    }
    if (GetTickCount() - stall_start >= s_flip_timeout_ms) {
        static unsigned said;
        uint32_t modulo = PG_GET_MASK(NV_PGRAPH_SURFACE,
                                      NV_PGRAPH_SURFACE_MODULO_3D);
        uint32_t next = PG_GET_MASK(NV_PGRAPH_SURFACE,
                                    NV_PGRAPH_SURFACE_READ_3D) + 1;
        if (said++ < 3) {
            fprintf(stderr, "[KELVIN] flip stall not released by a vblank"
                            " ISR within %lu ms; completing it (set"
                            " RECOMP_VBLANK to deliver vblank)\n",
                    (unsigned long)s_flip_timeout_ms);
            fflush(stderr);
        }
        if (modulo) {
            next %= modulo;
        }
        PG_SET_MASK(NV_PGRAPH_SURFACE, NV_PGRAPH_SURFACE_READ_3D, next);
        pg->waiting_for_flip = false;
        stall_start = 0;
        return false;
    }
    return true;
}

static void dispatch(KelvinState *pg, uint32_t subchannel, uint32_t method,
                     uint32_t parameter, bool inc)
{
    {
        /* RECOMP_KELVIN_METHODSTAT=1: once a second, the methods that arrived most often */
        static int stat_on = -1;
        static uint32_t ids[512];
        static unsigned cnt[512];
        static ULONGLONG last;
        if (stat_on < 0) { const char *e = getenv("RECOMP_KELVIN_METHODSTAT"); stat_on = e && *e && *e != '0'; }
        if (stat_on) {
            unsigned k = (method >> 2) & 511u, probes = 0;
            ULONGLONG now = GetTickCount64();
            while (probes < 512 && cnt[k] && ids[k] != method) { k = (k + 1) & 511u; probes++; }
            if (probes < 512) { ids[k] = method; cnt[k]++; }
            if (!last) last = now;
            if (now - last >= 1000) {
                unsigned order[512], n = 0, i, j;
                for (i = 0; i < 512; i++) if (cnt[i]) order[n++] = i;
                for (i = 0; i < n && i < 14; i++)
                    for (j = i + 1; j < n; j++) if (cnt[order[j]] > cnt[order[i]]) { unsigned t = order[i]; order[i] = order[j]; order[j] = t; }
                fprintf(stderr, "[KMETH] last second:");
                for (i = 0; i < n && i < 14; i++) fprintf(stderr, " 0x%X x%u", ids[order[i]], cnt[order[i]]);
                fprintf(stderr, "\n");
                memset(cnt, 0, sizeof(cnt));
                last = now;
            }
        }
    }
    if (method >= 0x180 && method < 0x200 && method != 0) {
        uint32_t instance;
        if (!kelvin_ramht_lookup(parameter, &instance, NULL)) {
            kelvin_unsupported(KU_DMA_UNRESOLVED, "object handle", parameter);
            return;
        }
        parameter = instance;
    } else if (method == 0) {
        uint32_t instance;
        if (!kelvin_ramht_lookup(parameter, &instance, NULL)) {
            kelvin_unsupported(KU_DMA_UNRESOLVED, "SET_OBJECT handle",
                               parameter);
            return;
        }
        parameter = instance;
    }
    kelvin_pgraph_method(pg, subchannel, method, parameter, inc);
}

/* Run the pusher until GET reaches PUT or the engine stalls. Returns the
 * number of words consumed. */
static int s_batch_ok = -1;

static uint32_t pusher_run(KelvinState *pg, uint32_t put)
{
    if (s_batch_ok < 0) { const char *e = getenv("RECOMP_KELVIN_RUNS"); s_batch_ok = !(e && *e == '0'); }
    uint32_t words = 0, since_publish = 0;
    kgpu_note_kick();
    unsigned int chid = GET_MASK(kelvin_aperture_r32(KELVIN_PFIFO_BASE +
                                                     NV_PFIFO_CACHE1_PUSH1),
                                 NV_PFIFO_CACHE1_PUSH1_CHID);
    const uint8_t *window = kelvin_guest(KELVIN_PHYS_WINDOW);
    bool stop = false;
    uint32_t held = 0;
    unsigned int lock_poll = 0;
    bool recheck = true;
    PushMap map;

    while (s_push.get != put) {
        uint32_t va, word;
        bool transferred = false;

        /* A word at a time under the lock, so the title's register
         * accesses interleave with methods as they would on hardware: the
         * lock is kept from one word to the next only while no register
         * access (or vblank) is waiting for it, and for a bounded number of
         * words. */
        if (held && (held >= KELVIN_WORDS_PER_LOCK || ((++lock_poll & 3) == 0 && kelvin_lock_wanted()))) {
            kelvin_unlock();
            kelvin_lock_yield();
            held = 0;
        }
        if (!held) {
            recheck = true;
            if (g_kelvin_tracing) {
                long long t0 = kelvin_ticks(), t1;
                kelvin_lock();
                t1 = kelvin_ticks();
                if (t1 - t0 > 1000) {
                    kelvin_render_trace_event(5, t0, t1, 0);
                }
            } else {
                kelvin_lock();
            }
            push_map(&map);
        }
        held++;
        /* What can stop the engine -- FIFO access off, a notify or context switch waiting for the title, the channel
         * not current -- changes only through register accesses, which need the lock this thread holds, or through
         * methods that set the waiting flags. So it is looked at when the lock was just taken, and while a flag says
         * the engine is waiting. */
        if (recheck || pg->waiting_for_nop || pg->waiting_for_context_switch || pg->waiting_for_flip) {
            stop = pusher_stalled(pg);
            if (!stop && s_push.count) {
                kelvin_context_switch(pg, chid);
                stop = pg->waiting_for_context_switch;
            }
            recheck = false;
            if (stop) {
                break;
            }
        }
        va = pushbuffer_va(&map, s_push.get);
        if (!va) {
            static int said;
            if (!said++) {
                fprintf(stderr, "[KELVIN] DMA_GET 0x%08X is outside the"
                                " pushbuffer; stopping\n", s_push.get);
                fflush(stderr);
            }
            s_push.get = put;
            break;
        }
        if (s_push.count > 1 && s_push.non_inc && !g_kelvin_tracing && s_batch_ok) {
            /* A run of data words for one method that takes them in bulk (indices, inline vertex data). The
             * run stays inside one lock hold and one GET-publication interval, as the per-word path would. */
            uint32_t n = s_push.count, room = KELVIN_GET_EVERY - since_publish;
            if (n > room) n = room;
            if (n > 64) n = 64;
            if (put > s_push.get) {
                uint32_t avail = (put - s_push.get) >> 2;
                if (n > avail) n = avail;
            } else {
                n = 1;
            }
            if (n > 1 && pushbuffer_va(&map, s_push.get + 4u * (n - 1)) == va + 4u * (n - 1) &&
                kelvin_pgraph_method_run(pg, s_push.subchannel, s_push.method,
                                         (const uint32_t *)(window + (va - KELVIN_PHYS_WINDOW)), n)) {
                s_push.get += 4u * n;
                words += n;
                pg->stats.words += n;
                held += n - 1;
                g_kelvin_word_get = s_push.get - 4;
                s_push.count -= n;
                since_publish += n;
                if (since_publish >= KELVIN_GET_EVERY && s_push.get != put) {
                    publish_get(kelvin_render_get_bound(s_push.get, pg->stats.words));
                    since_publish = 0;
                }
                continue;
            }
        }
        word = *(volatile const uint32_t *)(window + (va - KELVIN_PHYS_WINDOW));
        s_push.get += 4;
        words++;
        pg->stats.words++;
        frame_aspect_for_method(va, s_push.count ? s_push.method : UINT32_MAX);
        if (!s_push.count) view_for_word(va);

        if (s_push.count) {
            g_kelvin_word_get = s_push.get - 4;
            if (g_kelvin_tracing) {
                long long t0 = kelvin_ticks(), t1;
                dispatch(pg, s_push.subchannel, s_push.method, word,
                         !s_push.non_inc);
                t1 = kelvin_ticks();
                if (t1 - t0 > 2000) {
                    kelvin_render_trace_event(6, t0, t1,
                                              s_push.subchannel << 16 | s_push.method);
                }
            } else {
                dispatch(pg, s_push.subchannel, s_push.method, word,
                         !s_push.non_inc);
            }
            if (!s_push.non_inc) {
                s_push.method += 4;
            }
            s_push.count--;
        } else if ((word & 0xe0000003) == 0x20000000 || (word & 3) == 1) {
            uint32_t target = (word & 3) == 1 ? word & 0xfffffffc
                                              : word & 0x1fffffff;
            if (pg->stats.jumps < 4) {
                fprintf(stderr, "[KELVIN] pushbuffer jump at 0x%08X ->"
                                " 0x%08X\n", s_push.get - 4, target);
                fflush(stderr);
            }
            s_push.get = target;
            transferred = true;
            pg->stats.jumps++;
        } else if ((word & 3) == 2) {
            if (s_push.in_subroutine) {
                fprintf(stderr, "[KELVIN] nested pushbuffer call at"
                                " 0x%08X\n", s_push.get - 4);
            }
            s_push.subroutine = s_push.get;
            s_push.in_subroutine = true;
            s_push.get = word & 0xfffffffc;            /* call */
            transferred = true;
            pg->stats.calls++;
        } else if (word == 0x00020000) {
            if (s_push.in_subroutine) {                /* return */
                s_push.get = s_push.subroutine;
                s_push.in_subroutine = false;
                transferred = true;
            }
        } else if ((word & 0xe0030003) == 0 ||
                   (word & 0xe0030003) == 0x40000000) {
            s_push.method = word & 0x1fff;
            s_push.subchannel = (word >> 13) & 7;
            s_push.count = (word >> 18) & 0x7ff;
            s_push.non_inc = (word & 0xe0030003) == 0x40000000;
        } else {
            static unsigned said;
            if (said++ < 8) {
                fprintf(stderr, "[KELVIN] reserved pushbuffer command 0x%08X"
                                " at 0x%08X\n", word, s_push.get - 4);
            }
        }

        /* Publish progress in batches: a long segment should let the title
         * see its ring space coming back before the whole thing is done.
         * GET must also stay close to what has been read. D3D's fence wait
         * (BLACK's 0x0026CC10) rewrites four reserved words at a fence into
         * WAIT_FOR_IDLE and a notify when GET is at least 0x2400 bytes short
         * of them, then sleeps until the notify interrupts; words the pusher
         * had already read, with GET not yet showing it, never notify. So
         * publish every KELVIN_GET_EVERY words, well inside that margin --
         * but never GET at PUT, which reads as idle and waits for the drain
         * below. */
        if (transferred) {
            /* Word counts bound GET lag only within a contiguous segment.
             * An arbitrary jump or call can skip the fence margin in one
             * word. Finish vertex reads and expose the new location before
             * reading any word there; raster ordering remains in its queue.
             * GET at PUT still waits for the idle drain below. */
            kelvin_render_geometry_sync();
            if (s_push.get != put) {
                publish_get(s_push.get);
            }
            since_publish = 0;
        } else if (++since_publish >= KELVIN_GET_EVERY && s_push.get != put) {
            publish_get(kelvin_render_get_bound(s_push.get, pg->stats.words));
            since_publish = 0;
        }
    }
    if (held) {
        kelvin_unlock();
        kelvin_lock_yield();
    }
    /* GET at PUT reads as idle to D3D, and so does a pusher that stopped: the
     * pixels behind it are written first. */
    if (!kgpu_async_active()) kelvin_render_sync_for(KT_SYNC_IDLE);
    publish_get_idle(s_push.get);
    return words;
}

/* ---- the GPU thread ----------------------------------------------------------- */

volatile DWORD g_kelvin_pusher_tid;
extern void xbox_PinThread(const char *role);
extern int xbox_IdleWaitEnabled(void);
extern int xbox_IdleWait(unsigned int microseconds);

static DWORD WINAPI kelvin_thread(LPVOID arg)
{
    KelvinState *pg = &g_kelvin;
    g_kelvin_pusher_tid = GetCurrentThreadId();
    xbox_PinThread("PUSHER");
    DWORD last_report = GetTickCount();
    (void)arg;

    long long loop_mark = kelvin_ticks();
    int waited = -1;              /* the last loop read no word: KT_WAIT_* */

    g_kelvin_timing = getenv("RECOMP_KELVIN_TIMING") != NULL;
    /* RECOMP_KELVIN_GPU: the D3D11 backend is created on the thread that owns it. */
    if (kgpu_enabled()) {
        if (kgpu_init()) {
            fprintf(stderr, "[KELVIN] GPU backend active\n");
        } else {
            fprintf(stderr, "[KELVIN] GPU backend unavailable; CPU renderer retained\n");
        }
        fflush(stderr);
    }

    for (;;) {
        uint32_t put = kelvin_aperture_r32(KELVIN_USER_DMA_PUT);
        uint32_t done = 0;

        if (g_kelvin_timing > 0) {
            long long now = kelvin_ticks();
            g_kelvin_ticks[KT_LOOP] += now - loop_mark;
            if (waited >= 0) {
                g_kelvin_ticks[waited] += now - loop_mark;
            }
            loop_mark = now;
        }

        /* On hardware a write to the active channel's USER DMA_PUT lands in
         * PFIFO CACHE1_DMA_PUT; D3D's idle checks compare that register with
         * CACHE1_DMA_GET, and a PUT left at 0 reads as work outstanding. */
        aperture_w32(KELVIN_PFIFO_BASE + NV_PFIFO_CACHE1_DMA_PUT, put);

        if (!s_push.started) {
            if (put) {
                /* D3D sets PUT and GET together when it creates the channel;
                 * the first movement of PUT is where execution begins. */
                s_push.get = kelvin_aperture_r32(KELVIN_USER_DMA_GET);
                s_push.dma_instance = GET_MASK(
                    kelvin_aperture_r32(KELVIN_PFIFO_BASE +
                                        NV_PFIFO_CACHE1_DMA_INSTANCE),
                    NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS) << 4;
                s_push.started = true;
                fprintf(stderr, "[KELVIN] pusher started: GET 0x%08X PUT"
                                " 0x%08X, pushbuffer DMA instance 0x%05X%s\n",
                        s_push.get, put, s_push.dma_instance,
                        s_push.dma_instance ? "" : " (physical)");
                fprintf(stderr, "[KELVIN]   PFIFO CACHE1 DMA_PUT 0x%08X"
                                " DMA_GET 0x%08X; DMA object %08X %08X %08X\n",
                        kelvin_aperture_r32(KELVIN_PFIFO_BASE +
                                            NV_PFIFO_CACHE1_DMA_PUT),
                        kelvin_aperture_r32(KELVIN_PFIFO_BASE +
                                            NV_PFIFO_CACHE1_DMA_GET),
                        kelvin_pramin_r32(s_push.dma_instance),
                        kelvin_pramin_r32(s_push.dma_instance + 4),
                        kelvin_pramin_r32(s_push.dma_instance + 8));
                {
                    PushMap map;
                    uint32_t va;
                    int k;
                    push_map(&map);
                    va = pushbuffer_va(&map, s_push.get);
                    fprintf(stderr, "[KELVIN]   first words at GET (VA"
                                    " 0x%08X):", va);
                    for (k = 0; va && k < 8; k++) {
                        fprintf(stderr, " %08X",
                                *(uint32_t *)kelvin_guest(va + k * 4));
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        } else if (put != s_push.get) {
            long long t0 = kelvin_ticks();
            done = pusher_run(pg, put);
            if (g_kelvin_timing > 0) {
                g_kelvin_ticks[KT_PUSHER] += kelvin_ticks() - t0;
            }
            kelvin_render_trace_event(4, t0, kelvin_ticks(), done);
        }

        if (GetTickCount() - last_report > 10000) {
            last_report = GetTickCount();
            kelvin_report();
        }
        waited = done ? -1 : put == s_push.get ? KT_WAIT_PUT
                                : pg->waiting_for_flip ? KT_WAIT_FLIP : KT_WAIT_ACK;
        if (!done) {
            long long t0 = kelvin_ticks();
            /* Nothing to do: wait for PUT to move, or for what the engine is waiting on to be cleared, spinning a
             * little before sleeping. PUT is guest memory, so there is no event to wait on, and a Sleep(0) gives
             * nothing back on an otherwise idle core. */
            const unsigned int spin_limit = xbox_IdleWaitEnabled() ? 100 : 3000;
            unsigned int spins;
            bool was_stalled = pg->waiting_for_nop || pg->waiting_for_context_switch || pg->waiting_for_flip;
            for (spins = 0; spins < spin_limit; spins++) {
                bool stalled;
                _mm_pause();
                stalled = pg->waiting_for_nop || pg->waiting_for_context_switch || pg->waiting_for_flip;
                if (stalled != was_stalled) break;
                if (!stalled && kelvin_aperture_r32(KELVIN_USER_DMA_PUT) != s_push.get) break;
            }
            if (spins == spin_limit && !xbox_IdleWait(200)) Sleep(0);
            kelvin_render_trace_event(3, t0, kelvin_ticks(), (uint32_t)waited);
        }
    }
    return 0;
}

int kelvin_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_NV2A_KELVIN");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

int kelvin_active(void)
{
    /* InterlockedExchange publishes the initialized engine. Acquire its
     * state on every query without a read-modify-write on the shared flag. */
#ifdef _MSC_VER
    return ReadAcquire(&s_active) != 0;
#else
    return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE) != 0;
#endif
}

void kelvin_start(void)
{
    const char *t;

    if (!kelvin_enabled() || kelvin_active()) {
        return;
    }
    kelvin_pgraph_init(&g_kelvin, NULL);
    if (!kelvin_mmio_start(&g_kelvin)) {
        fprintf(stderr, "[KELVIN] NV2A engine not started: its interrupt"
                        " registers could not be modelled\n");
        fflush(stderr);
        return;
    }
    t = getenv("RECOMP_NV2A_FLIP_TIMEOUT_MS");
    if (t) {
        s_flip_timeout_ms = (DWORD)strtoul(t, NULL, 0);
    }
    InterlockedExchange(&s_active, 1);
    s_thread = CreateThread(NULL, 0, kelvin_thread, NULL, 0, NULL);
    fprintf(stderr, "[KELVIN] NV2A engine on the CPU: %s\n",
            s_thread ? "started" : "FAILED to start");
    fflush(stderr);
}

void kelvin_report(void)
{
    const KelvinStats *s = &g_kelvin.stats;
    int i;

    fprintf(stderr, "[KELVIN] %llu words, %llu methods (%llu unhandled),"
                    " %llu jumps, %llu calls | %llu draws (%llu no-op),"
                    " %llu triangles, %llu lines, %llu pixels | %llu clears,"
                    " %llu flips,"
                    " %llu semaphores, %llu blits, %llu notifies\n",
            (unsigned long long)s->words, (unsigned long long)s->methods,
            (unsigned long long)s->unhandled_methods,
            (unsigned long long)s->jumps, (unsigned long long)s->calls,
            (unsigned long long)s->draws, (unsigned long long)s->draws_nop,
            (unsigned long long)s->triangles, (unsigned long long)s->lines,
            (unsigned long long)s->pixels,
            (unsigned long long)s->clears, (unsigned long long)s->flips,
            (unsigned long long)s->semaphores,
            (unsigned long long)s->image_blits,
            (unsigned long long)s->notifies);
    if (s->triangles_bounds_clamped) {
        fprintf(stderr, "[KELVIN]   triangle bounds outside integer range: %llu"
                        " clipped before conversion\n",
                (unsigned long long)s->triangles_bounds_clamped);
    }
    if (s->swizzled_clears) {
        fprintf(stderr, "[KELVIN]   swizzled clears: %llu\n",
                (unsigned long long)s->swizzled_clears);
    }
    if (s->raster_batches) {
        fprintf(stderr, "[KELVIN]   raster batches: %llu; waited for all"
                        " earlier: %llu one-thread, %llu snapshot,"
                        " %llu conflict\n",
                (unsigned long long)s->raster_batches,
                (unsigned long long)s->raster_batches_single,
                (unsigned long long)s->raster_batches_snapshot,
                (unsigned long long)s->raster_batches_conflict);
    }
    if (s->vertices) {
        fprintf(stderr, "[KELVIN]   vertices shaded: %llu, %llu of them in"
                        " parallel; %llu draws set up on the raster threads\n",
                (unsigned long long)s->vertices,
                (unsigned long long)s->vertices_parallel,
                (unsigned long long)s->draws_deferred);
    }
    if (g_kelvin_timing > 0) {
        /* Milliseconds per flip since the last report. */
        static long long prev[KT_COUNT], prev_lanes, prev_loops;
        static uint64_t prev_flips;
        long long lanes, loops, d[KT_COUNT];
        LARGE_INTEGER f;
        uint64_t flips = s->flips - prev_flips;
        double k;
        int i;
        QueryPerformanceFrequency(&f);
        kwork_busy_ticks(&lanes, &loops);
        for (i = 0; i < KT_COUNT; i++) {
            d[i] = g_kelvin_ticks[i] - prev[i];
            prev[i] = g_kelvin_ticks[i];
        }
        k = flips ? 1000.0 / (double)f.QuadPart / (double)flips : 0.0;
        fprintf(stderr, "[KELVIN]   ms per flip over %llu flips: loop %.1f, pusher"
                        " %.1f, draws %.1f (vertex loops %.1f, queue full %.1f);"
                        " draw jobs waited for %.1f;"
                        " drains: clear %.1f, semaphore %.1f, flip %.1f,"
                        " notify %.1f, idle %.1f, other %.1f; workers busy:"
                        " lanes %.1f, vertex items %.1f; no words: for PUT %.1f,"
                " flip %.1f, ack %.1f\n",
                (unsigned long long)flips, d[KT_LOOP] * k, d[KT_PUSHER] * k,
                d[KT_DRAW] * k, d[KT_PRESHADE] * k, d[KT_SUBMIT] * k,
                d[KT_GEOMETRY] * k,
                d[KT_SYNC_CLEAR] * k, d[KT_SYNC_SEMAPHORE] * k,
                d[KT_SYNC_FLIP] * k, d[KT_SYNC_NOTIFY] * k,
                d[KT_SYNC_IDLE] * k, d[KT_SYNC_OTHER] * k,
                (lanes - prev_lanes) * k, (loops - prev_loops) * k,
                d[KT_WAIT_PUT] * k, d[KT_WAIT_FLIP] * k, d[KT_WAIT_ACK] * k);
        {
            extern volatile long long g_kelvin_notify_ticks, g_kelvin_notify_count;
            static long long prev_nt, prev_nc;
            long long nc = g_kelvin_notify_count - prev_nc;
            fprintf(stderr, "[KELVIN]   notify raise->ack: %lld in %.0f us each on average\n", nc,
                    nc ? (double)(g_kelvin_notify_ticks - prev_nt) * 1e6 / (double)f.QuadPart / (double)nc : 0.0);
            prev_nt = g_kelvin_notify_ticks; prev_nc = g_kelvin_notify_count;
        }
        {
            extern volatile long g_kelvin_sync_count[16];
            static long prev_sc[16];
            fprintf(stderr, "[KELVIN]   syncs per flip: clear %.1f semaphore %.1f flip %.1f notify %.1f idle %.1f other %.1f\n",
                    flips ? (g_kelvin_sync_count[KT_SYNC_CLEAR] - prev_sc[KT_SYNC_CLEAR]) / (double)flips : 0.0,
                    flips ? (g_kelvin_sync_count[KT_SYNC_SEMAPHORE] - prev_sc[KT_SYNC_SEMAPHORE]) / (double)flips : 0.0,
                    flips ? (g_kelvin_sync_count[KT_SYNC_FLIP] - prev_sc[KT_SYNC_FLIP]) / (double)flips : 0.0,
                    flips ? (g_kelvin_sync_count[KT_SYNC_NOTIFY] - prev_sc[KT_SYNC_NOTIFY]) / (double)flips : 0.0,
                    flips ? (g_kelvin_sync_count[KT_SYNC_IDLE] - prev_sc[KT_SYNC_IDLE]) / (double)flips : 0.0,
                    flips ? (g_kelvin_sync_count[KT_SYNC_OTHER] - prev_sc[KT_SYNC_OTHER]) / (double)flips : 0.0);
            for (i = 0; i < 16; i++) prev_sc[i] = g_kelvin_sync_count[i];
        }
        prev_lanes = lanes;
        prev_loops = loops;
        prev_flips = s->flips;
        {
            static long long prev_scan[3];
            long long now[3];
            for (i = 0; i < 3; i++) {
                now[i] = g_kelvin_scan[i];
            }
            if (flips) {
                fprintf(stderr, "[KELVIN]   per flip: %lld positions scanned,"
                                " %lld fragments, %lld written\n",
                        (now[0] - prev_scan[0]) / (long long)flips,
                        (now[1] - prev_scan[1]) / (long long)flips,
                        (now[2] - prev_scan[2]) / (long long)flips);
            }
            for (i = 0; i < 3; i++) {
                prev_scan[i] = now[i];
            }
        }
    }
    kelvin_mmio_report();
    kgpu_report();
    fprintf(stderr, "[KELVIN]   pusher: GET 0x%08X PUT 0x%08X%s%s%s%s\n",
            s_push.get, kelvin_aperture_r32(KELVIN_USER_DMA_PUT),
            g_kelvin.waiting_for_nop ? ", waiting for notify ack" : "",
            g_kelvin.waiting_for_context_switch ? ", waiting for context"
                                                  " switch" : "",
            g_kelvin.waiting_for_flip ? ", in flip stall" : "",
            g_kelvin.regs && !(pgraph_reg_r(&g_kelvin, NV_PGRAPH_FIFO) &
                               NV_PGRAPH_FIFO_ACCESS)
                ? ", FIFO access off" : "");
    for (i = 0; i < KU_COUNT; i++) {
        if (s->unsupported[i]) {
            fprintf(stderr, "[KELVIN]   not implemented: %-34s x%llu\n",
                    s_unsupported_names[i],
                    (unsigned long long)s->unsupported[i]);
        }
    }
    fflush(stderr);
}
