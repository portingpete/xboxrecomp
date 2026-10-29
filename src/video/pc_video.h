/*
 * pc_video.h -- the PC video settings of the recompiled game: one model for the window, the
 * internal (render) resolution, the camera and the presenter.
 *
 * Where the values come from, lowest priority first:
 *   1. built-in defaults (720p internal resolution, a 1280x960 window, original camera);
 *   2. the settings file, local\video.ini (RECOMP_VIDEO_INI overrides the path), which the
 *      in-game Video Settings page and the launcher both write;
 *   3. the RECOMP_* environment variables the diagnostics and lab scripts have always used
 *      (RECOMP_WINDOW_WIDTH/HEIGHT/MODE, RECOMP_RENDER_WIDTH/HEIGHT, RECOMP_SCALE_MODE,
 *      RECOMP_PRESENT_FILTER, RECOMP_VSYNC, RECOMP_FPS_LIMIT, RECOMP_BLACK_ASPECT,
 *      RECOMP_BLACK_FOV, RECOMP_BLACK_MOTION_BLUR, RECOMP_INTERNAL_HEIGHT, RECOMP_KGPU_RT). A variable that is
 *      set wins for the life of the process and its row in the menu is shown locked.
 *
 * The internal resolution is a number of lines (480p .. 2160p); the width follows from the
 * camera aspect ratio, so the raster always matches what the camera sees. The window is a
 * separate setting: the picture is scaled to it (fit, stretch or integer).
 *
 * Threads: any thread may read; changes come from the game thread (native menus) or presenter and are
 * announced by pc_video_generation(), which every consumer polls at its own safe point.
 */
#ifndef XBOXRECOMP_PC_VIDEO_H
#define XBOXRECOMP_PC_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Camera aspect. ORIGINAL is the game's own 4:3 camera; 4:3 is the same shape chosen explicitly. */
enum { PCV_ASPECT_ORIGINAL, PCV_ASPECT_4_3, PCV_ASPECT_16_9, PCV_ASPECT_21_9, PCV_ASPECT_32_9, PCV_ASPECT_COUNT };
enum { PCV_SCALE_FIT, PCV_SCALE_STRETCH, PCV_SCALE_INTEGER };
/* Anti-aliasing: FXAA filters the finished scene (before the HUD); SSAA renders at twice the width and height and
 * resolves back to the internal resolution. DLAA is NVIDIA DLSS at the internal resolution (FXAA where it cannot run);
 * the DLSS modes rasterise the world smaller (2/3, 0.58 or half the width and height, never under 640 x 480) and DLSS
 * brings it up to the internal resolution before the HUD.
 * Depth of field and sharpening have four steps: off, low, medium, high. */
enum { PCV_AA_OFF, PCV_AA_FXAA, PCV_AA_SSAA, PCV_AA_DLAA, PCV_AA_DLSS_QUALITY, PCV_AA_DLSS_BALANCED, PCV_AA_DLSS_PERFORMANCE,
       PCV_AA_COUNT };
enum { PCV_STEP_OFF, PCV_STEP_LOW, PCV_STEP_MEDIUM, PCV_STEP_HIGH, PCV_STEP_COUNT };
/* Ambient occlusion is a scene-depth effect applied before the HUD. Quality controls sampling
 * density and radius; the AO mask and its depth-aware blur stay at the scene resolution. */
enum { PCV_AO_OFF, PCV_AO_SSAO, PCV_AO_HBAO, PCV_AO_HBAO_PLUS, PCV_AO_GTAO, PCV_AO_COUNT };
enum { PCV_AOQ_LOW, PCV_AOQ_MEDIUM, PCV_AOQ_HIGH, PCV_AOQ_ULTRA, PCV_AOQ_COUNT };
/* Reflections: Original keeps the game's 128 x 128 environment maps; the others replace them on the surfaces that use
 * them with screen-space reflections, marching more steps at higher quality. */
enum { PCV_REFL_ORIGINAL, PCV_REFL_LOW, PCV_REFL_MEDIUM, PCV_REFL_HIGH, PCV_REFL_ULTRA, PCV_REFL_COUNT };

