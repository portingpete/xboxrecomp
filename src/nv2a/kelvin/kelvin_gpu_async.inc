/* Asynchronous submission, included by kelvin_gpu.c after the draw, clear and flip it hands over.
 *
 * The pusher thread (A) parses the pushbuffer and works out each draw; the Direct3D work of drawing it -- the textures,
 * the uploads, the state, the draw call itself -- is the larger half of its time. kgpu_draw, kgpu_clear and kgpu_flip
 * now copy what they were given into a ring and a thread of its own (B) executes the copies, in order, with the same
 * kgpu_*_now functions as before. Only B touches the Direct3D immediate context.
 *
 * What stays in order:
 *   - Semaphore writes, and GET at PUT (which D3D reads as "the GPU is idle"), are calls queued behind the draws
 *     before them (kgpu_defer_call).
 *   - A notify (an interrupt that tells the title the GPU got here) waits for the queue to drain.
 *   - GET is held at the oldest draw B has not finished (kgpu_get_bound): the title may reuse a draw's vertex memory
 *     once GET is past it, and B reads that memory when it executes the draw. A is stopped from running further ahead
 *     of B than a bounded number of pushbuffer words, so GET stays within D3D's fence-rewrite margin of what A has
 *     read (see KG_GET_LAG in kelvin_render.c).
 *   - At most RECOMP_KGPU_MAX_FLIPS frames (default 2) are queued, so the picture does not trail the game.
 *
 * RECOMP_KGPU_ASYNC=0 runs everything in the caller as before. The compare mode (RECOMP_KGPU_COMPARE) does too. */
#include <synchapi.h>
#pragma comment(lib, "synchronization.lib")

extern volatile DWORD g_black_main_tid;
extern void xbox_PinThread(const char *role);
extern long long xbox_kernel_next_vblank_qpc(long long *period);
extern void pc_input_frame_complete(void);

#define KGQ_CAP ((size_t)32 << 20)
#define KGQ_DRAWS 1024
#define KGQ_ALIGN16(n) (((size_t)(n) + 15u) & ~(size_t)15u)

enum { KGQ_PAD = 1, KGQ_DRAW, KGQ_CLEAR, KGQ_FLIP, KGQ_CALL };
enum { KGD_T = 1, KGD_PS = 2, KGD_CONST = 4, KGD_GVS = 8, KGD_RECS = 16 };

typedef struct KgHdr { uint32_t size, kind; uint64_t pad; } KgHdr;

typedef struct KgDraw {
    uint32_t flags, icount;
    uint64_t epoch;
    uint64_t ao_camera;             /* title camera sampled with this submission, independent of later title frames */
    int32_t cull_enable, cull_ctrl, ccw_front, topology;
    float zbias, zslope;
    uint32_t vcount, stride, inl_bytes, index_limit;
    uint64_t inl_base;               /* where the private vertex data (inline arrays) was, to rebase the pointers into it */
    int32_t slot[KG_SLOTS];
} KgDraw;

typedef struct KgClear {
    KelvinTarget t;
    uint32_t color, depth, stencil, cv, zv, chan, fmt, pad;
    int32_t x0, y0, x1, y1;
} KgClear;

typedef struct KgFlip {
    KelvinTarget t;
    uint32_t fmt, aspect_w, aspect_h, pad;
    long long present_qpc;           /* do not present before this time (0: at once) */
} KgFlip;

typedef struct KgCall {
    void (*fn)(uintptr_t, uintptr_t);
    uintptr_t a, b;
} KgCall;

