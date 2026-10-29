/* The native Video Settings page. See pc_video_menu.h. */
#include "pc_video_menu.h"
#include "pc_video.h"
#include "pc_crosshair.h"
#include "../input/pc_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---- rows ------------------------------------------------------------------------------------------- */

enum {
    R_MODE = PCV_ROW_MODE, R_WINDOW = PCV_ROW_WINDOW, R_INTERNAL = PCV_ROW_INTERNAL, R_AA = PCV_ROW_AA, R_AF = PCV_ROW_AF,
    R_DOF = PCV_ROW_DOF, R_SHARPEN = PCV_ROW_SHARPEN, R_ASPECT = PCV_ROW_ASPECT, R_FOV = PCV_ROW_FOV, R_SCALE = PCV_ROW_SCALE,
    R_AO_METHOD = PCV_ROW_AO_METHOD, R_AO_QUALITY = PCV_ROW_AO_QUALITY, R_REFLECTIONS = PCV_ROW_REFLECTIONS,
    R_RAY_TRACING = PCV_ROW_RAY_TRACING,
    R_FILTER = PCV_ROW_FILTER, R_VSYNC = PCV_ROW_VSYNC, R_FPS = PCV_ROW_FPS, R_BLUR = PCV_ROW_BLUR, R_CROSSHAIR = PCV_ROW_CROSSHAIR,
    R_HITMARKERS = PCV_ROW_HITMARKERS, R_OPTIONAL_SECONDARIES = PCV_ROW_OPTIONAL_SECONDARIES,
    R_LOD_DISTANCE = PCV_ROW_LOD_DISTANCE, R_BULLET_DAMAGE = PCV_ROW_BULLET_DAMAGE,
    R_ENEMY_HEALTH = PCV_ROW_ENEMY_HEALTH, R_HEADSHOT_INSTAKILL = PCV_ROW_HEADSHOT_INSTAKILL, R_RESET = PCV_ROW_RESET,
    R_DONE = PCV_ROW_DONE, R_COUNT = PCV_ROW_COUNT, R_AIM_MODE = PCV_ROW_AIM_MODE, R_SPRINT = PCV_ROW_SPRINT, R_JUMP = PCV_ROW_JUMP
};
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_ACCEPT, A_BACK, A_OPEN };

#define KEY_ESC 0x001u
#define KEY_BACKSPACE 0x00Eu
#define KEY_ENTER 0x01Cu
#define KEY_SPACE 0x039u
#define KEY_NUMENTER 0x11Cu
#define KEY_UP 0x148u
#define KEY_LEFT 0x14Bu
#define KEY_RIGHT 0x14Du
#define KEY_DOWN 0x150u
#define KEY_F11 0x057u
#define KEY_V 0x02Fu
#define KEY_W 0x011u
#define KEY_A 0x01Eu
#define KEY_S 0x01Fu
#define KEY_D 0x020u

static SRWLOCK m_lock = SRWLOCK_INIT;
static int m_open, m_sel, m_force;
static void (*m_native_opener)(void);
static volatile LONG m_hint_hidden;
static ULONGLONG m_seen_tick;
static unsigned char m_key_swallowed[0x200];
static unsigned m_button_swallowed;
static unsigned m_pad_prev;
static int m_pad_hold;
static ULONGLONG m_pad_repeat[4];

/* Pad bits as tracked by the report filter. */
enum { P_UP = 1, P_DOWN = 2, P_LEFT = 4, P_RIGHT = 8, P_A = 16, P_B = 32, P_OPEN = 64, P_DIR = P_UP | P_DOWN | P_LEFT | P_RIGHT, P_ALL = 127 };

void pc_video_menu_page_seen(void)
{
    m_seen_tick = GetTickCount64();     /* a word-sized store; the reader tolerates a stale value */
}

static int available_locked(void)
{
    ULONGLONG now = GetTickCount64(), seen = m_seen_tick;
    return m_force || (seen && now - seen < 400);
}

int pc_video_menu_is_open(void)
{
    int open;
    AcquireSRWLockShared(&m_lock);
    open = m_open;
    ReleaseSRWLockShared(&m_lock);
    return open;
}

/* ---- the window sizes the page offers --------------------------------------------------------------------- */

static const unsigned k_sizes[][2] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 },
    { 1366, 768 }, { 1400, 1050 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
    { 1920, 1440 }, { 2048, 1536 }, { 2560, 1080 }, { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 }, { 3840, 1080 }, { 3840, 1600 },
    { 3840, 2160 }, { 3840, 2400 }, { 5120, 1440 }, { 5120, 2160 }, { 7680, 2160 }
};
#define N_SIZES (sizeof(k_sizes) / sizeof(k_sizes[0]))

/* The offered sizes that fit the screen, with the current one included wherever it falls. */
static unsigned size_list(const PcVideoSettings *cur, unsigned out[][2], unsigned cap, unsigned *index)
{
    const unsigned sw = (unsigned)GetSystemMetrics(SM_CXSCREEN), sh = (unsigned)GetSystemMetrics(SM_CYSCREEN);
    unsigned n = 0, i, at = (unsigned)-1;
    const uint64_t cur_area = (uint64_t)cur->window_width * cur->window_height;
    for (i = 0; i < N_SIZES && n < cap; i++) {
        const unsigned w = k_sizes[i][0], h = k_sizes[i][1];
        if (w == cur->window_width && h == cur->window_height) at = n;
        else {
            if (at == (unsigned)-1 && (uint64_t)w * h > cur_area && (w > sw || h > sh) == 0) {
                /* the current size is not a preset: it goes before the first larger one */
                out[n][0] = cur->window_width; out[n][1] = cur->window_height; at = n++;
                if (n >= cap) break;
            }
            if (w > sw || h > sh) continue;
        }
        out[n][0] = w; out[n][1] = h; n++;
    }
    if (at == (unsigned)-1 && n < cap) { out[n][0] = cur->window_width; out[n][1] = cur->window_height; at = n++; }
    if (at == (unsigned)-1) at = 0;
    *index = at;
    return n;
}

/* ---- changing a row ------------------------------------------------------------------------------------- */

static unsigned wrap(int v, unsigned n) { return (unsigned)(((v % (int)n) + (int)n) % (int)n); }
static unsigned cycle_percent(unsigned current, const unsigned *values, unsigned count, int dir)
{
    unsigned i;
    for (i = 0; i < count && values[i] != current; i++) {}
    if (i == count) i = 0;
    return values[wrap((int)i + dir, count)];
}

static unsigned row_lock_bit(int row);

/* Ray tracing without a GPU that can do it cannot be changed either. */
static int rt_unavailable(int row) { return row == R_RAY_TRACING && pc_video_rt_support() == PCV_RT_UNAVAILABLE; }

int pc_video_menu_row_locked(int row)
{
    return (pc_video_locked() & row_lock_bit(row)) != 0 || rt_unavailable(row);
}

