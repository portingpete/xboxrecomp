/* PC video settings: defaults, settings file, environment overrides. See pc_video.h. */
#include "pc_video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#endif

const unsigned pc_video_internal_heights[] = { 480, 720, 1080, 1440, 2160 };
const unsigned pc_video_internal_height_count = 5;
const unsigned pc_video_fps_limits[] = { 0, 30, 60, 120, 144, 165, 240 };
const unsigned pc_video_fps_limit_count = 7;
const unsigned pc_video_af_levels[] = { 1, 2, 4, 8, 16 };
const unsigned pc_video_af_level_count = 5;
const unsigned pc_video_lod_distances[] = { 100, 200, 300, 500, 800, 1000 };
const unsigned pc_video_lod_distance_count = 6;
const unsigned pc_video_bullet_damages[] = { 50, 100, 150, 200, 300, 400, 600, 1000 };
const unsigned pc_video_bullet_damage_count = 8;
const unsigned pc_video_enemy_healths[] = { 25, 50, 75, 100, 150, 200, 300, 400 };
const unsigned pc_video_enemy_health_count = 8;

#define PCV_MAX_PIXELS 16777216u

#if defined(_WIN32)
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile LONG s_generation, s_aspect, s_fov_milli, s_blur_off, s_split;
static DWORD s_poll_at;
#else
static int s_once_done;
static unsigned s_generation, s_aspect, s_fov_milli, s_blur_off, s_split;
#endif
static PcVideoSettings s_cur, s_file;    /* what is in force; what the settings file holds */
static unsigned s_locked;
static char s_path[512];
static PcVideoResizeHook s_resize_hook;
static PcVideoRtSupport s_rt_support;

void pc_video_defaults(PcVideoSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->internal_height = 720;
    s->window_width = 1280; s->window_height = 960;
    s->scale = PCV_SCALE_FIT;
    s->vsync = 1;
    s->aspect = PCV_ASPECT_ORIGINAL;
    s->aa = PCV_AA_FXAA;
    s->af = 16;
    s->dof = PCV_STEP_OFF;
    s->sharpen = PCV_STEP_OFF;
    s->ao_method = PCV_AO_OFF;
    s->ao_quality = PCV_AOQ_HIGH;
    s->reflections = PCV_REFL_HIGH;
    s->crosshair = 0;             /* original dot; the added crosshair is opt-in */
    s->hitmarkers = 0;
    s->optional_secondaries = 0;
    s->lod_distance = s->bullet_damage = s->enemy_health = 100;
}

static int in_list(const unsigned *list, unsigned count, unsigned v)
{
    unsigned i;
    for (i = 0; i < count; i++) if (list[i] == v) return 1;
    return 0;
}
static int valid_fov(double f) { return f == 0.0 || (isfinite(f) && f >= 35.0 && f <= 120.0); }

const char *pc_video_aspect_name(unsigned a)
{
    static const char *const names[PCV_ASPECT_COUNT] = { "original", "4:3", "16:9", "21:9", "32:9" };
    return a < PCV_ASPECT_COUNT ? names[a] : names[0];
}
static int aspect_from_name(const char *t)
{
    unsigned a;
    for (a = 0; a < PCV_ASPECT_COUNT; a++) if (!_stricmp(t, pc_video_aspect_name(a))) return (int)a;
    return -1;
}

static const char *const k_aa_names[PCV_AA_COUNT] = { "off", "fxaa", "ssaa", "dlaa", "dlss_quality", "dlss_balanced",
                                                      "dlss_performance" };
static const char *const k_step_names[PCV_STEP_COUNT] = { "off", "low", "medium", "high" };
static const char *const k_ao_names[PCV_AO_COUNT] = { "off", "ssao", "hbao", "hbao_plus", "gtao" };
static const char *const k_ao_quality_names[PCV_AOQ_COUNT] = { "low", "medium", "high", "ultra" };
static const char *const k_refl_names[PCV_REFL_COUNT] = { "original", "low", "medium", "high", "ultra" };
static const char *const k_crosshair_names[] = { "original", "cross", "t", "circle" };
#define CROSSHAIR_COUNT 4u
static int name_index(const char *const *names, unsigned count, const char *t)
{
    unsigned i;
    for (i = 0; i < count; i++) if (!_stricmp(t, names[i])) return (int)i;
    return -1;
}

void pc_video_aspect_ratio(unsigned aspect, unsigned *num, unsigned *den)
{
    static const unsigned n[PCV_ASPECT_COUNT] = { 0, 4, 16, 21, 32 }, d[PCV_ASPECT_COUNT] = { 0, 3, 9, 9, 9 };
    if (aspect >= PCV_ASPECT_COUNT) aspect = PCV_ASPECT_ORIGINAL;
    *num = n[aspect]; *den = d[aspect];
}

void pc_video_render_size_for(const PcVideoSettings *s, unsigned *w, unsigned *h)
{
    unsigned num, den, width, height;
    if (s->render_override_w && s->render_override_h) { *w = s->render_override_w; *h = s->render_override_h; return; }
    pc_video_aspect_ratio(s->aspect, &num, &den);
    if (!num) { num = 4; den = 3; }               /* the game's own camera is 4:3 */
    height = s->internal_height;
    width = (unsigned)(((uint64_t)height * num + den / 2) / den);
    width = (width + 1u) & ~1u;
    if (width < 640) width = 640;
    if (height < 480) height = 480;
    *w = width; *h = height;
}