static int s_kq_mode = -1;                 /* -1 unread, 0 off, 1 on */
static uint8_t *s_kq;
static uint64_t s_kq_wr_local;             /* A: where the next packet goes (published only when complete) */
static volatile LONG64 s_kq_wr;            /* published write position, bytes */
static volatile LONG64 s_kq_rd;            /* B's read position, bytes */
static volatile LONG64 s_kq_done;          /* packets B finished (a pad is not a packet) */
static uint64_t s_kq_posted;               /* A: packets posted */
static volatile LONG s_kq_b_sleep, s_kq_a_sleep;
static uint64_t s_draws_posted;
static volatile LONG64 s_draws_done;
static uint64_t s_dget[KGQ_DRAWS], s_dwords[KGQ_DRAWS];
static uint64_t s_flips_posted;
static volatile LONG64 s_flips_done;
static int s_max_flips = 2;
static int s_pace_debug = -1;
static double s_present_delay = 0.5;       /* in vblank periods; 0 presents as soon as the frame is drawn */
static unsigned s_lag_words = 1280;
static uint64_t s_a_waits_cap, s_a_waits_flip, s_a_waits_space, s_a_drains, s_b_sleeps;
static uint64_t s_a_long_waits;
static volatile long long s_a_wait_ticks, s_b_idle_ticks;      /* QPC ticks A spent waiting for B, B for A */
volatile DWORD g_kgq_tid;

/* What B has, and A's copy of it, so a draw sends only what changed since the last. */
static KelvinTarget a_t, b_t;
static KPixelState a_ps, b_ps;
static float a_consts[192 * 4], b_consts[192 * 4];
static bool a_has_t, a_has_ps, a_has_c;

static bool kgq_enabled(void)
{
    return s_kq_mode > 0;
}

bool kgpu_async_active(void)
{
    return s_kq_mode > 0;
}

/* Until B has finished more than `seen` packets (read before the condition that is being waited on was tested, so a
 * completion in between is not lost), or a millisecond has passed. */
static void kgq_a_wait(LONG64 seen)
{
    int i;
    long long t0 = qpc_now();
    for (i = 0; i < 4000; i++) {
        if (s_kq_done != seen) goto out;
        _mm_pause();
    }
    s_kq_a_sleep = 1;
    MemoryBarrier();
    if (s_kq_done == seen) WaitOnAddress(&s_kq_done, &seen, sizeof(seen), 1);
    s_kq_a_sleep = 0;
out:
    {
        long long dt = qpc_now() - t0;
        s_a_wait_ticks += dt;
        if (dt > G.qpf / 2000) s_a_long_waits++;           /* over half a millisecond */
    }
}
#define KGQ_A_WHILE(cond, counter)     for (;;) { LONG64 seen_ = s_kq_done; if (!(cond)) break; (counter)++; kgq_a_wait(seen_); }

/* Finished frames waiting for their time to be shown, oldest first (GPU thread only). With a delay of more than a
 * vblank period two or three are normally waiting. */
#define KGQ_PEND 6
typedef struct KgPend { long long at; unsigned slot; uint64_t value; uint32_t va, aw, ah; } KgPend;
static KgPend s_pend[KGQ_PEND];
static unsigned s_pend_n;
static long long s_cur_present_qpc;

/* How evenly frames reach the presenter: intervals between publications, 0.25 ms bins up to 10 ms. */
static unsigned s_pubhist[40];
static long long s_pub_prev;

static void kgq_note_publish(void)
{
    long long now = qpc_now();
    if (s_pub_prev) {
        double ms = (double)(now - s_pub_prev) * 1000.0 / (double)G.qpf;
        s_pubhist[ms >= 9.99 ? 39 : (unsigned)(ms * 4.0)]++;
    }
    s_pub_prev = now;
}

static void kgq_pub_oldest(void)
{
    KgPend p = s_pend[0];
    memmove(&s_pend[0], &s_pend[1], (s_pend_n - 1) * sizeof(s_pend[0]));
    s_pend_n--;
    kgq_note_publish();
    xbox_FramebufferPublishGpu(p.slot, p.value, p.va, p.aw, p.ah);
}

/* Show the frames whose time has come. */
static void kgq_pub_poll(void)
{
    while (s_pend_n && qpc_now() >= s_pend[0].at) kgq_pub_oldest();
}

static unsigned long long s_dbg_def, s_dbg_imm, s_dbg_zero;
static long long s_dbg_late_ticks;