int pc_video_menu_row_change(int row, int dir)
{
    PcVideoSettings s;
    unsigned locked = pc_video_locked(), i;
    if (!dir || row < 0 || row >= R_RESET || (locked & row_lock_bit(row)) || rt_unavailable(row)) return 0;
    dir = dir < 0 ? -1 : 1;
    pc_video_get(&s);
    switch (row) {
    case R_MODE:
        s.borderless = !s.borderless; break;
    case R_WINDOW: {
        unsigned list[40][2], at, n;
        n = size_list(&s, list, 40, &at);
        at = wrap((int)at + dir, n);
        s.window_width = list[at][0]; s.window_height = list[at][1];
        break;
    }
    case R_INTERNAL:
        for (i = 0; i < pc_video_internal_height_count && pc_video_internal_heights[i] != s.internal_height; i++) {}
        if (i >= pc_video_internal_height_count) i = 1;
        s.internal_height = pc_video_internal_heights[wrap((int)i + dir, pc_video_internal_height_count)];
        break;
    case R_AA:
        s.aa = wrap((int)s.aa + dir, PCV_AA_COUNT); break;
    case R_AF:
        for (i = 0; i < pc_video_af_level_count && pc_video_af_levels[i] != s.af; i++) {}
        if (i >= pc_video_af_level_count) i = 0;
        s.af = pc_video_af_levels[wrap((int)i + dir, pc_video_af_level_count)];
        break;
    case R_DOF:
        s.dof = wrap((int)s.dof + dir, PCV_STEP_COUNT); break;
    case R_SHARPEN:
        s.sharpen = wrap((int)s.sharpen + dir, PCV_STEP_COUNT); break;
    case R_AO_METHOD:
        s.ao_method = wrap((int)s.ao_method + dir, PCV_AO_COUNT); break;
    case R_AO_QUALITY:
        s.ao_quality = wrap((int)s.ao_quality + dir, PCV_AOQ_COUNT); break;
    case R_REFLECTIONS:
        s.reflections = wrap((int)s.reflections + dir, PCV_REFL_COUNT); break;
    case R_RAY_TRACING:
        s.ray_tracing = !s.ray_tracing; break;
    case R_ASPECT:
        s.aspect = wrap((int)s.aspect + dir, PCV_ASPECT_COUNT); break;
    case R_FOV: {
        int deg;
        /* Original, then 35 .. 120 degrees in steps of 5. */
        deg = s.fov == 0.0 ? 30 : (int)(s.fov / 5.0 + 0.5) * 5;
        deg += dir * 5;
        if (deg < 30) deg = 120;                 /* wrap: below Original is the widest */
        if (deg > 120) deg = 30;
        if (deg < 35) s.fov = 0.0; else s.fov = (double)deg;
        break;
    }
    case R_SCALE:
        s.scale = wrap((int)s.scale + dir, 3); break;
    case R_FILTER:
        s.nearest = !s.nearest; break;
    case R_VSYNC:
        s.vsync = !s.vsync; break;
    case R_FPS:
        for (i = 0; i < pc_video_fps_limit_count && pc_video_fps_limits[i] != s.fps_limit; i++) {}
        if (i >= pc_video_fps_limit_count) i = 0;
        s.fps_limit = pc_video_fps_limits[wrap((int)i + dir, pc_video_fps_limit_count)];
        break;
    case R_BLUR:
        s.motion_blur_off = !s.motion_blur_off; break;
    case R_CROSSHAIR:
        s.crosshair = wrap((int)s.crosshair + dir, PCX_STYLE_COUNT); break;
    case R_HITMARKERS:
        s.hitmarkers = !s.hitmarkers; break;
    case R_OPTIONAL_SECONDARIES:
        s.optional_secondaries = !s.optional_secondaries; break;
    case R_LOD_DISTANCE:
        s.lod_distance = cycle_percent(s.lod_distance, pc_video_lod_distances, pc_video_lod_distance_count, dir); break;
    case R_BULLET_DAMAGE:
        s.bullet_damage = cycle_percent(s.bullet_damage, pc_video_bullet_damages, pc_video_bullet_damage_count, dir); break;
    case R_ENEMY_HEALTH:
        s.enemy_health = cycle_percent(s.enemy_health, pc_video_enemy_healths, pc_video_enemy_health_count, dir); break;
    case R_HEADSHOT_INSTAKILL:
        s.headshot_instakill = !s.headshot_instakill; break;
    case R_SPRINT: s.sprint = !s.sprint; break;
    case R_JUMP: s.jump = !s.jump; break;
    case R_AIM_MODE:
        s.aim_hold = !s.aim_hold; break;
    default: return 0;
    }
    return pc_video_set(&s);
}

void pc_video_menu_reset_settings(void) { pc_video_reset(); }