unsigned pc_video_ssaa_factor_for(const PcVideoSettings *s)
{
    unsigned w, h;
    if (s->aa != PCV_AA_SSAA || (s->render_override_w && s->render_override_h)) return 1;
    pc_video_render_size_for(s, &w, &h);
    if (w * 2u > 8192u || h * 2u > 8192u || (uint64_t)w * 2u * h * 2u > PCV_MAX_PIXELS) return 1;
    return 2;
}

void pc_video_raster_size_for(const PcVideoSettings *s, unsigned *w, unsigned *h)
{
    const unsigned f = pc_video_ssaa_factor_for(s);
    pc_video_render_size_for(s, w, h);
    if (s->aa >= PCV_AA_DLSS_QUALITY && s->aa <= PCV_AA_DLSS_PERFORMANCE && !(s->render_override_w && s->render_override_h)) {
        double k = s->aa == PCV_AA_DLSS_QUALITY ? 2.0 / 3.0 : s->aa == PCV_AA_DLSS_BALANCED ? 0.58 : 0.5;
        if (k * *w < 640.0) k = 640.0 / *w;
        if (k * *h < 480.0) k = 480.0 / *h;
        if (k < 1.0) { *w = ((unsigned)(*w * k + 0.5) + 1u) & ~1u; *h = ((unsigned)(*h * k + 0.5) + 1u) & ~1u; }
    }
    *w *= f; *h *= f;
}

/* ---- the settings file ------------------------------------------------------------------------------- */

static char *trim(char *p)
{
    char *e;
    while (*p && isspace((unsigned char)*p)) p++;
    e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    return p;
}

static int parse_uint(const char *t, unsigned lo, unsigned hi, unsigned *out)
{
    char *end;
    unsigned long v;
    if (!t || !*t || !isdigit((unsigned char)*t)) return 0;
    v = strtoul(t, &end, 10);
    if (*end || v < lo || v > hi) return 0;
    *out = (unsigned)v;
    return 1;
}