static void kgq_publish_frame(unsigned slot, uint64_t value, uint32_t va, uint32_t aspect_w, uint32_t aspect_h)
{
    long long at = s_cur_present_qpc;
    if (!at) s_dbg_zero++;
    else if (at > qpc_now()) s_dbg_def++;
    else { s_dbg_imm++; s_dbg_late_ticks += qpc_now() - at; }
    kgq_pub_poll();
    if (at && at > qpc_now()) {
        if (s_pend_n == KGQ_PEND) kgq_pub_oldest();       /* never expected: more frames in flight than the queue allows */
        s_pend[s_pend_n].at = at; s_pend[s_pend_n].slot = slot; s_pend[s_pend_n].value = value;
        s_pend[s_pend_n].va = va; s_pend[s_pend_n].aw = aspect_w; s_pend[s_pend_n].ah = aspect_h;
        s_pend_n++;
        return;
    }
    /* frames come out in order: anything waiting goes first */
    while (s_pend_n) kgq_pub_oldest();
    kgq_note_publish();
    xbox_FramebufferPublishGpu(slot, value, va, aspect_w, aspect_h);
}

extern int xbox_IdleWaitEnabled(void);
extern int xbox_IdleWait(unsigned int microseconds);

/* How long the last sleep below took: a frame is only slept on when twice that is left before its time. */
static long long s_b_nap_ticks;

static void kgq_b_wait(uint64_t rd)
{
    const int spin_limit = xbox_IdleWaitEnabled() ? 100 : 8000;
    int i;
    for (i = 0; i < spin_limit; i++) {
        if ((uint64_t)s_kq_wr != rd) return;
        if (s_pend_n) kgq_pub_poll();
        _mm_pause();
    }
    if (s_pend_n) {
        /* a frame waits for its time: sleep while it is far off, and stay awake for the end of the wait */
        while ((uint64_t)s_kq_wr == rd && s_pend_n) {
            long long now;
            kgq_pub_poll();
            now = qpc_now();
            if (s_pend_n && s_pend[0].at - now > 2 * s_b_nap_ticks && xbox_IdleWait(200)) {
                s_b_nap_ticks = qpc_now() - now;
                continue;
            }
            _mm_pause();                       /* not SwitchToThread: giving the core away can cost a whole time slice */
        }
        return;
    }
    s_kq_b_sleep = 1;
    MemoryBarrier();
    if ((uint64_t)s_kq_wr == rd) {
        LONG64 seen = (LONG64)rd;
        s_b_sleeps++;
        WaitOnAddress(&s_kq_wr, &seen, sizeof(seen), 1);
    }
    s_kq_b_sleep = 0;
}

/* Room for a packet of `size` bytes (a multiple of 16), after a pad when it would wrap. */
static uint8_t *kgq_alloc(size_t size)
{
    size_t off = (size_t)(s_kq_wr_local & (KGQ_CAP - 1)), pad = 0;
    if (off + size > KGQ_CAP) pad = KGQ_CAP - off;
    KGQ_A_WHILE(s_kq_wr_local + size + pad - (uint64_t)s_kq_rd > KGQ_CAP, s_a_waits_space)
    if (pad) {
        KgHdr *p = (KgHdr *)(s_kq + off);
        p->size = (uint32_t)pad; p->kind = KGQ_PAD; p->pad = 0;
        s_kq_wr_local += pad;
        off = 0;
    }
    return s_kq + off;
}

static void kgq_publish(size_t size)
{
    s_kq_wr_local += size;
    s_kq_posted++;
    _InterlockedExchange64(&s_kq_wr, (LONG64)s_kq_wr_local);
    if (s_kq_b_sleep) WakeByAddressSingle((PVOID)&s_kq_wr);
}

void kgpu_drain(void)
{
    if (!kgq_enabled()) return;
    s_a_drains++;
    {
        uint64_t unused = 0;
        KGQ_A_WHILE((uint64_t)s_kq_done < s_kq_posted, unused)
    }
}