static void act(int a)
{
    int row = -1, dir = 0, reset = 0;
    void (*request)(void) = NULL;
    AcquireSRWLockExclusive(&m_lock);
    switch (a) {
    case A_UP: if (m_open) m_sel = (m_sel + R_COUNT - 1) % R_COUNT; break;
    case A_DOWN: if (m_open) m_sel = (m_sel + 1) % R_COUNT; break;
    case A_LEFT: if (m_open) { row = m_sel; dir = -1; } break;
    case A_RIGHT: if (m_open) { row = m_sel; dir = 1; } break;
    case A_ACCEPT:
        if (!m_open) break;
        if (m_sel == R_RESET) reset = 1;
        else if (m_sel == R_DONE) m_open = 0;
        else { row = m_sel; dir = 1; }
        break;
    case A_BACK: m_open = 0; break;
    case A_OPEN:
        if (available_locked()) {
            if (m_native_opener) request = m_native_opener;
            else { m_open = 1; m_sel = 0; }
        }
        break;
    default: break;
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (request) request();
    if (reset) pc_video_menu_reset_settings();
    else if (row >= 0) pc_video_menu_row_change(row, dir);
}

void pc_video_menu_open(void)
{
    void (*request)(void);
    AcquireSRWLockExclusive(&m_lock);
    m_seen_tick = GetTickCount64();
    request = m_native_opener;
    if (!request && !m_open) { m_open = 1; m_sel = 0; }
    ReleaseSRWLockExclusive(&m_lock);
    if (request) request();
}

void pc_video_menu_set_native_opener(void (*request)(void))
{
    AcquireSRWLockExclusive(&m_lock);
    m_native_opener = request;
    if (request) m_open = 0;
    ReleaseSRWLockExclusive(&m_lock);
}

void pc_video_menu_hide_hint(int hide) { InterlockedExchange(&m_hint_hidden, hide ? 1 : 0); }

void pc_video_menu_test_action(int action) { act(action); }
void pc_video_menu_test_force_available(int on) { AcquireSRWLockExclusive(&m_lock); m_force = on; ReleaseSRWLockExclusive(&m_lock); }
int pc_video_menu_test_selected(void) { return m_sel; }

/* ---- the input filters --------------------------------------------------------------------------------------- */

static int f_key(unsigned id, int down, int repeat)
{
    int action = -1, take = 0;
    if (id >= 0x200 || id == KEY_F11) return 0;
    AcquireSRWLockExclusive(&m_lock);
    if (!down) {
        take = m_key_swallowed[id];
        m_key_swallowed[id] = 0;
    } else if (m_open) {
        take = 1;
        m_key_swallowed[id] = 1;
        switch (id) {
        case KEY_UP: case KEY_W: action = A_UP; break;
        case KEY_DOWN: case KEY_S: action = A_DOWN; break;
        case KEY_LEFT: case KEY_A: action = A_LEFT; break;
        case KEY_RIGHT: case KEY_D: action = A_RIGHT; break;
        case KEY_ENTER: case KEY_NUMENTER: case KEY_SPACE: if (!repeat) action = A_ACCEPT; break;
        case KEY_ESC: case KEY_BACKSPACE: if (!repeat) action = A_BACK; break;
        default: break;
        }
    } else if (id == KEY_V && !repeat && available_locked()) {
        take = 1;
        m_key_swallowed[id] = 1;
        action = A_OPEN;
    } else if (id == KEY_V && repeat && m_key_swallowed[id]) {
        take = 1;
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (action >= 0) act(action);
    return take;
}

static int f_button(int button, int down)
{
    int action = -1, take = 0;
    if (button < 0 || button > 4) return 0;
    AcquireSRWLockExclusive(&m_lock);
    if (!down) {
        take = (m_button_swallowed >> button) & 1u;
        m_button_swallowed &= ~(1u << button);
    } else if (m_open) {
        take = 1;
        m_button_swallowed |= 1u << button;
        action = button == 0 ? A_ACCEPT : button == 1 ? A_BACK : -1;
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (action >= 0) act(action);
    return take;
}

static int f_wheel(int delta)
{
    int open;
    AcquireSRWLockShared(&m_lock);
    open = m_open;
    ReleaseSRWLockShared(&m_lock);
    if (!open) return 0;
    act(delta > 0 ? A_UP : A_DOWN);
    return 1;
}

static unsigned pad_bits(const uint8_t out[20])
{
    unsigned b = 0;
    const int lx = (int16_t)(out[12] | (out[13] << 8)), ly = (int16_t)(out[14] | (out[15] << 8));
    if ((out[2] & 1) || ly > 20000) b |= P_UP;
    if ((out[2] & 2) || ly < -20000) b |= P_DOWN;
    if ((out[2] & 4) || lx < -20000) b |= P_LEFT;
    if ((out[2] & 8) || lx > 20000) b |= P_RIGHT;
    if (out[4] > 100) b |= P_A;
    if (out[5] > 100) b |= P_B;
    if (out[9] > 100) b |= P_OPEN;           /* the White button: the right shoulder of an XInput pad */
    return b;
}

static int f_report(uint8_t out[20], int pad_present)
{
    unsigned held = pad_present ? pad_bits(out) : 0, edges;
    ULONGLONG now = GetTickCount64();
    int actions[8], n = 0, take, i;
    AcquireSRWLockExclusive(&m_lock);
    edges = held & ~m_pad_prev;
    if (!m_open) {
        if ((edges & P_OPEN) && available_locked()) { actions[n++] = A_OPEN; m_pad_hold = 1; }
    } else {
        static const unsigned dir_bit[4] = { P_UP, P_DOWN, P_LEFT, P_RIGHT };
        static const int dir_action[4] = { A_UP, A_DOWN, A_LEFT, A_RIGHT };
        for (i = 0; i < 4; i++) {
            if (!(held & dir_bit[i])) continue;
            if (edges & dir_bit[i]) { actions[n++] = dir_action[i]; m_pad_repeat[i] = now + 400; }
            else if (now >= m_pad_repeat[i]) { actions[n++] = dir_action[i]; m_pad_repeat[i] = now + 90; }
        }
        if (edges & P_A) actions[n++] = A_ACCEPT;
        if (edges & P_B) { actions[n++] = A_BACK; m_pad_hold = 1; }
    }
    m_pad_prev = held;
    take = m_open || m_pad_hold || n > 0;
    /* Whatever closed or opened the page is not the game's press: keep the pad neutral until it is let go. */
    if (m_pad_hold && !(held & P_ALL)) m_pad_hold = 0;
    ReleaseSRWLockExclusive(&m_lock);
    for (i = 0; i < n; i++) act(actions[i]);
    if (!take) {
        AcquireSRWLockShared(&m_lock);
        take = m_open || m_pad_hold;
        ReleaseSRWLockShared(&m_lock);
    }
    return take;
}

static const PcInputFilter k_filter = { f_key, f_button, f_wheel, f_report, NULL };

void pc_video_menu_install(void)
{
    const char *force = getenv("RECOMP_VIDEO_MENU_FORCE");
    if (force && *force && *force != '0') m_force = 1;
    pc_input_set_filter(&k_filter);
}

/* ---- drawing ------------------------------------------------------------------------------------------------- */

static uint32_t *m_buf;
static unsigned m_buf_w, m_buf_h;
static uint64_t m_last_key;
static int m_last_visible;

#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static void blend_px(uint32_t *dst, unsigned r, unsigned g, unsigned b, unsigned a)
{
    /* source premultiplied; a is its alpha 0..255 */
    uint32_t d = *dst;
    unsigned inv = 255 - a;
    unsigned dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = d >> 24;
    dr = r + (dr * inv + 127) / 255; dg = g + (dg * inv + 127) / 255; db = b + (db * inv + 127) / 255; da = a + (da * inv + 127) / 255;
    *dst = ((uint32_t)da << 24) | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | db;
}

static void fill_rect(int x, int y, int w, int h, uint32_t argb)
{
    int xx, yy;
    const unsigned a = argb >> 24, r = ((argb >> 16) & 255) * a / 255, g = ((argb >> 8) & 255) * a / 255, b = (argb & 255) * a / 255;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)m_buf_w) w = (int)m_buf_w - x;
    if (y + h > (int)m_buf_h) h = (int)m_buf_h - y;
    for (yy = 0; yy < h; yy++) {
        uint32_t *row = m_buf + (size_t)(y + yy) * m_buf_w + x;
        for (xx = 0; xx < w; xx++) blend_px(row + xx, r, g, b, a);
    }
}

static void frame_rect(int x, int y, int w, int h, int t, uint32_t argb)
{
    fill_rect(x, y, w, t, argb); fill_rect(x, y + h - t, w, t, argb);
    fill_rect(x, y + t, t, h - 2 * t, argb); fill_rect(x + w - t, y + t, t, h - 2 * t, argb);
}

/* A triangle pointing left (dir -1) or right (dir 1): s pixels tall, two thirds as deep, centred on cy. */
static void triangle(int x, int cy, int s, int dir, uint32_t argb)
{
    const int depth = s * 2 / 3;
    int i;
    for (i = 0; i < s; i++) {
        const int c = i * 2 + 1 - s;                          /* -s+1 .. s-1: distance from the middle, doubled */
        const int width = depth * (s - (c < 0 ? -c : c)) / s + 1;
        fill_rect(dir > 0 ? x : x + depth - width, cy - s / 2 + i, width, 1, argb);
    }
}

static HFONT make_font(int px, int weight)
{
    static const wchar_t *const faces[] = { L"Bahnschrift", L"Segoe UI", L"Arial" };
    unsigned i;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT chosen = NULL;
    for (i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
        HFONT f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, faces[i]);
        wchar_t got[64] = L"";
        HGDIOBJ old;
        if (!f) continue;
        old = SelectObject(dc, f);
        GetTextFaceW(dc, 64, got);
        SelectObject(dc, old);
        if (!_wcsnicmp(got, faces[i], wcslen(faces[i])) || i + 1 == sizeof(faces) / sizeof(faces[0])) { chosen = f; break; }
        DeleteObject(f);
    }
    DeleteDC(dc);
    return chosen;
}