static int apply_key(PcVideoSettings *s, const char *key, const char *value)
{
    unsigned u;
    if (!_stricmp(key, "internal_height")) {
        if (parse_uint(value, 1, 100000, &u) && in_list(pc_video_internal_heights, pc_video_internal_height_count, u)) { s->internal_height = u; return 1; }
    } else if (!_stricmp(key, "window_width")) {
        if (parse_uint(value, 320, 7680, &u)) { s->window_width = u; return 1; }
    } else if (!_stricmp(key, "window_height")) {
        if (parse_uint(value, 240, 4320, &u)) { s->window_height = u; return 1; }
    } else if (!_stricmp(key, "window_mode")) {
        if (!_stricmp(value, "borderless")) { s->borderless = 1; return 1; }
        if (!_stricmp(value, "windowed")) { s->borderless = 0; return 1; }
    } else if (!_stricmp(key, "scale")) {
        if (!_stricmp(value, "fit")) { s->scale = PCV_SCALE_FIT; return 1; }
        if (!_stricmp(value, "stretch")) { s->scale = PCV_SCALE_STRETCH; return 1; }
        if (!_stricmp(value, "integer")) { s->scale = PCV_SCALE_INTEGER; return 1; }
    } else if (!_stricmp(key, "filter")) {
        if (!_stricmp(value, "nearest")) { s->nearest = 1; return 1; }
        if (!_stricmp(value, "linear")) { s->nearest = 0; return 1; }
    } else if (!_stricmp(key, "vsync")) {
        if (parse_uint(value, 0, 1, &u)) { s->vsync = u; return 1; }
    } else if (!_stricmp(key, "fps_limit")) {
        if (parse_uint(value, 0, 1000, &u) && in_list(pc_video_fps_limits, pc_video_fps_limit_count, u)) { s->fps_limit = u; return 1; }
    } else if (!_stricmp(key, "aspect")) {
        int a = aspect_from_name(value);
        if (a >= 0) { s->aspect = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "fov")) {
        char *end;
        double f = strtod(value, &end);
        if (end != value && !*end && valid_fov(f)) { s->fov = f; return 1; }
    } else if (!_stricmp(key, "motion_blur")) {
        if (!_stricmp(value, "off")) { s->motion_blur_off = 1; return 1; }
        if (!_stricmp(value, "original")) { s->motion_blur_off = 0; return 1; }
    } else if (!_stricmp(key, "action_blur")) {
        if (!_stricmp(value, "off")) { s->action_blur_off = 1; return 1; }
        if (!_stricmp(value, "original")) { s->action_blur_off = 0; return 1; }
    } else if (!_stricmp(key, "aa")) {
        int a = name_index(k_aa_names, PCV_AA_COUNT, value);
        if (a >= 0) { s->aa = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "af")) {
        if (parse_uint(value, 1, 64, &u) && in_list(pc_video_af_levels, pc_video_af_level_count, u)) { s->af = u; return 1; }
    } else if (!_stricmp(key, "dof")) {
        int a = name_index(k_step_names, PCV_STEP_COUNT, value);
        if (a >= 0) { s->dof = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "sharpen")) {
        int a = name_index(k_step_names, PCV_STEP_COUNT, value);
        if (a >= 0) { s->sharpen = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "ao_method")) {
        int a = name_index(k_ao_names, PCV_AO_COUNT, value);
        if (a >= 0) { s->ao_method = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "ao_quality")) {
        int a = name_index(k_ao_quality_names, PCV_AOQ_COUNT, value);
        if (a >= 0) { s->ao_quality = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "reflections")) {
        int a = name_index(k_refl_names, PCV_REFL_COUNT, value);
        if (a >= 0) { s->reflections = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "crosshair")) {
        int a = name_index(k_crosshair_names, CROSSHAIR_COUNT, value);
        if (a >= 0) { s->crosshair = (unsigned)a; return 1; }
    } else if (!_stricmp(key, "hitmarkers")) {
        if (!_stricmp(value, "on")) { s->hitmarkers = 1; return 1; }
        if (!_stricmp(value, "off")) { s->hitmarkers = 0; return 1; }
    } else if (!_stricmp(key, "optional_secondaries")) {
        if (!_stricmp(value, "on")) { s->optional_secondaries = 1; return 1; }
        if (!_stricmp(value, "off")) { s->optional_secondaries = 0; return 1; }
    } else if (!_stricmp(key, "lod_distance")) {
        if (parse_uint(value, 100, 1000, &u) && in_list(pc_video_lod_distances, pc_video_lod_distance_count, u)) { s->lod_distance = u; return 1; }
    } else if (!_stricmp(key, "bullet_damage")) {
        if (parse_uint(value, 50, 1000, &u) && in_list(pc_video_bullet_damages, pc_video_bullet_damage_count, u)) { s->bullet_damage = u; return 1; }
    } else if (!_stricmp(key, "enemy_health")) {
        if (parse_uint(value, 25, 400, &u) && in_list(pc_video_enemy_healths, pc_video_enemy_health_count, u)) { s->enemy_health = u; return 1; }
    } else if (!_stricmp(key, "sprint")) {
        if (!_stricmp(value, "on")) { s->sprint = 1; return 1; }
        if (!_stricmp(value, "off")) { s->sprint = 0; return 1; }
    } else if (!_stricmp(key, "jump")) {
        if (!_stricmp(value, "on")) { s->jump = 1; return 1; }
        if (!_stricmp(value, "off")) { s->jump = 0; return 1; }
    } else if (!_stricmp(key, "aim_mode")) {
        if (!_stricmp(value, "toggle")) { s->aim_hold = 0; return 1; }
        if (!_stricmp(value, "hold")) { s->aim_hold = 1; return 1; }
    } else if (!_stricmp(key, "headshot_instakill")) {
        if (!_stricmp(value, "on")) { s->headshot_instakill = 1; return 1; }
        if (!_stricmp(value, "off")) { s->headshot_instakill = 0; return 1; }
    } else if (!_stricmp(key, "split_screen")) {
        if (!_stricmp(value, "on")) { s->split_screen = 1; return 1; }
        if (!_stricmp(value, "off")) { s->split_screen = 0; return 1; }
    } else if (!_stricmp(key, "ray_tracing")) {
        if (!_stricmp(value, "on")) { s->ray_tracing = 1; return 1; }
        if (!_stricmp(value, "off")) { s->ray_tracing = 0; return 1; }
    } else if (!_stricmp(key, "ssao")) {
        /* The original single SSAO setting combined method and quality. */
        int a = name_index(k_ao_quality_names, PCV_AOQ_COUNT, value);
        if (!_stricmp(value, "off")) { s->ao_method = PCV_AO_OFF; return 1; }
        if (a >= 0) { s->ao_method = PCV_AO_SSAO; s->ao_quality = (unsigned)a; return 1; }
    }
    return 0;
}

int pc_video_parse(const char *text, PcVideoSettings *inout)
{
    char line[256];
    int applied = 0;
    const char *p = text;
    while (p && *p) {
        size_t n = strcspn(p, "\r\n");
        char *eq, *key, *value;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n); line[n] = 0;
        p += strcspn(p, "\r\n");
        while (*p == '\r' || *p == '\n') p++;
        key = trim(line);
        if (!*key || *key == ';' || *key == '#' || *key == '[') continue;
        eq = strchr(key, '=');
        if (!eq) continue;
        *eq = 0;
        value = trim(eq + 1);
        key = trim(key);
        applied += apply_key(inout, key, value);
    }
    return applied;
}