void kgpu_defer_call(void (*fn)(uintptr_t, uintptr_t), uintptr_t a, uintptr_t b)
{
    uint8_t *p;
    KgCall *c;
    size_t size = sizeof(KgHdr) + KGQ_ALIGN16(sizeof(KgCall));
    if (!kgq_enabled()) { fn(a, b); return; }
    p = kgq_alloc(size);
    ((KgHdr *)p)->size = (uint32_t)size; ((KgHdr *)p)->kind = KGQ_CALL; ((KgHdr *)p)->pad = 0;
    c = (KgCall *)(p + sizeof(KgHdr));
    c->fn = fn; c->a = a; c->b = b;
    kgq_publish(size);
}

/* The GET to publish: no further than the oldest draw B has not finished. */
uint32_t kgpu_get_bound(uint32_t get)
{
    uint64_t done = (uint64_t)s_draws_done;
    if (!kgq_enabled() || done >= s_draws_posted) return get;
    return (uint32_t)s_dget[done % KGQ_DRAWS];
}

static bool kgq_guest_ptr(const uint8_t *p)
{
    const uint8_t *base = kelvin_guest(KELVIN_PHYS_WINDOW);
    return p >= base && p < base + KELVIN_PHYS_SIZE;
}

void kgpu_draw(const KGpuDraw *d)
{
    uint8_t *p, *q;
    KgDraw *k;
    uint32_t flags = 0;
    size_t size, tsz = KGQ_ALIGN16(sizeof(KelvinTarget)), psz = KGQ_ALIGN16(sizeof(KPixelState));
    size_t gsz = KGQ_ALIGN16(sizeof(KGpuVsDraw)), csz = sizeof(a_consts), rsz = 0, isz, nsz = 0;
    const uint8_t *inl_lo = NULL, *inl_hi = NULL;
    int a;
    uint64_t words;
    const uint64_t ao_camera = kg_ao_camera_bits();
    if (!kgq_enabled()) { s_ao_draw_camera = ao_camera; kgpu_draw_now(d); return; }
    if (!G.ok || !d->icount || (!d->gvs && !d->vcount)) return;
    if (!a_has_t || memcmp(&a_t, d->t, sizeof(a_t)) != 0) flags |= KGD_T;
    if (!a_has_ps || memcmp(&a_ps, d->ps, sizeof(a_ps)) != 0) flags |= KGD_PS;
    if (d->gvs) {
        flags |= KGD_GVS;
        if (!a_has_c || memcmp(a_consts, d->gvs->constants, sizeof(a_consts)) != 0) flags |= KGD_CONST;
        for (a = 0; a < 16; a++) {
            const KGpuAttrSrc *as = &d->gvs->attr[a];
            const uint8_t *lo, *hi;
            if (!as->fmt) continue;
            lo = as->data + as->lo; hi = as->data + as->hi;
            if (kgq_guest_ptr(lo)) continue;
            if (!inl_lo || lo < inl_lo) inl_lo = lo;
            if (!inl_hi || hi > inl_hi) inl_hi = hi;
        }
        if (inl_lo) nsz = KGQ_ALIGN16((size_t)(inl_hi - inl_lo));
    } else {
        flags |= KGD_RECS;
        rsz = KGQ_ALIGN16((size_t)d->vcount * d->stride * 16);
    }
    isz = KGQ_ALIGN16((size_t)d->icount * 4);
    csz = (flags & KGD_CONST) ? KGQ_ALIGN16(csz) : 0;
    size = sizeof(KgHdr) + KGQ_ALIGN16(sizeof(KgDraw)) + ((flags & KGD_T) ? tsz : 0) + ((flags & KGD_PS) ? psz : 0) +
           ((flags & KGD_GVS) ? gsz : 0) + csz + rsz + isz + nsz;
    if (size > KGQ_CAP / 4) {
        /* too large for the ring: everything before it finishes, then it runs here */
        kgpu_drain();
        G.epoch = s_kick_epoch;
        s_ao_draw_camera = ao_camera;
        kgpu_draw_now(d);
        kg_cache_invalidate();           /* it ran with its own state: what the next queued draw leaves unchanged is not that */
        return;
    }
    /* A may not run further ahead of B than the GET lag allows. */
    words = g_kelvin.stats.words;
    KGQ_A_WHILE(s_draws_posted > (uint64_t)s_draws_done &&
                words - s_dwords[(uint64_t)s_draws_done % KGQ_DRAWS] > s_lag_words, s_a_waits_cap)
    s_dget[s_draws_posted % KGQ_DRAWS] = g_kelvin_word_get;
    s_dwords[s_draws_posted % KGQ_DRAWS] = words;
    s_draws_posted++;

    p = kgq_alloc(size);
    ((KgHdr *)p)->size = (uint32_t)size; ((KgHdr *)p)->kind = KGQ_DRAW; ((KgHdr *)p)->pad = 0;
    k = (KgDraw *)(p + sizeof(KgHdr));
    k->flags = flags; k->icount = d->icount; k->epoch = s_kick_epoch;
    k->ao_camera = ao_camera;
    k->cull_enable = d->cull_enable ? 1 : 0; k->cull_ctrl = d->cull_ctrl; k->ccw_front = d->ccw_front ? 1 : 0;
    k->topology = (int32_t)d->topology;
    k->zbias = d->zbias; k->zslope = d->zslope;
    k->vcount = d->vcount; k->stride = d->stride; k->inl_bytes = (uint32_t)(inl_lo ? inl_hi - inl_lo : 0);
    k->index_limit = d->index_limit;
    k->inl_base = (uint64_t)(uintptr_t)inl_lo;
    for (a = 0; a < KG_SLOTS; a++) k->slot[a] = d->slot[a];
    q = (uint8_t *)k + KGQ_ALIGN16(sizeof(KgDraw));
    if (flags & KGD_T) { memcpy(q, d->t, sizeof(KelvinTarget)); memcpy(&a_t, d->t, sizeof(a_t)); a_has_t = true; q += tsz; }
    if (flags & KGD_PS) { memcpy(q, d->ps, sizeof(KPixelState)); memcpy(&a_ps, d->ps, sizeof(a_ps)); a_has_ps = true; q += psz; }
    if (flags & KGD_GVS) {
        memcpy(q, d->gvs, sizeof(KGpuVsDraw)); q += gsz;
        if (flags & KGD_CONST) {
            memcpy(q, d->gvs->constants, sizeof(a_consts)); memcpy(a_consts, d->gvs->constants, sizeof(a_consts));
            a_has_c = true; q += csz;
        }
    }
    if (flags & KGD_RECS) { memcpy(q, d->recs, (size_t)d->vcount * d->stride * 16); q += rsz; }
    memcpy(q, d->indices, (size_t)d->icount * 4); q += isz;
    if (inl_lo) memcpy(q, inl_lo, (size_t)(inl_hi - inl_lo));
    kgq_publish(size);
}