/* align: 0 left, 1 right, 2 centre; (x, y) is the top-left / top-right / top-centre of the text. */
static int draw_text(HFONT font, const wchar_t *text, int x, int y, int align, uint32_t rgb, unsigned alpha)
{
    HDC dc;
    SIZE sz;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HGDIOBJ old_font, old_bmp;
    int len = (int)wcslen(text), w, h, ox, oy, xx, yy;
    const unsigned cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
    if (!len || !font) return 0;
    dc = CreateCompatibleDC(NULL);
    old_font = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text, len, &sz);
    w = sz.cx + 8; h = sz.cy + 4;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { SelectObject(dc, old_font); DeleteDC(dc); return sz.cx; }
    old_bmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 4, 2, text, len);
    GdiFlush();
    ox = align == 1 ? x - sz.cx - 4 : align == 2 ? x - sz.cx / 2 - 4 : x - 4;
    oy = y - 2;
    for (yy = 0; yy < h; yy++) {
        const int py = oy + yy;
        const uint32_t *src = (const uint32_t *)bits + (size_t)yy * w;
        if (py < 0 || py >= (int)m_buf_h) continue;
        for (xx = 0; xx < w; xx++) {
            const int px = ox + xx;
            const unsigned cov = (src[xx] >> 8) & 255;          /* grayscale antialiasing: any channel */
            unsigned a;
            if (!cov || px < 0 || px >= (int)m_buf_w) continue;
            a = cov * alpha / 255;
            blend_px(m_buf + (size_t)py * m_buf_w + px, cr * a / 255, cg * a / 255, cb * a / 255, a);
        }
    }
    SelectObject(dc, old_bmp); DeleteObject(bmp);
    SelectObject(dc, old_font); DeleteDC(dc);
    return sz.cx;
}

static int text_width(HFONT font, const wchar_t *text)
{
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old); DeleteDC(dc);
    return sz.cx;
}

static const char *const k_row_label[R_COUNT] = {
    "WINDOW MODE", "WINDOW SIZE", "INTERNAL RESOLUTION", "ANTI-ALIASING", "ANISOTROPIC FILTERING", "DEPTH OF FIELD",
    "SHARPENING", "AO METHOD", "AO QUALITY", "REFLECTIONS", "RAY TRACING (EXPERIMENTAL)", "ASPECT RATIO", "FIELD OF VIEW", "SCALING", "SCALING FILTER", "VSYNC", "FRAME RATE LIMIT",
    "MOTION BLUR", "CROSSHAIR", "HIT MARKERS", "OPTIONAL SECONDARY OBJECTIVES", "LOD DISTANCE", "BULLET DAMAGE",
    "ENEMY HEALTH", "INSTANT-KILL HEADSHOTS", "AIM MODE", "SPRINT", "JUMP", "RESET TO DEFAULTS", "DONE"
};

static const char *const k_row_label_pt[R_COUNT] = {
    "MODO DE JANELA", "RESOLUÇÃO", "RESOLUÇÃO INTERNA", "SUAVIZAÇÃO DE SERRILHADO", "FILTRAGEM ANISOTRÓPICA", "PROFUNDIDADE DE CAMPO",
    "NITIDEZ", "MÉTODO DE OCLUSÃO", "QUALIDADE DA OCLUSÃO", "REFLEXOS", "RAY TRACING (EXPERIMENTAL)", "PROPORÇÃO", "FOV", "ESCALA DA IMAGEM", "FILTRO DE ESCALA", "SINCRONIZAÇÃO VERTICAL", "LIMITE DE QUADROS",
    "DESFOQUE DE MOVIMENTO", "MIRA", "MARCADORES DE ACERTO", "OBJETIVOS SECUNDÁRIOS OPCIONAIS", "DISTÂNCIA DE LOD", "DANO DAS BALAS",
    "VIDA DOS INIMIGOS", "TIROS NA CABEÇA LETAIS", "MODO DE MIRA", "RESTAURAR PADRÕES", "CONCLUÍDO"
};

static int video_menu_portuguese(void)
{
    const char *language = getenv("RECOMP_LANGUAGE");
    return language && !_stricmp(language, "pt-BR");
}

static const char *localized_video_value(const char *value)
{
    static const struct { const char *english, *portuguese; } words[] = {
        { "BORDERLESS", "SEM BORDAS" }, { "WINDOWED", "EM JANELA" },
        { "OFF", "DESATIVADO" }, { "ON", "ATIVADO" }, { "LOW", "BAIXA" },
        { "MEDIUM", "MÉDIA" }, { "HIGH", "ALTA" }, { "ULTRA", "MÁXIMA" },
        { "SSAO CLASSIC", "SSAO CLÁSSICO" }, { "ORIGINAL", "ORIGINAL" },
        { "STRETCH", "ESTICAR" }, { "INTEGER", "INTEIRA" }, { "FIT", "AJUSTAR" },
        { "NEAREST", "VIZINHO PRÓXIMO" }, { "LINEAR", "LINEAR" }, { "UNLIMITED", "ILIMITADO" },
        { "32:9", "SUPER ULTRA-WIDE (32:9)" }, { "UNAVAILABLE", "INDISPONÍVEL" },
        { "CROSS", "CRUZ" }, { "T-SHAPE", "EM T" }, { "CIRCLE", "CÍRCULO" },
        { "TOGGLE", "ALTERNAR" }, { "HOLD", "SEGURAR" },
        { "DLSS QUALITY", "DLSS QUALIDADE" }, { "DLSS BALANCED", "DLSS EQUILIBRADO" }, { "DLSS PERFORMANCE", "DLSS DESEMPENHO" }
    };
    int i;
    for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])); ++i)
        if (!strcmp(value, words[i].english)) return words[i].portuguese;
    return value;
}

static const char *video_menu_text(const char *english, const char *portuguese)
{
    return video_menu_portuguese() ? portuguese : english;
}

const char *pc_video_menu_row_label(int row)
{
    if (row < 0 || row >= R_COUNT) return "";
    return video_menu_portuguese() ? k_row_label_pt[row] : k_row_label[row];
}

/* Values are deliberately compact so the game's 640 x 480 native pages retain their normal row
 * spacing. Long explanations and availability status belong to the selected row's help text. */