size_t pc_video_format(const PcVideoSettings *s, char *out, size_t cap)
{
    int n = snprintf(out, cap,
        "; BLACK video settings. Edited by the in-game Video Settings page and by the launcher.\n"
        "[video]\n"
        "internal_height=%u\n"
        "window_width=%u\n"
        "window_height=%u\n"
        "window_mode=%s\n"
        "scale=%s\n"
        "filter=%s\n"
        "vsync=%u\n"
        "fps_limit=%u\n"
        "aspect=%s\n"
        "fov=%g\n"
        "motion_blur=%s\n"
        "action_blur=%s\n"
        "aa=%s\n"
        "af=%u\n"
        "dof=%s\n"
        "sharpen=%s\n"
        "ao_method=%s\n"
        "ao_quality=%s\n"
        "reflections=%s\n"
        "ray_tracing=%s\n"
        "crosshair=%s\n"
        "hitmarkers=%s\n"
        "optional_secondaries=%s\n"
        "lod_distance=%u\n"
        "bullet_damage=%u\n"
        "enemy_health=%u\n"
        "headshot_instakill=%s\n"
        "sprint=%s\n"
        "jump=%s\n"
        "aim_mode=%s\n"
        "split_screen=%s\n",
        s->internal_height, s->window_width, s->window_height,
        s->borderless ? "borderless" : "windowed",
        s->scale == PCV_SCALE_STRETCH ? "stretch" : s->scale == PCV_SCALE_INTEGER ? "integer" : "fit",
        s->nearest ? "nearest" : "linear", s->vsync ? 1u : 0u, s->fps_limit,
        pc_video_aspect_name(s->aspect), s->fov, s->motion_blur_off ? "off" : "original",
        s->action_blur_off ? "off" : "original",
        k_aa_names[s->aa < PCV_AA_COUNT ? s->aa : 0], s->af, k_step_names[s->dof < PCV_STEP_COUNT ? s->dof : 0],
        k_step_names[s->sharpen < PCV_STEP_COUNT ? s->sharpen : 0],
        k_ao_names[s->ao_method < PCV_AO_COUNT ? s->ao_method : PCV_AO_OFF],
        k_ao_quality_names[s->ao_quality < PCV_AOQ_COUNT ? s->ao_quality : PCV_AOQ_HIGH],
        k_refl_names[s->reflections < PCV_REFL_COUNT ? s->reflections : PCV_REFL_HIGH], s->ray_tracing ? "on" : "off",
        k_crosshair_names[s->crosshair < CROSSHAIR_COUNT ? s->crosshair : 0], s->hitmarkers ? "on" : "off",
        s->optional_secondaries ? "on" : "off", s->lod_distance, s->bullet_damage, s->enemy_health,
        s->headshot_instakill ? "on" : "off", s->sprint ? "on" : "off", s->jump ? "on" : "off", s->aim_hold ? "hold" : "toggle", s->split_screen ? "on" : "off");
    return n < 0 ? 0 : (size_t)n;
}

#if defined(_WIN32)
static void save_file(const PcVideoSettings *s)
{
    char text[1024], tmp[sizeof(s_path) + 8], dir[sizeof(s_path)], *slash;
    size_t n = pc_video_format(s, text, sizeof(text));
    FILE *f;
    if (!n || n >= sizeof(text) || !s_path[0]) return;
    snprintf(dir, sizeof(dir), "%s", s_path);
    slash = strrchr(dir, '\\'); if (!slash) slash = strrchr(dir, '/');
    if (slash) { *slash = 0; CreateDirectoryA(dir, NULL); }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);
    f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "[VIDEO] cannot write %s\n", tmp); return; }
    if (fwrite(text, 1, n, f) != n) { fclose(f); DeleteFileA(tmp); return; }
    if (fclose(f) != 0) { DeleteFileA(tmp); return; }
    if (!MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fprintf(stderr, "[VIDEO] cannot replace %s: %lu\n", s_path, GetLastError());
}
#else
static void save_file(const PcVideoSettings *s) { (void)s; }
#endif

static int load_file(PcVideoSettings *s)
{
    FILE *f = fopen(s_path, "rb");
    char text[4096];
    size_t n;
    if (!f) return -1;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return pc_video_parse(text, s);
}

/* ---- the environment --------------------------------------------------------------------------------- */