void kgpu_clear(const KelvinTarget *t, bool color, bool depth, bool stencil,
                uint32_t color_value, uint32_t zs_value, unsigned int channel_mask,
                int x0, int y0, int x1, int y1, unsigned int color_format)
{
    uint8_t *p;
    KgClear *c;
    size_t size = sizeof(KgHdr) + KGQ_ALIGN16(sizeof(KgClear));
    if (!kgq_enabled()) {
        kgpu_clear_now(t, color, depth, stencil, color_value, zs_value, channel_mask, x0, y0, x1, y1, color_format);
        return;
    }
    if (!G.ok) return;
    p = kgq_alloc(size);
    ((KgHdr *)p)->size = (uint32_t)size; ((KgHdr *)p)->kind = KGQ_CLEAR; ((KgHdr *)p)->pad = 0;
    c = (KgClear *)(p + sizeof(KgHdr));
    memcpy(&c->t, t, sizeof(*t));
    c->color = color; c->depth = depth; c->stencil = stencil; c->cv = color_value; c->zv = zs_value;
    c->chan = channel_mask; c->fmt = color_format; c->pad = 0;
    c->x0 = x0; c->y0 = y0; c->x1 = x1; c->y1 = y1;
    kgq_publish(size);
}

void kgpu_flip(const KelvinTarget *t, unsigned int color_format, uint32_t aspect_w, uint32_t aspect_h)
{
    uint8_t *p;
    KgFlip *f;
    size_t size = sizeof(KgHdr) + KGQ_ALIGN16(sizeof(KgFlip));
    if (!kgq_enabled()) { kgpu_flip_now(t, color_format, aspect_w, aspect_h); return; }
    if (!G.ok) return;
    /* The input context counts the game's frames: it is told at the flip, in the order the game polled, not when the
     * picture is later shown (xbox_FramebufferInputByCaller). */
    pc_input_frame_complete();
    KGQ_A_WHILE(s_flips_posted - (uint64_t)s_flips_done >= (uint64_t)s_max_flips, s_a_waits_flip)
    p = kgq_alloc(size);
    ((KgHdr *)p)->size = (uint32_t)size; ((KgHdr *)p)->kind = KGQ_FLIP; ((KgHdr *)p)->pad = 0;
    f = (KgFlip *)(p + sizeof(KgHdr));
    memcpy(&f->t, t, sizeof(*t));
    f->fmt = color_format; f->aspect_w = aspect_w; f->aspect_h = aspect_h; f->pad = 0;
    f->present_qpc = 0;
    if (s_present_delay > 0.0) {
        /* The pusher goes on when the next vblank releases this flip; the frame is shown a fixed time after that, so
         * frames are presented evenly however long each took to draw (a frame that is late is shown as soon as it is
         * done). */
        long long period = 0, next = xbox_kernel_next_vblank_qpc(&period);
        if (next) {
            /* Two flips queued for the same vblank (the game caught up after a late frame) are shown a period apart rather
             * than together, so the picture moves evenly; the later one is never put off by more than a period. */
            static long long last_target;
            long long ideal = next + (long long)((double)period * s_present_delay), t = ideal;
            if (last_target && t < last_target + period * 9 / 10) t = last_target + period * 9 / 10;
            if (t - ideal > period) t = ideal;
            last_target = t;
            f->present_qpc = t;
        }
        if (s_pace_debug < 0) s_pace_debug = getenv("RECOMP_KGPU_PACE_DEBUG") != NULL;
        if (s_pace_debug) {
            static long long prev_pres, prev_post;
            static int shown;
            long long now = qpc_now();
            if (prev_pres && (f->present_qpc - prev_pres > period * 3 / 2 || f->present_qpc - prev_pres < period / 2) && shown++ < 400)
                fprintf(stderr, "[PACE] post +%.2f ms, next vblank in %.2f ms, target step %.2f ms\n",
                        (double)(now - prev_post) * 1000.0 / (double)G.qpf, (double)(next - now) * 1000.0 / (double)G.qpf,
                        (double)(f->present_qpc - prev_pres) * 1000.0 / (double)G.qpf);
            prev_pres = f->present_qpc; prev_post = now;
        }
    }
    s_flips_posted++;
    kgq_publish(size);
}