size_t pc_video_menu_row_value(int row, char *out, size_t cap)
{
    PcVideoSettings cur;
    const PcVideoSettings *s = &cur;
    char value[96] = "";
    size_t len, n;
    unsigned rw, rh;
    if (!out || !cap) return 0;
    pc_video_get(&cur);
    switch (row) {
    case R_MODE: strcpy(value, s->borderless ? "BORDERLESS" : "WINDOWED"); break;
    case R_WINDOW: snprintf(value, sizeof(value), "%u x %u", s->window_width, s->window_height); break;
    case R_INTERNAL:
        pc_video_render_size_for(s, &rw, &rh);
        if (s->render_override_w) snprintf(value, sizeof(value), "%u x %u", rw, rh);
        else snprintf(value, sizeof(value), "%uP", s->internal_height);
        break;
    case R_AA:
        strcpy(value, s->aa == PCV_AA_FXAA ? "FXAA" : s->aa == PCV_AA_SSAA ? "SSAA 4X" : s->aa == PCV_AA_DLAA ? "DLAA" :
                s->aa == PCV_AA_DLSS_QUALITY ? "DLSS QUALITY" : s->aa == PCV_AA_DLSS_BALANCED ? "DLSS BALANCED" :
                s->aa == PCV_AA_DLSS_PERFORMANCE ? "DLSS PERFORMANCE" : "OFF");
        break;
    case R_AF:
        if (s->af > 1) snprintf(value, sizeof(value), "%uX", s->af); else strcpy(value, "OFF");
        break;
    case R_DOF:
        strcpy(value, s->dof == PCV_STEP_LOW ? "LOW" : s->dof == PCV_STEP_MEDIUM ? "MEDIUM" : s->dof == PCV_STEP_HIGH ? "HIGH" : "OFF");
        break;
    case R_SHARPEN:
        strcpy(value, s->sharpen == PCV_STEP_LOW ? "LOW" : s->sharpen == PCV_STEP_MEDIUM ? "MEDIUM" : s->sharpen == PCV_STEP_HIGH ? "HIGH" : "OFF");
        break;
    case R_AO_METHOD:
        strcpy(value, s->ao_method == PCV_AO_SSAO ? "SSAO CLASSIC" : s->ao_method == PCV_AO_HBAO ? "HBAO" :
                s->ao_method == PCV_AO_HBAO_PLUS ? "HBAO+" : s->ao_method == PCV_AO_GTAO ? "GTAO" : "OFF");
        break;
    case R_AO_QUALITY:
        strcpy(value, s->ao_quality == PCV_AOQ_LOW ? "LOW" : s->ao_quality == PCV_AOQ_MEDIUM ? "MEDIUM" :
                s->ao_quality == PCV_AOQ_ULTRA ? "ULTRA" : "HIGH");
        break;
    case R_REFLECTIONS:
        strcpy(value, s->reflections == PCV_REFL_ORIGINAL ? "ORIGINAL" : s->reflections == PCV_REFL_LOW ? "LOW" :
                s->reflections == PCV_REFL_MEDIUM ? "MEDIUM" : s->reflections == PCV_REFL_ULTRA ? "ULTRA" : "HIGH");
        break;
    case R_RAY_TRACING:
        strcpy(value, pc_video_rt_support() == PCV_RT_UNAVAILABLE ? "UNAVAILABLE" : s->ray_tracing ? "ON" : "OFF");
        break;
    case R_ASPECT:
        strcpy(value, s->aspect == PCV_ASPECT_ORIGINAL ? "ORIGINAL" : s->aspect == PCV_ASPECT_4_3 ? "4:3" :
                s->aspect == PCV_ASPECT_16_9 ? "16:9" : s->aspect == PCV_ASPECT_21_9 ? "21:9" : "32:9");
        break;
    case R_FOV:
        if (s->fov == 0.0) strcpy(value, "ORIGINAL"); else snprintf(value, sizeof(value), "%g\xC2\xB0", s->fov);
        break;
    case R_SCALE: strcpy(value, s->scale == PCV_SCALE_STRETCH ? "STRETCH" : s->scale == PCV_SCALE_INTEGER ? "INTEGER" : "FIT"); break;
    case R_FILTER: strcpy(value, s->nearest ? "NEAREST" : "LINEAR"); break;
    case R_VSYNC: strcpy(value, s->vsync ? "ON" : "OFF"); break;
    case R_FPS:
        if (s->fps_limit) snprintf(value, sizeof(value), "%u FPS", s->fps_limit); else strcpy(value, "UNLIMITED");
        break;
    case R_BLUR: strcpy(value, s->motion_blur_off ? "OFF" : "ORIGINAL"); break;
    case R_CROSSHAIR:
        strcpy(value, s->crosshair == PCX_CROSS ? "CROSS" : s->crosshair == PCX_T ? "T-SHAPE" : s->crosshair == PCX_CIRCLE ? "CIRCLE" : "OFF");
        break;
    case R_HITMARKERS: strcpy(value, s->hitmarkers ? "ON" : "OFF"); break;
    case R_OPTIONAL_SECONDARIES: strcpy(value, s->optional_secondaries ? "ON" : "OFF"); break;
    case R_LOD_DISTANCE: snprintf(value, sizeof(value), "%gX", s->lod_distance / 100.0); break;
    case R_BULLET_DAMAGE: snprintf(value, sizeof(value), "%gX", s->bullet_damage / 100.0); break;
    case R_ENEMY_HEALTH: snprintf(value, sizeof(value), "%gX", s->enemy_health / 100.0); break;
    case R_HEADSHOT_INSTAKILL: strcpy(value, s->headshot_instakill ? "ON" : "OFF"); break;
    case R_SPRINT: strcpy(value, s->sprint ? "ON" : "OFF"); break;
    case R_JUMP: strcpy(value, s->jump ? "ON" : "OFF"); break;
    case R_AIM_MODE: strcpy(value, s->aim_hold ? "HOLD" : "TOGGLE"); break;
    default: break;
    }
    if (video_menu_portuguese()) {
        const char *localized = localized_video_value(value);
        if (localized != value) snprintf(value, sizeof(value), "%s", localized);
        else if (!strcmp(value, "SSAA 4X")) strcpy(value, "SSAA 4X");
        else if (!strcmp(value, "SSAO CLASSIC")) strcpy(value, "SSAO CLÁSSICO");
    }
    len = strlen(value);
    n = len < cap - 1 ? len : cap - 1;
    /* A small caller buffer must not receive half of a UTF-8 degree symbol. */
    while (n && ((unsigned char)value[n] & 0xC0) == 0x80) n--;
    memcpy(out, value, n); out[n] = 0;
    return n;
}