static const char *env_text(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

static unsigned apply_environment(PcVideoSettings *s)
{
    unsigned lock = 0, u;
    const char *w = env_text("RECOMP_WINDOW_WIDTH"), *h = env_text("RECOMP_WINDOW_HEIGHT"), *t;
    if (w || h) {
        lock |= PCV_LOCK_WINDOW_SIZE;
        if (!(w && parse_uint(w, 320, 7680, &s->window_width))) { if (w) s->window_width = 1280; }
        if (!(h && parse_uint(h, 240, 4320, &s->window_height))) { if (h) s->window_height = 960; }
    }
    if ((t = env_text("RECOMP_WINDOW_MODE")) != NULL) { lock |= PCV_LOCK_WINDOW_MODE; s->borderless = !strcmp(t, "borderless"); }
    if ((t = env_text("RECOMP_SCALE_MODE")) != NULL) {
        lock |= PCV_LOCK_SCALE;
        s->scale = !strcmp(t, "stretch") ? PCV_SCALE_STRETCH : !strcmp(t, "integer") ? PCV_SCALE_INTEGER : PCV_SCALE_FIT;
    }
    if ((t = env_text("RECOMP_PRESENT_FILTER")) != NULL) { lock |= PCV_LOCK_FILTER; s->nearest = !strcmp(t, "nearest"); }
    if ((t = env_text("RECOMP_VSYNC")) != NULL) { lock |= PCV_LOCK_VSYNC; s->vsync = strcmp(t, "0") != 0; }
    if ((t = env_text("RECOMP_FPS_LIMIT")) != NULL) {
        lock |= PCV_LOCK_FPS;
        s->fps_limit = parse_uint(t, 0, 240, &u) && in_list(pc_video_fps_limits, pc_video_fps_limit_count, u) ? u : 0;
    }
    if ((t = env_text("RECOMP_BLACK_ASPECT")) != NULL) {
        int a = aspect_from_name(t);
        lock |= PCV_LOCK_ASPECT;
        s->aspect = a > 0 ? (unsigned)a : PCV_ASPECT_ORIGINAL;
    }
    if ((t = env_text("RECOMP_BLACK_FOV")) != NULL) {
        char *end;
        double f = strtod(t, &end);
        lock |= PCV_LOCK_FOV;
        s->fov = end != t && !*end && valid_fov(f) ? f : 0.0;
    }
    if ((t = env_text("RECOMP_BLACK_MOTION_BLUR")) != NULL) { lock |= PCV_LOCK_BLUR; s->motion_blur_off = !strcmp(t, "off"); }
    if ((t = env_text("RECOMP_AA")) != NULL) {
        int a = name_index(k_aa_names, PCV_AA_COUNT, t);
        lock |= PCV_LOCK_AA; s->aa = a >= 0 ? (unsigned)a : PCV_AA_OFF;
    }
    if ((t = env_text("RECOMP_AF")) != NULL) {
        lock |= PCV_LOCK_AF;
        s->af = parse_uint(t, 1, 64, &u) && in_list(pc_video_af_levels, pc_video_af_level_count, u) ? u : 1u;
    }
    if ((t = env_text("RECOMP_DOF")) != NULL) {
        int a = name_index(k_step_names, PCV_STEP_COUNT, t);
        lock |= PCV_LOCK_DOF; s->dof = a >= 0 ? (unsigned)a : PCV_STEP_OFF;
    }
    if ((t = env_text("RECOMP_SHARPEN")) != NULL) {
        int a = name_index(k_step_names, PCV_STEP_COUNT, t);
        lock |= PCV_LOCK_SHARPEN; s->sharpen = a >= 0 ? (unsigned)a : PCV_STEP_OFF;
    }
    if ((t = env_text("RECOMP_SSAO")) != NULL) {
        lock |= PCV_LOCK_AO_METHOD | PCV_LOCK_AO_QUALITY;
        if (!apply_key(s, "ssao", t)) { s->ao_method = PCV_AO_OFF; s->ao_quality = PCV_AOQ_HIGH; }
    }
    if ((t = env_text("RECOMP_AO_METHOD")) != NULL) {
        int a = name_index(k_ao_names, PCV_AO_COUNT, t);
        lock |= PCV_LOCK_AO_METHOD; s->ao_method = a >= 0 ? (unsigned)a : PCV_AO_OFF;
    }
    if ((t = env_text("RECOMP_AO_QUALITY")) != NULL) {
        int a = name_index(k_ao_quality_names, PCV_AOQ_COUNT, t);
        lock |= PCV_LOCK_AO_QUALITY; s->ao_quality = a >= 0 ? (unsigned)a : PCV_AOQ_HIGH;
    }
    if ((t = env_text("RECOMP_REFLECTIONS")) != NULL) {
        int a = name_index(k_refl_names, PCV_REFL_COUNT, t);
        lock |= PCV_LOCK_REFLECTIONS; s->reflections = a >= 0 ? (unsigned)a : PCV_REFL_ORIGINAL;
    }
    if ((t = env_text("RECOMP_KGPU_RT")) != NULL) { lock |= PCV_LOCK_RAY_TRACING; s->ray_tracing = strcmp(t, "0") != 0; }
    if ((t = env_text("RECOMP_CROSSHAIR")) != NULL) {
        int a = name_index(k_crosshair_names, CROSSHAIR_COUNT, t);
        lock |= PCV_LOCK_CROSSHAIR; s->crosshair = a >= 0 ? (unsigned)a : 0u;
    }
    if ((t = env_text("RECOMP_HITMARKERS")) != NULL) { lock |= PCV_LOCK_HITMARKERS; s->hitmarkers = strcmp(t, "0") && _stricmp(t, "off"); }
    if ((t = env_text("RECOMP_OPTIONAL_SECONDARIES")) != NULL) {
        lock |= PCV_LOCK_OPTIONAL_SECONDARIES;
        s->optional_secondaries = !strcmp(t, "1") || !_stricmp(t, "on");
    }
    if ((t = env_text("RECOMP_LOD_DISTANCE")) != NULL) {
        lock |= PCV_LOCK_LOD_DISTANCE;
        s->lod_distance = parse_uint(t, 100, 1000, &u) && in_list(pc_video_lod_distances, pc_video_lod_distance_count, u) ? u : 100u;
    }
    if ((t = env_text("RECOMP_BULLET_DAMAGE")) != NULL) {
        lock |= PCV_LOCK_BULLET_DAMAGE;
        s->bullet_damage = parse_uint(t, 50, 1000, &u) && in_list(pc_video_bullet_damages, pc_video_bullet_damage_count, u) ? u : 100u;
    }
    if ((t = env_text("RECOMP_ENEMY_HEALTH")) != NULL) {
        lock |= PCV_LOCK_ENEMY_HEALTH;
        s->enemy_health = parse_uint(t, 25, 400, &u) && in_list(pc_video_enemy_healths, pc_video_enemy_health_count, u) ? u : 100u;
    }
    if ((t = env_text("RECOMP_HEADSHOT_INSTAKILL")) != NULL) {
        lock |= PCV_LOCK_HEADSHOT_INSTAKILL;
        s->headshot_instakill = !strcmp(t, "1") || !_stricmp(t, "on");
    }
    if ((t = env_text("RECOMP_SPLITSCREEN")) != NULL) { lock |= PCV_LOCK_SPLIT; s->split_screen = strcmp(t, "0") && _stricmp(t, "off"); }
    if ((t = env_text("RECOMP_INTERNAL_HEIGHT")) != NULL && parse_uint(t, 1, 100000, &u) &&
        in_list(pc_video_internal_heights, pc_video_internal_height_count, u)) {
        lock |= PCV_LOCK_INTERNAL; s->internal_height = u;
    }
    w = env_text("RECOMP_RENDER_WIDTH"); h = env_text("RECOMP_RENDER_HEIGHT");
    if (w && h) {
        unsigned rw, rh;
        if (parse_uint(w, 240, 8192, &rw) && parse_uint(h, 240, 8192, &rh) && (uint64_t)rw * rh <= PCV_MAX_PIXELS) {
            s->render_override_w = rw; s->render_override_h = rh;
            lock |= PCV_LOCK_INTERNAL;
        }
    }
    return lock;
}

static void publish_mirrors(const PcVideoSettings *s)
{
#if defined(_WIN32)
    InterlockedExchange(&s_aspect, (LONG)s->aspect);
    InterlockedExchange(&s_fov_milli, (LONG)(s->fov * 1000.0 + 0.5));
    InterlockedExchange(&s_blur_off, (LONG)s->motion_blur_off);
    InterlockedExchange(&s_split, (LONG)s->split_screen);
#else
    s_aspect = s->aspect; s_fov_milli = (unsigned)(s->fov * 1000.0 + 0.5); s_blur_off = s->motion_blur_off; s_split = s->split_screen;
#endif
}

static void do_init(const char *path_override)
{
#if defined(_WIN32)
    s_poll_at = GetTickCount() - 250u;
#endif
    const char *p = path_override ? path_override : env_text("RECOMP_VIDEO_INI");
    pc_video_defaults(&s_file);
    snprintf(s_path, sizeof(s_path), "%s", p ? p : "local\\video.ini");
    if (load_file(&s_file) < 0) fprintf(stderr, "[VIDEO] no settings file at %s: defaults\n", s_path);
    s_cur = s_file;
    s_locked = apply_environment(&s_cur);
    publish_mirrors(&s_cur);
    {
        unsigned rw, rh;
        pc_video_render_size_for(&s_cur, &rw, &rh);
        fprintf(stderr, "[VIDEO] internal %ux%u (%up), window %ux%u %s, scale %u, filter %s, vsync %u, cap %u, camera %s, fov %g, blur %s%s; "
                        "aa %s, af %ux, dof %s, sharpen %s, ao %s (%s), reflections %s, ray tracing %s; locked 0x%X\n",
                rw, rh, s_cur.internal_height, s_cur.window_width, s_cur.window_height,
                s_cur.borderless ? "borderless" : "windowed", s_cur.scale, s_cur.nearest ? "nearest" : "linear",
                s_cur.vsync, s_cur.fps_limit, pc_video_aspect_name(s_cur.aspect), s_cur.fov,
                s_cur.motion_blur_off ? "off" : "original", s_cur.render_override_w ? " (raster size overridden)" : "",
                k_aa_names[s_cur.aa], s_cur.af, k_step_names[s_cur.dof], k_step_names[s_cur.sharpen],
                k_ao_names[s_cur.ao_method], k_ao_quality_names[s_cur.ao_quality], k_refl_names[s_cur.reflections],
                s_cur.ray_tracing ? "on" : "off", s_locked);
    }
}

#if defined(_WIN32)
static BOOL CALLBACK init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)ctx;
    do_init((const char *)param);
    return TRUE;
}
void pc_video_init(void) { InitOnceExecuteOnce(&s_once, init_cb, NULL, NULL); }
void pc_video_test_reset(const char *path)
{
    INIT_ONCE fresh = INIT_ONCE_STATIC_INIT;
    s_once = fresh; s_resize_hook = NULL; s_rt_support = NULL; s_generation = 0;
    InitOnceExecuteOnce(&s_once, init_cb, (PVOID)path, NULL);
}
#define READ_LOCK() AcquireSRWLockShared(&s_lock)
#define READ_UNLOCK() ReleaseSRWLockShared(&s_lock)
#define WRITE_LOCK() AcquireSRWLockExclusive(&s_lock)
#define WRITE_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
void pc_video_init(void) { if (!s_once_done) { s_once_done = 1; do_init(NULL); } }
void pc_video_test_reset(const char *path) { s_once_done = 1; s_resize_hook = NULL; s_rt_support = NULL; s_generation = 0; do_init(path); }
#define READ_LOCK() ((void)0)
#define READ_UNLOCK() ((void)0)
#define WRITE_LOCK() ((void)0)
#define WRITE_UNLOCK() ((void)0)
#endif