typedef struct PcVideoSettings {
    unsigned internal_height;                 /* 480, 720, 1080, 1440 or 2160 */
    unsigned window_width, window_height;     /* client size of the window when windowed */
    unsigned borderless;                      /* 1 = borderless full screen */
    unsigned scale;                           /* PCV_SCALE_* */
    unsigned nearest;                         /* 1 = nearest filter when scaling to the window */
    unsigned vsync;
    unsigned fps_limit;                       /* 0 or one of the entries of pc_video_fps_limits */
    unsigned aspect;                          /* PCV_ASPECT_* */
    double fov;                               /* vertical degrees (35..120) or 0 = the game's own */
    unsigned motion_blur_off;
    unsigned action_blur_off;                 /* BLACK's own reload/aim blur (the game's hook reads it) */
    unsigned aa;                              /* PCV_AA_* */
    unsigned af;                              /* anisotropic filtering: 1 (off), 2, 4, 8 or 16 */
    unsigned dof;                             /* depth of field, PCV_STEP_* */
    unsigned sharpen;                         /* sharpening, PCV_STEP_* */
    unsigned ao_method;                       /* ambient occlusion, PCV_AO_* */
    unsigned ao_quality;                      /* ambient occlusion sampling, PCV_AOQ_* */
    unsigned reflections;                     /* PCV_REFL_* */
    unsigned ray_tracing;                     /* 1 = ray-traced effects (experimental), see pc_video_rt_support */
    unsigned crosshair;                       /* PCX_* (pc_crosshair.h): ORIGINAL is the game's own dot alone */
    unsigned hitmarkers;                      /* 1 = a marker when the player kills (pc_crosshair.h) */
    unsigned optional_secondaries;             /* 1 = mission completion only requires primary objectives */
    unsigned lod_distance;                     /* percentage of native object/geometry LOD distance */
    unsigned bullet_damage;                    /* percentage of player bullet damage against enemies */
    unsigned enemy_health;                     /* percentage of enemy health at spawn */
    unsigned headshot_instakill;                /* 1 = confirmed player headshots kill enemies */
    unsigned sprint;                           /* optional PC movement, off by default */
    unsigned jump;
    unsigned aim_hold;                         /* 0 = original toggle, 1 = hold the aim button */
    unsigned split_screen;                    /* 1 = split-screen, player two in the bottom half (pc_split.h) */
    unsigned render_override_w, render_override_h; /* both non-zero: an explicit raster size (RECOMP_RENDER_*) */
} PcVideoSettings;

/* Fields that the environment pins for this process (bit set = locked). */
enum {
    PCV_LOCK_WINDOW_SIZE = 1u << 0, PCV_LOCK_WINDOW_MODE = 1u << 1, PCV_LOCK_INTERNAL = 1u << 2,
    PCV_LOCK_SCALE = 1u << 3, PCV_LOCK_FILTER = 1u << 4, PCV_LOCK_VSYNC = 1u << 5,
    PCV_LOCK_FPS = 1u << 6, PCV_LOCK_ASPECT = 1u << 7, PCV_LOCK_FOV = 1u << 8, PCV_LOCK_BLUR = 1u << 9,
    PCV_LOCK_AA = 1u << 10, PCV_LOCK_AF = 1u << 11, PCV_LOCK_DOF = 1u << 12, PCV_LOCK_SHARPEN = 1u << 13,
    PCV_LOCK_AO_METHOD = 1u << 14, PCV_LOCK_AO_QUALITY = 1u << 15, PCV_LOCK_REFLECTIONS = 1u << 16,
    PCV_LOCK_RAY_TRACING = 1u << 17, PCV_LOCK_CROSSHAIR = 1u << 18, PCV_LOCK_HITMARKERS = 1u << 19,
    PCV_LOCK_SPLIT = 1u << 20, PCV_LOCK_OPTIONAL_SECONDARIES = 1u << 21,
    PCV_LOCK_LOD_DISTANCE = 1u << 22, PCV_LOCK_BULLET_DAMAGE = 1u << 23,
    PCV_LOCK_ENEMY_HEALTH = 1u << 24, PCV_LOCK_HEADSHOT_INSTAKILL = 1u << 25
};