const char *pc_video_menu_row_help(int row)
{
    PcVideoSettings cur;
    int live = pc_video_live_resize();
    if (rt_unavailable(row) && !(pc_video_locked() & PCV_LOCK_RAY_TRACING))
        return video_menu_text("Ray tracing needs the GPU renderer and a graphics card with DirectX Raytracing.",
                               "O ray tracing exige o renderizador GPU e uma placa de vídeo com DirectX Raytracing.");
    if (pc_video_menu_row_locked(row)) return video_menu_text("This setting is fixed by an environment variable for this run.", "Esta opção está fixada por uma variável de ambiente nesta partida.");
    pc_video_get(&cur);
    switch (row) {
    case R_MODE: return video_menu_text("Borderless fills the screen. F11 switches at any time.", "O modo sem bordas preenche a tela. F11 alterna o modo a qualquer momento.");
    case R_WINDOW: return video_menu_text("The size of the window when windowed.", "O tamanho da janela no modo em janela.");
    case R_INTERNAL: return live ? video_menu_text("Resolution of the rendered scene. Higher is sharper and slower. Changes apply immediately.", "Resolução da cena renderizada. Valores maiores deixam a imagem mais nítida e lenta. As alterações são aplicadas na hora.")
                                 : video_menu_text("Resolution of the rendered scene. Changes require a restart.", "Resolução da cena renderizada. As alterações exigem reiniciar o jogo.");
    case R_AA:
        if (!live) return video_menu_text("Anti-aliasing is available with the GPU renderer only.", "A suavização de serrilhado só está disponível no renderizador GPU.");
        if (cur.aa == PCV_AA_SSAA && pc_video_ssaa_factor_for(&cur) == 1)
            return video_menu_text("SSAA 4X is unavailable at this resolution. Lower the internal resolution to enable it.", "SSAA 4X não está disponível nesta resolução. Reduza a resolução interna para ativá-lo.");
        return video_menu_text("FXAA smooths edges; SSAA 4X is the cleanest. DLAA and DLSS need an RTX card; DLSS draws fewer pixels for speed.",
                               "FXAA suaviza as bordas; SSAA 4X é a mais limpa. DLAA e DLSS exigem placa RTX; o DLSS desenha menos pixels e ganha velocidade.");
    case R_AF: return live ? video_menu_text("Keeps textures sharp on floors and walls seen at a slant.", "Mantém nítidas as texturas de pisos e paredes vistas de lado.") : video_menu_text("Anisotropic filtering is available with the GPU renderer only.", "A filtragem anisotrópica só está disponível no renderizador GPU.");
    case R_DOF: return live ? video_menu_text("Blurs the background. Higher is a wider, stronger blur.", "Desfoca o fundo. Valores maiores ampliam e intensificam o desfoque.") : video_menu_text("Depth of field is available with the GPU renderer only.", "A profundidade de campo só está disponível no renderizador GPU.");
    case R_SHARPEN: return live ? video_menu_text("Brings back fine detail softened by filters and scaling.", "Recupera detalhes finos suavizados pelos filtros e pela escala.") : video_menu_text("Sharpening is available with the GPU renderer only.", "A nitidez só está disponível no renderizador GPU.");
    case R_AO_METHOD: return live ? video_menu_text("Ambient occlusion adds contact shading using scene depth. Choose SSAO Classic, HBAO, HBAO+ or GTAO.", "A oclusão de ambiente adiciona sombras de contato usando a profundidade da cena. Escolha SSAO clássico, HBAO, HBAO+ ou GTAO.")
                                    : video_menu_text("Ambient occlusion is available with the GPU renderer only.", "A oclusão de ambiente só está disponível no renderizador GPU.");
    case R_AO_QUALITY:
        if (!live) return video_menu_text("Ambient occlusion is available with the GPU renderer only.", "A oclusão de ambiente só está disponível no renderizador GPU.");
        if (cur.ao_method == PCV_AO_OFF) return video_menu_text("Quality changes AO sampling density and radius. Choose an AO method to enable the effect.", "A qualidade altera a amostragem e o alcance da oclusão. Escolha um método para ativar o efeito.");
        return video_menu_text("Higher quality increases AO sampling density and radius, with more GPU work. All levels use scene resolution and depth-aware blur.", "Qualidades maiores aumentam a amostragem e o alcance da oclusão, exigindo mais da GPU. Todos os níveis usam a resolução da cena e desfoque sensível à profundidade.");
    case R_REFLECTIONS:
        if (!live) return video_menu_text("Screen-space reflections are available with the GPU renderer only.", "Os reflexos em espaço de tela só estão disponíveis no renderizador GPU.");
        return video_menu_text("Replaces the game's low-resolution reflection maps with reflections of the scene on screen. Original keeps the maps. Higher quality traces more steps.",
                               "Substitui os mapas de reflexo de baixa resolução do jogo por reflexos da cena na tela. Original mantém os mapas. Qualidades maiores traçam mais passos.");
    case R_RAY_TRACING:
        if (pc_video_rt_support() == PCV_RT_RESTART)
            return video_menu_text("Experimental. The GPU's ray tracing cores add shadows from characters and props, ambient occlusion, bounce light, and reflections when Reflections is above Original. Takes effect when the game restarts.",
                                   "Experimental. Os núcleos de ray tracing da GPU adicionam sombras de personagens e objetos, oclusão de ambiente, luz rebatida e reflexos quando Reflexos está acima de Original. Vale a partir da próxima vez que o jogo iniciar.");
        return video_menu_text("Experimental. The GPU's ray tracing cores add shadows from characters and props, ambient occlusion, bounce light, and reflections when Reflections is above Original.",
                               "Experimental. Os núcleos de ray tracing da GPU adicionam sombras de personagens e objetos, oclusão de ambiente, luz rebatida e reflexos quando Reflexos está acima de Original.");
    case R_ASPECT: return video_menu_text("The shape of the camera. Original is the game's 4:3 camera.", "Proporção da câmera. Original usa a proporção 4:3 do jogo.");
    case R_FOV: return video_menu_text("Vertical field of view during gameplay, in degrees.", "Campo de visão vertical durante o jogo, em graus.");
    case R_SCALE: return video_menu_text("Fit keeps the picture's shape. Stretch fills the window. Integer uses whole-number scales.", "Ajustar mantém a proporção da imagem. Esticar preenche a janela. Inteira usa múltiplos sem fração.");
    case R_FILTER: return video_menu_text("Linear smooths the scaled picture. Nearest keeps its pixels sharp.", "Linear suaviza a imagem redimensionada. Vizinho próximo preserva os pixels nítidos.");
    case R_VSYNC: return video_menu_text("Waits for the display to refresh before showing a frame.", "Aguarda a atualização da tela antes de exibir cada quadro.");
    case R_FPS: return video_menu_text("Caps how often the picture is shown.", "Limita a frequência de exibição da imagem.");
    case R_BLUR: return video_menu_text("The game's own camera motion blur.", "Desfoque de movimento original da câmera do jogo.");
    case R_CROSSHAIR: return video_menu_text("A crosshair around the game's dot that opens as you move and fire. Off keeps the dot alone.",
                                             "Uma mira em volta do ponto do jogo que se abre ao se mover e atirar. Desligado mantém só o ponto.");
    case R_HITMARKERS: return video_menu_text("A mark on the crosshair when a shot of yours kills an enemy.",
                                              "Uma marca na mira quando um tiro seu mata um inimigo.");
    case R_OPTIONAL_SECONDARIES: return video_menu_text(
        "Allow mission completion without the required secondary objectives. Primary objectives are still required.",
        "Permite concluir a missão sem os objetivos secundários exigidos. Os objetivos primários continuam obrigatórios.");
    case R_LOD_DISTANCE: return video_menu_text(
        "Scales object and geometry detail distance. 1X keeps the original distances; higher values cost more performance.",
        "Ajusta a distância de detalhe de objetos e cenários. 1X mantém as distâncias originais; valores maiores exigem mais desempenho.");
    case R_BULLET_DAMAGE: return video_menu_text(
        "Scales your bullets' damage against enemies. 1X keeps the original damage.",
        "Ajusta o dano das suas balas contra inimigos. 1X mantém o dano original.");
    case R_ENEMY_HEALTH: return video_menu_text(
        "Scales enemy health when they spawn. 1X keeps the original health. Restart the mission to apply it to existing enemies.",
        "Ajusta a vida dos inimigos ao surgirem. 1X mantém a vida original. Reinicie a missão para aplicar aos inimigos existentes.");
    case R_HEADSHOT_INSTAKILL: return video_menu_text(
        "Your confirmed headshots kill enemies instantly. Off keeps the game's normal damage rules.",
        "Seus tiros confirmados na cabeça matam inimigos instantaneamente. Desligado mantém as regras normais de dano.");
    case R_SPRINT: return video_menu_text(
        "Hold the Sprint key while moving to run faster. Default: Left Shift. Off keeps original movement.",
        "Segure a tecla Correr ao se mover para correr mais rapido. Padrao: Shift esquerdo. Desligado mantem o movimento original.");
    case R_JUMP: return video_menu_text(
        "Press the Jump key on the ground to jump. Default: Space. Release before jumping again.",
        "Pressione Pular no chao para saltar. Padrao: Espaco. Solte a tecla antes de pular novamente.");
    case R_AIM_MODE: return video_menu_text(
        "Toggle keeps the original aim behavior. Hold aims only while the aim button is held.",
        "Alternar mantém a mira original. Segurar mira apenas enquanto o botão de mira estiver pressionado.");
    case R_RESET: return video_menu_text("Restores every video setting to its default. Locked settings keep their values.", "Restaura as configurações de vídeo para os valores padrão. Opções fixadas mantêm seus valores.");
    case R_DONE: return video_menu_text("Back to the Options page.", "Volta à página de opções.");
    default: return "";
    }
}