void pc_video_get(PcVideoSettings *out)
{
    pc_video_init();
    READ_LOCK();
    *out = s_cur;
    READ_UNLOCK();
}

unsigned pc_video_locked(void) { pc_video_init(); return s_locked; }

unsigned pc_video_generation(void)
{
#if defined(_WIN32)
    return (unsigned)InterlockedCompareExchange(&s_generation, 0, 0);
#else
    return s_generation;
#endif
}

unsigned pc_video_aspect(void)
{
    pc_video_init();
#if defined(_WIN32)
    return (unsigned)InterlockedCompareExchange(&s_aspect, 0, 0);
#else
    return s_aspect;
#endif
}

double pc_video_fov(void)
{
    pc_video_init();
#if defined(_WIN32)
    return (double)InterlockedCompareExchange(&s_fov_milli, 0, 0) / 1000.0;
#else
    return (double)s_fov_milli / 1000.0;
#endif
}

unsigned pc_video_split(void)
{
    pc_video_init();
#if defined(_WIN32)
    return (unsigned)InterlockedCompareExchange(&s_split, 0, 0);
#else
    return s_split;
#endif
}

int pc_video_motion_blur_off(void)
{
    pc_video_init();
#if defined(_WIN32)
    return InterlockedCompareExchange(&s_blur_off, 0, 0) != 0;
#else
    return s_blur_off != 0;
#endif
}