extern const unsigned pc_video_internal_heights[];   /* 480 .. 2160 */
extern const unsigned pc_video_internal_height_count;
extern const unsigned pc_video_fps_limits[];         /* 0 (unlimited), 30, 60, ... */
extern const unsigned pc_video_fps_limit_count;
extern const unsigned pc_video_af_levels[];          /* 1 (off), 2, 4, 8, 16 */
extern const unsigned pc_video_af_level_count;
extern const unsigned pc_video_lod_distances[], pc_video_lod_distance_count;
extern const unsigned pc_video_bullet_damages[], pc_video_bullet_damage_count;
extern const unsigned pc_video_enemy_healths[], pc_video_enemy_health_count;

/* Reads the settings file and the environment once (idempotent, thread safe). */
void pc_video_init(void);
void pc_video_get(PcVideoSettings *out);
/* Presenter thread only: import launcher edits, at most four file reads per second. */
void pc_video_poll_file(void);
unsigned pc_video_locked(void);
/* Bumped by every successful pc_video_set. Cheap enough to poll every frame. */
unsigned pc_video_generation(void);
/* Validates and publishes new values (locked fields keep the environment's value) and writes the
 * settings file. Returns 0 when a value was out of range (nothing changes). */
int pc_video_set(const PcVideoSettings *wanted);
/* Back to the built-in defaults (locked fields excepted). */
void pc_video_reset(void);
/* Remember the window mode chosen outside the menu (F11) without announcing it as a change. */
void pc_video_note_borderless(unsigned borderless);

/* The internal resolution: what the game's picture is, and what is presented. */
void pc_video_render_size_for(const PcVideoSettings *s, unsigned *w, unsigned *h);
void pc_video_render_size(unsigned *w, unsigned *h);
/* What the GPU renderer rasterises at: the internal resolution, doubled in each direction under SSAA (when that
 * fits the renderer's limits; otherwise SSAA is not available and it is the internal resolution). */
void pc_video_raster_size_for(const PcVideoSettings *s, unsigned *w, unsigned *h);
void pc_video_raster_size(unsigned *w, unsigned *h);
unsigned pc_video_ssaa_factor_for(const PcVideoSettings *s);   /* 1 or 2 */
/* The camera's aspect as a ratio, or 0/0 for the game's own. */
void pc_video_aspect_ratio(unsigned aspect, unsigned *num, unsigned *den);
const char *pc_video_aspect_name(unsigned aspect);       /* "original", "4:3", "16:9", ... */

/* Cheap reads for the guest hooks (the camera runs these every frame). */
unsigned pc_video_aspect(void);
double pc_video_fov(void);
int pc_video_motion_blur_off(void);
unsigned pc_video_split(void);

/* A renderer that can change its raster size while running registers here; the new sizes (the raster it draws at and
 * the internal resolution it presents) are delivered on the thread that calls pc_video_set. Without one, the internal resolution applies
 * at the next start (the menu says so). */
typedef void (*PcVideoResizeHook)(unsigned raster_w, unsigned raster_h, unsigned out_w, unsigned out_h);
void pc_video_set_resize_hook(PcVideoResizeHook hook);
int pc_video_live_resize(void);

/* Ray tracing needs a GPU with DirectX Raytracing and starts with the game: what the renderer can do with the setting.
 * UNAVAILABLE without such a GPU (or a renderer that answers); LIVE when it is running, so the setting applies at
 * once; RESTART when the game started without it. The renderer's query may take a moment the first time. */
enum { PCV_RT_UNAVAILABLE, PCV_RT_LIVE, PCV_RT_RESTART };
typedef int (*PcVideoRtSupport)(void);
void pc_video_set_rt_support(PcVideoRtSupport query);
int pc_video_rt_support(void);

/* The settings file's text for these settings (also what the launcher and tests parse). */
size_t pc_video_format(const PcVideoSettings *s, char *out, size_t cap);
/* Parses settings-file text over `inout`; unknown and invalid keys are skipped. Returns the number of keys applied. */
int pc_video_parse(const char *text, PcVideoSettings *inout);
void pc_video_defaults(PcVideoSettings *out);
/* Tests only: forget everything and read the environment again; `path` overrides the file. */
void pc_video_test_reset(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_PC_VIDEO_H */