/* The legacy popup retains its detailed values until a native opener is installed. */
static void row_value(int row, const PcVideoSettings *s, wchar_t *out, size_t cap, int live)
{
    char value[96];
    unsigned rw, rh;
    if (!out || !cap) return;
    pc_video_menu_row_value(row, value, sizeof(value));
    MultiByteToWideChar(CP_UTF8, 0, value, -1, out, (int)cap);
    out[cap - 1] = 0;
    if (row == R_MODE) wcsncpy(out, s->borderless ? L"BORDERLESS FULLSCREEN" : L"WINDOWED", cap);
    else if (row == R_INTERNAL) {
        pc_video_render_size_for(s, &rw, &rh);
        if (s->render_override_w) swprintf(out, cap, L"CUSTOM  %u %lc %u", rw, (wint_t)0x00D7, rh);
        else swprintf(out, cap, L"%up  (%u %lc %u)%ls", s->internal_height, rw, (wint_t)0x00D7, rh, live ? L"" : L"  RESTART");
    } else if (row == R_AA && s->aa == PCV_AA_SSAA)
        wcsncpy(out, pc_video_ssaa_factor_for(s) == 1 ? L"SSAA 4X  (NOT AT THIS SIZE)" : L"SSAA 4X  (2 x 2)", cap);
    else if (row == R_ASPECT && s->aspect == PCV_ASPECT_ORIGINAL) wcsncpy(out, L"ORIGINAL (4:3)", cap);
    out[cap - 1] = 0;
    if (!live && ((row == R_AA && s->aa) || (row == R_AF && s->af > 1) ||
                  (row == R_DOF && s->dof) || (row == R_SHARPEN && s->sharpen) ||
                  ((row == R_AO_METHOD || row == R_AO_QUALITY) && s->ao_method != PCV_AO_OFF) ||
                  (row == R_REFLECTIONS && s->reflections != PCV_REFL_ORIGINAL)))
        wcsncat(out, L"  GPU ONLY", cap - wcslen(out) - 1);
}

static unsigned row_lock_bit(int row)
{
    switch (row) {
    case R_MODE: return PCV_LOCK_WINDOW_MODE; case R_WINDOW: return PCV_LOCK_WINDOW_SIZE; case R_INTERNAL: return PCV_LOCK_INTERNAL;
    case R_AA: return PCV_LOCK_AA; case R_AF: return PCV_LOCK_AF; case R_DOF: return PCV_LOCK_DOF; case R_SHARPEN: return PCV_LOCK_SHARPEN;
    case R_AO_METHOD: return PCV_LOCK_AO_METHOD; case R_AO_QUALITY: return PCV_LOCK_AO_QUALITY;
    case R_REFLECTIONS: return PCV_LOCK_REFLECTIONS; case R_RAY_TRACING: return PCV_LOCK_RAY_TRACING;
    case R_ASPECT: return PCV_LOCK_ASPECT; case R_FOV: return PCV_LOCK_FOV; case R_SCALE: return PCV_LOCK_SCALE;
    case R_FILTER: return PCV_LOCK_FILTER; case R_VSYNC: return PCV_LOCK_VSYNC; case R_FPS: return PCV_LOCK_FPS;
    case R_BLUR: return PCV_LOCK_BLUR; case R_CROSSHAIR: return PCV_LOCK_CROSSHAIR; case R_HITMARKERS: return PCV_LOCK_HITMARKERS;
    case R_OPTIONAL_SECONDARIES: return PCV_LOCK_OPTIONAL_SECONDARIES;
    case R_LOD_DISTANCE: return PCV_LOCK_LOD_DISTANCE;
    case R_BULLET_DAMAGE: return PCV_LOCK_BULLET_DAMAGE;
    case R_ENEMY_HEALTH: return PCV_LOCK_ENEMY_HEALTH;
    case R_HEADSHOT_INSTAKILL: return PCV_LOCK_HEADSHOT_INSTAKILL;
    default: return 0;
    }
}

static void draw_hint(double s, int keyboard)
{
    wchar_t text[96];
    const char *suffix = video_menu_text("VIDEO SETTINGS", "CONFIGURAÇÕES DE VÍDEO");
    char hint[112];
    snprintf(hint, sizeof(hint), keyboard ? "[V]  %s" : "[RB]  %s", suffix);
    MultiByteToWideChar(CP_UTF8, 0, hint, -1, text, (int)(sizeof(text) / sizeof(text[0])));
    HFONT font = make_font((int)(21 * s + 0.5), FW_NORMAL);
    const int tw = text_width(font, text), pad = (int)(18 * s), h = (int)(44 * s);
    const int w = tw + 2 * pad, x = ((int)m_buf_w - w) / 2, y = (int)m_buf_h - h - (int)(34 * s);
    fill_rect(x, y, w, h, RGBA(8, 14, 15, 215));
    frame_rect(x, y, w, h, 1, RGBA(92, 112, 108, 255));
    fill_rect(x, y, (int)(4 * s) + 1, h, RGBA(200, 160, 74, 255));
    draw_text(font, text, x + pad, y + (h - (int)(21 * s)) / 2 - 2, 0, 0xE6E6DC, 255);
    DeleteObject(font);
}

/* The panel in design units (a 1080-line window): title, the setting rows, a gap, Reset and Done, help and legend. */
#define PANEL_ROW 40
#define PANEL_UNITS (104 + R_RESET * PANEL_ROW + 18 + 2 * PANEL_ROW + 112)