static DWORD WINAPI kgq_thread(LPVOID unused)
{
    uint64_t rd = 0, done = 0, draws = 0, flips = 0;
    static KGpuVsDraw gv;
    (void)unused;
    xbox_PinThread("GPU");
    g_kgq_tid = GetCurrentThreadId();
    for (;;) {
        const KgHdr *h;
        uint64_t wr = (uint64_t)s_kq_wr;
        if (wr == rd) {
            long long t0 = qpc_now();
            kgq_b_wait(rd);
            s_b_idle_ticks += qpc_now() - t0;
            continue;
        }
        h = (const KgHdr *)(s_kq + (rd & (KGQ_CAP - 1)));
        switch (h->kind) {
        case KGQ_PAD:
            rd += h->size;
            s_kq_rd = (LONG64)rd;
            continue;
        case KGQ_DRAW: {
            const KgDraw *k = (const KgDraw *)(h + 1);
            const uint8_t *q = (const uint8_t *)k + KGQ_ALIGN16(sizeof(KgDraw));
            KGpuDraw d;
            int a;
            if (k->flags & KGD_T) { memcpy(&b_t, q, sizeof(b_t)); q += KGQ_ALIGN16(sizeof(KelvinTarget)); }
            if (k->flags & KGD_PS) { memcpy(&b_ps, q, sizeof(b_ps)); q += KGQ_ALIGN16(sizeof(KPixelState)); }
            memset(&d, 0, sizeof(d));
            d.t = &b_t; d.ps = &b_ps;
            d.cull_enable = k->cull_enable != 0; d.cull_ctrl = k->cull_ctrl; d.ccw_front = k->ccw_front != 0;
            d.topology = (unsigned int)k->topology; d.index_limit = k->index_limit;
            d.zbias = k->zbias; d.zslope = k->zslope;
            d.clean = ((k->flags & KGD_T) ? 0u : KG_CLEAN_T) | ((k->flags & KGD_PS) ? 0u : KG_CLEAN_PS);
            for (a = 0; a < KG_SLOTS; a++) d.slot[a] = k->slot[a];
            if (k->flags & KGD_GVS) {
                memcpy(&gv, q, sizeof(gv)); q += KGQ_ALIGN16(sizeof(KGpuVsDraw));
                if (k->flags & KGD_CONST) { memcpy(b_consts, q, sizeof(b_consts)); q += KGQ_ALIGN16(sizeof(b_consts)); }
                gv.constants = b_consts;
                d.gvs = &gv;
            }
            if (k->flags & KGD_RECS) {
                d.recs = (const float *)q; d.vcount = k->vcount; d.stride = k->stride;
                q += KGQ_ALIGN16((size_t)k->vcount * k->stride * 16);
            }
            d.indices = (const uint32_t *)q; d.icount = k->icount;
            q += KGQ_ALIGN16((size_t)k->icount * 4);
            if (k->inl_bytes) {
                for (a = 0; a < 16; a++) {
                    if (gv.attr[a].fmt && !kgq_guest_ptr(gv.attr[a].data + gv.attr[a].lo)) {
                        gv.attr[a].data = q + ((intptr_t)(uintptr_t)gv.attr[a].data - (intptr_t)k->inl_base);
                    }
                }
            }
            G.epoch = k->epoch;
            s_ao_draw_camera = k->ao_camera;
            kgpu_draw_now(&d);
            draws++;
            break;
        }
        case KGQ_CLEAR: {
            const KgClear *c = (const KgClear *)(h + 1);
            kgpu_clear_now(&c->t, c->color != 0, c->depth != 0, c->stencil != 0, c->cv, c->zv, c->chan, c->x0, c->y0,
                           c->x1, c->y1, c->fmt);
            break;
        }
        case KGQ_FLIP: {
            const KgFlip *f = (const KgFlip *)(h + 1);
            s_cur_present_qpc = f->present_qpc;
            kgpu_flip_now(&f->t, f->fmt, f->aspect_w, f->aspect_h);
            s_cur_present_qpc = 0;
            flips++;
            break;
        }
        case KGQ_CALL: {
            const KgCall *c = (const KgCall *)(h + 1);
            c->fn(c->a, c->b);
            break;
        }
        default:
            break;
        }
        if (s_pend_n) kgq_pub_poll();
        rd += h->size;
        s_kq_rd = (LONG64)rd;
        s_draws_done = (LONG64)draws;
        s_flips_done = (LONG64)flips;
        done++;
        _InterlockedExchange64(&s_kq_done, (LONG64)done);
        if (s_kq_a_sleep) WakeByAddressAll((PVOID)&s_kq_done);
    }
    return 0;
}