static int valid_settings(const PcVideoSettings *s)
{
    return in_list(pc_video_internal_heights, pc_video_internal_height_count, s->internal_height) &&
           s->window_width >= 320 && s->window_width <= 7680 && s->window_height >= 240 && s->window_height <= 4320 &&
           s->scale <= PCV_SCALE_INTEGER && in_list(pc_video_fps_limits, pc_video_fps_limit_count, s->fps_limit) &&
           s->aspect < PCV_ASPECT_COUNT && valid_fov(s->fov) && s->aa < PCV_AA_COUNT &&
           in_list(pc_video_af_levels, pc_video_af_level_count, s->af) && s->dof < PCV_STEP_COUNT && s->sharpen < PCV_STEP_COUNT &&
           s->ao_method < PCV_AO_COUNT && s->ao_quality < PCV_AOQ_COUNT && s->reflections < PCV_REFL_COUNT && s->ray_tracing <= 1 &&
           s->crosshair < CROSSHAIR_COUNT && s->hitmarkers <= 1 && s->optional_secondaries <= 1 && s->split_screen <= 1 &&
           in_list(pc_video_lod_distances, pc_video_lod_distance_count, s->lod_distance) &&
           in_list(pc_video_bullet_damages, pc_video_bullet_damage_count, s->bullet_damage) &&
           in_list(pc_video_enemy_healths, pc_video_enemy_health_count, s->enemy_health) && s->headshot_instakill <= 1 &&
           s->aim_hold <= 1 && s->sprint <= 1 && s->jump <= 1;
}

/* The new values over the current ones, except where the environment holds a field. */
static PcVideoSettings merge_unlocked(const PcVideoSettings *base, const PcVideoSettings *wanted, unsigned locked)
{
    PcVideoSettings r = *wanted;
    if (locked & PCV_LOCK_WINDOW_SIZE) { r.window_width = base->window_width; r.window_height = base->window_height; }
    if (locked & PCV_LOCK_WINDOW_MODE) r.borderless = base->borderless;
    if (locked & PCV_LOCK_INTERNAL) { r.internal_height = base->internal_height; }
    r.render_override_w = base->render_override_w; r.render_override_h = base->render_override_h;
    if (locked & PCV_LOCK_SCALE) r.scale = base->scale;
    if (locked & PCV_LOCK_FILTER) r.nearest = base->nearest;
    if (locked & PCV_LOCK_VSYNC) r.vsync = base->vsync;
    if (locked & PCV_LOCK_FPS) r.fps_limit = base->fps_limit;
    if (locked & PCV_LOCK_ASPECT) r.aspect = base->aspect;
    if (locked & PCV_LOCK_FOV) r.fov = base->fov;
    if (locked & PCV_LOCK_BLUR) r.motion_blur_off = base->motion_blur_off;
    if (locked & PCV_LOCK_AA) r.aa = base->aa;
    if (locked & PCV_LOCK_AF) r.af = base->af;
    if (locked & PCV_LOCK_DOF) r.dof = base->dof;
    if (locked & PCV_LOCK_SHARPEN) r.sharpen = base->sharpen;
    if (locked & PCV_LOCK_AO_METHOD) r.ao_method = base->ao_method;
    if (locked & PCV_LOCK_AO_QUALITY) r.ao_quality = base->ao_quality;
    if (locked & PCV_LOCK_REFLECTIONS) r.reflections = base->reflections;
    if (locked & PCV_LOCK_RAY_TRACING) r.ray_tracing = base->ray_tracing;
    if (locked & PCV_LOCK_CROSSHAIR) r.crosshair = base->crosshair;
    if (locked & PCV_LOCK_HITMARKERS) r.hitmarkers = base->hitmarkers;
    if (locked & PCV_LOCK_OPTIONAL_SECONDARIES) r.optional_secondaries = base->optional_secondaries;
    if (locked & PCV_LOCK_LOD_DISTANCE) r.lod_distance = base->lod_distance;
    if (locked & PCV_LOCK_BULLET_DAMAGE) r.bullet_damage = base->bullet_damage;
    if (locked & PCV_LOCK_ENEMY_HEALTH) r.enemy_health = base->enemy_health;
    if (locked & PCV_LOCK_HEADSHOT_INSTAKILL) r.headshot_instakill = base->headshot_instakill;
    if (locked & PCV_LOCK_SPLIT) r.split_screen = base->split_screen;
    return r;
}