static void draw_panel(double s, int sel, int keyboard, int live)
{
    PcVideoSettings cur;
    const unsigned locked = pc_video_locked();
    const int pw = (int)(980 * s), pad = (int)(40 * s), row_h = (int)(PANEL_ROW * s), gap = (int)(18 * s);
    const int title_h = (int)(104 * s), foot_h = (int)(112 * s);
    const int ph = title_h + (R_RESET)*row_h + gap + 2 * row_h + foot_h;
    const int px = ((int)m_buf_w - pw) / 2, py = ((int)m_buf_h - ph) / 2;
    const int vw = (int)(400 * s), vx = px + pw - pad - vw;
    HFONT f_title = make_font((int)(44 * s + 0.5), FW_BOLD), f_row = make_font((int)(24 * s + 0.5), FW_NORMAL);
    HFONT f_sel = make_font((int)(24 * s + 0.5), FW_BOLD), f_small = make_font((int)(19 * s + 0.5), FW_NORMAL);
    wchar_t title[64];
    int row, y;
    pc_video_get(&cur);
    {   /* dim the page underneath: a plain store, the buffer holds nothing yet (a blend over 3440x1440 would be a stutter) */
        size_t i, n = (size_t)m_buf_w * m_buf_h;
        for (i = 0; i < n; i++) m_buf[i] = 0x78000000u;
    }
    fill_rect(px, py, pw, ph, RGBA(8, 14, 15, 238));
    frame_rect(px, py, pw, ph, 1, RGBA(92, 112, 108, 255));
    fill_rect(px + pad, py + (int)(28 * s), (int)(6 * s) + 1, (int)(48 * s), RGBA(200, 160, 74, 255));
    MultiByteToWideChar(CP_UTF8, 0, video_menu_text("VIDEO SETTINGS", "CONFIGURAÇÕES DE VÍDEO"), -1, title, (int)(sizeof(title) / sizeof(title[0])));
    draw_text(f_title, title, px + pad + (int)(20 * s), py + (int)(24 * s), 0, 0xE8E2D0, 255);
    fill_rect(px + pad, py + title_h - (int)(8 * s), pw - 2 * pad, 1, RGBA(92, 112, 108, 255));
    y = py + title_h;
    for (row = 0; row < R_COUNT; row++) {
        const int is_sel = row == sel, is_lock = (locked & row_lock_bit(row)) != 0;
        wchar_t value[96], label[64];
        const int ry = y + (row < R_RESET ? row * row_h : R_RESET * row_h + gap + (row - R_RESET) * row_h);
        const int ty = ry + (row_h - (int)(24 * s)) / 2 - 3;
        HFONT font = is_sel ? f_sel : f_row;
        if (row == R_RESET) fill_rect(px + pad, ry - gap / 2, pw - 2 * pad, 1, RGBA(92, 112, 108, 200));
        if (is_sel) {
            fill_rect(px + pad - (int)(12 * s), ry, pw - 2 * pad + (int)(24 * s), row_h, RGBA(255, 255, 255, 30));
            fill_rect(px + pad - (int)(12 * s), ry, (int)(4 * s) + 1, row_h, RGBA(200, 160, 74, 255));
        }
        MultiByteToWideChar(CP_UTF8, 0, pc_video_menu_row_label(row), -1, label, 64);
        draw_text(font, label, px + pad + (int)(8 * s), ty, 0, is_lock ? 0x6C7C7A : is_sel ? 0xF4F4EC : 0x93A29E, 255);
        if (row < R_RESET) {
            const int cx = vx + vw / 2;
            const unsigned colour = is_lock ? 0x6C7C7A : is_sel ? 0xF0CE78 : 0xB4C2BE;
            row_value(row, &cur, value, 96, live);
            if (is_lock) wcsncat(value, L"  (LOCKED)", 95 - wcslen(value));
            draw_text(font, value, cx, ty, 2, colour, 255);
            if (is_sel && !is_lock) {
                const int tri = (int)(16 * s);
                triangle(vx, ry + row_h / 2, tri, -1, RGBA(200, 160, 74, 255));
                triangle(vx + vw - tri * 2 / 3, ry + row_h / 2, tri, 1, RGBA(200, 160, 74, 255));
            }
        }
    }
    {
        const int fy = py + ph - foot_h + (int)(14 * s);
        wchar_t help[256];
        const wchar_t *legend = keyboard ? L"UP / DOWN  SELECT        LEFT / RIGHT  CHANGE        ESC  BACK"
                                         : L"D-PAD  SELECT / CHANGE        A  CHANGE        B  BACK";
        MultiByteToWideChar(CP_UTF8, 0, pc_video_menu_row_help(sel), -1, help, 256);
        fill_rect(px + pad, fy - (int)(8 * s), pw - 2 * pad, 1, RGBA(92, 112, 108, 255));
        draw_text(f_small, help, px + pad, fy + (int)(4 * s), 0, 0xC4CFCB, 255);
        draw_text(f_small, legend, px + pad, fy + (int)(46 * s), 0, 0x93A29E, 255);
    }
    DeleteObject(f_title); DeleteObject(f_row); DeleteObject(f_sel); DeleteObject(f_small);
}

int pc_video_menu_overlay(unsigned w, unsigned h, const uint32_t **pixels, int *changed)
{
    int open, sel, hint, keyboard, live;
    uint64_t key;
    double s;
    ULONGLONG now = GetTickCount64();
    if (!w || !h) return 0;
    AcquireSRWLockExclusive(&m_lock);
    /* If the game leaves the Options page (a new level, a disconnect), the page goes with it. */
    if (m_open && !m_force && (!m_seen_tick || now - m_seen_tick > 1500)) m_open = 0;
    open = m_open; sel = m_sel;
    hint = !m_open && available_locked() && !m_hint_hidden;
    ReleaseSRWLockExclusive(&m_lock);
    keyboard = pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE;
    live = pc_video_live_resize();
    if (!open && !hint) {
        *changed = m_last_visible;     /* one final clear when it disappears */
        m_last_visible = 0; m_last_key = 0;
        return 0;
    }
    key = (uint64_t)open | ((uint64_t)(unsigned)sel << 1) | ((uint64_t)hint << 8) | ((uint64_t)keyboard << 9) | ((uint64_t)live << 10) |
          ((uint64_t)(pc_video_generation() & 0xFFFFu) << 16) | ((uint64_t)(w & 0xFFFFu) << 32) | ((uint64_t)(h & 0xFFFFu) << 48);
    if (w != m_buf_w || h != m_buf_h || !m_buf) {
        free(m_buf);
        m_buf = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
        if (!m_buf) { m_buf_w = m_buf_h = 0; return 0; }
        m_buf_w = w; m_buf_h = h; m_last_key = 0;
    }
    *changed = key != m_last_key || !m_last_visible;
    if (*changed) {
        s = (double)h / 1080.0;
        if (s < 0.5) s = 0.5;
        if (s > 2.0) s = 2.0;
        if (open) {                                  /* the panel never taller than the window */
            const double fit = (double)h * 0.94 / (double)PANEL_UNITS;
            if (s > fit) s = fit;
        }
        if (open) draw_panel(s, sel, keyboard, live);
        else { memset(m_buf, 0, (size_t)w * h * sizeof(uint32_t)); draw_hint(s, keyboard); }
        m_last_key = key;
    }
    m_last_visible = 1;
    *pixels = m_buf;
    return 1;
}

#else  /* !_WIN32 */
const char *pc_video_menu_row_label(int row) { (void)row; return ""; }
size_t pc_video_menu_row_value(int row, char *out, size_t cap) { (void)row; if (out && cap) out[0] = 0; return 0; }
const char *pc_video_menu_row_help(int row) { (void)row; return ""; }
int pc_video_menu_row_locked(int row) { (void)row; return 0; }
int pc_video_menu_row_change(int row, int dir) { (void)row; (void)dir; return 0; }
void pc_video_menu_reset_settings(void) {}
void pc_video_menu_set_native_opener(void (*request)(void)) { (void)request; }
void pc_video_menu_page_seen(void) {}
void pc_video_menu_install(void) {}
int pc_video_menu_is_open(void) { return 0; }
void pc_video_menu_open(void) {}
void pc_video_menu_hide_hint(int hide) { (void)hide; }
int pc_video_menu_overlay(unsigned w, unsigned h, const uint32_t **p, int *c) { (void)w; (void)h; (void)p; (void)c; return 0; }
void pc_video_menu_test_action(int a) { (void)a; }
void pc_video_menu_test_force_available(int on) { (void)on; }
int pc_video_menu_test_selected(void) { return 0; }
#endif