static void kgq_start(void)
{
    const char *e = getenv("RECOMP_KGPU_ASYNC");
    if (s_kq_mode >= 0) return;
    s_kq_mode = 0;
    if (e && *e == '0') return;
    if (kgpu_compare_enabled()) return;
    if (!(e && *e)) {
        /* The submission thread and the pusher each keep a processor busy next to the game's own thread: on a small
         * machine that costs more than it gains. RECOMP_KGPU_ASYNC=1 forces it. */
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (si.dwNumberOfProcessors < 8) {
            fprintf(stderr, "[KGPU] %lu logical processors: submitting from the pusher thread (RECOMP_KGPU_ASYNC=1 overrides)\n",
                    (unsigned long)si.dwNumberOfProcessors);
            return;
        }
    }
    s_kq = (uint8_t *)VirtualAlloc(NULL, KGQ_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!s_kq) return;
    e = getenv("RECOMP_KGPU_MAX_FLIPS");
    if (e && *e) { s_max_flips = atoi(e); if (s_max_flips < 1) s_max_flips = 1; if (s_max_flips > 8) s_max_flips = 8; }
    e = getenv("RECOMP_KGPU_PRESENT_DELAY");
    if (e && *e) { s_present_delay = atof(e); if (s_present_delay < 0.0) s_present_delay = 0.0; if (s_present_delay > 4.0) s_present_delay = 4.0; }
    if (s_present_delay > 0.0 && s_max_flips < 3 && !getenv("RECOMP_KGPU_MAX_FLIPS")) s_max_flips = 3;
    e = getenv("RECOMP_KGPU_LAG_WORDS");
    if (e && *e) { int v = atoi(e); if (v < 64) v = 64; if (v > 1800) v = 1800; s_lag_words = (unsigned)v; }
    s_kq_mode = 1;
    {
        HANDLE th = CreateThread(NULL, 0, kgq_thread, NULL, 0, NULL);
        if (!th) { s_kq_mode = 0; return; }
        CloseHandle(th);
    }
    xbox_FramebufferInputByCaller(1);
    fprintf(stderr, "[KGPU] asynchronous submission: %u MiB queue, GET lag %u words, %d frames in flight\n",
            (unsigned)(KGQ_CAP >> 20), s_lag_words, s_max_flips);
}