static int publish(const PcVideoSettings *wanted, int announce, int persist)
{
    PcVideoSettings next, file_next, before;
    unsigned ow, oh, nw, nh, orw, orh, nrw, nrh;
    PcVideoResizeHook hook;
    pc_video_init();
    if (!valid_settings(wanted)) return 0;
    WRITE_LOCK();
    before = s_cur;
    next = merge_unlocked(&s_cur, wanted, s_locked);
    /* The file keeps the player's choice for every field the environment does not pin. */
    file_next = merge_unlocked(&s_file, wanted, s_locked);
    file_next.render_override_w = file_next.render_override_h = 0;
    s_cur = next; s_file = file_next;
    publish_mirrors(&s_cur);
    hook = s_resize_hook;
    WRITE_UNLOCK();
    if (persist) save_file(&file_next);
    if (announce) {
#if defined(_WIN32)
        InterlockedIncrement(&s_generation);
#else
        s_generation++;
#endif
    }
    pc_video_render_size_for(&before, &ow, &oh);
    pc_video_render_size_for(&next, &nw, &nh);
    pc_video_raster_size_for(&before, &orw, &orh);
    pc_video_raster_size_for(&next, &nrw, &nrh);
    if (hook && (ow != nw || oh != nh || orw != nrw || orh != nrh)) hook(nrw, nrh, nw, nh);
    return 1;
}

int pc_video_set(const PcVideoSettings *wanted) { return publish(wanted, 1, 1); }

void pc_video_poll_file(void)
{
#if defined(_WIN32)
    DWORD now = GetTickCount();
    PcVideoSettings wanted, before;
    char old_text[1024], new_text[1024];
    pc_video_init();
    if ((DWORD)(now - s_poll_at) < 250u) return;
    s_poll_at = now;
    READ_LOCK();
    before = s_file;
    READ_UNLOCK();
    wanted = before;
    if (load_file(&wanted) <= 0 || !valid_settings(&wanted)) return;
    wanted = merge_unlocked(&before, &wanted, s_locked);
    pc_video_format(&before, old_text, sizeof(old_text));
    pc_video_format(&wanted, new_text, sizeof(new_text));
    if (strcmp(old_text, new_text) == 0) return;
    /* Reuse the in-game path: generation, camera mirrors and queued renderer resize.
     * Do not write back over the launcher's atomic replacement. */
    if (publish(&wanted, 1, 0)) fprintf(stderr, "[VIDEO] launcher settings applied live\n");
#endif
}

void pc_video_reset(void)
{
    PcVideoSettings d;
    pc_video_defaults(&d);
    publish(&d, 1, 1);
}

void pc_video_note_borderless(unsigned borderless)
{
    PcVideoSettings file;
    pc_video_init();
    borderless = borderless ? 1u : 0u;
    WRITE_LOCK();
    if (s_cur.borderless == borderless) { WRITE_UNLOCK(); return; }
    s_cur.borderless = borderless;
    /* F11 works even where the environment chose the starting mode; only an unpinned choice is remembered. */
    if (!(s_locked & PCV_LOCK_WINDOW_MODE)) s_file.borderless = borderless;
    file = s_file;
    WRITE_UNLOCK();
    if (!(s_locked & PCV_LOCK_WINDOW_MODE)) save_file(&file);
}

void pc_video_render_size(unsigned *w, unsigned *h)
{
    PcVideoSettings s;
    pc_video_get(&s);
    pc_video_render_size_for(&s, w, h);
}

void pc_video_raster_size(unsigned *w, unsigned *h)
{
    PcVideoSettings s;
    pc_video_get(&s);
    pc_video_raster_size_for(&s, w, h);
}

void pc_video_set_resize_hook(PcVideoResizeHook hook)
{
    pc_video_init();
    WRITE_LOCK();
    s_resize_hook = hook;
    WRITE_UNLOCK();
}

int pc_video_live_resize(void)
{
    int live;
    pc_video_init();
    READ_LOCK();
    live = s_resize_hook != NULL;
    READ_UNLOCK();
    return live;
}

void pc_video_set_rt_support(PcVideoRtSupport query)
{
    pc_video_init();
    WRITE_LOCK();
    s_rt_support = query;
    WRITE_UNLOCK();
}

int pc_video_rt_support(void)
{
    PcVideoRtSupport query;
    pc_video_init();
    READ_LOCK();
    query = s_rt_support;
    READ_UNLOCK();
    return query ? query() : PCV_RT_UNAVAILABLE;
}