static void kgq_report(void)
{
    static uint64_t p_cap, p_flip, p_space, p_drain, p_sleep, p_flips, p_long;
    static long long p_a, p_b;
    double k;
    if (!kgq_enabled()) return;
    k = G.frames > p_flips ? 1000.0 / (double)G.qpf / (double)(G.frames - p_flips) : 0.0;
    fprintf(stderr, "[KGPU] async: per flip A waited for B %.2f ms, B waited for A %.2f ms; waits %llu on the GET lag, "
                    "%llu on queued frames, %llu on ring space; %llu drains; B slept %llu times; %llu waits over 0.5 ms\n",
            (s_a_wait_ticks - p_a) * k, (s_b_idle_ticks - p_b) * k,
            (unsigned long long)(s_a_waits_cap - p_cap), (unsigned long long)(s_a_waits_flip - p_flip),
            (unsigned long long)(s_a_waits_space - p_space), (unsigned long long)(s_a_drains - p_drain),
            (unsigned long long)(s_b_sleeps - p_sleep), (unsigned long long)(s_a_long_waits - p_long));
    p_long = s_a_long_waits;
    {
        char line[480];
        int len = 0, b;
        for (b = 0; b < 40; b++) {
            if (s_pubhist[b] && len < 420) len += snprintf(line + len, sizeof(line) - len, " %.2f:%u", b * 0.25, s_pubhist[b]);
        }
        line[len] = 0;
        fprintf(stderr, "[KGPU] presenter intervals by 0.25 ms bin:%s\n", line);
        fprintf(stderr, "[KGPU] paced frames: %llu waited for their time, %llu late (mean %.2f ms late), %llu unpaced\n", s_dbg_def, s_dbg_imm,
                s_dbg_imm ? (double)s_dbg_late_ticks * 1000.0 / (double)G.qpf / (double)s_dbg_imm : 0.0, s_dbg_zero);
        memset(s_pubhist, 0, sizeof(s_pubhist));
    }
    p_cap = s_a_waits_cap; p_flip = s_a_waits_flip; p_space = s_a_waits_space; p_drain = s_a_drains; p_sleep = s_b_sleeps;
    p_a = s_a_wait_ticks; p_b = s_b_idle_ticks; p_flips = G.frames;
}
