/*
 * pc_video_menu.h -- video settings services and the legacy presenter popup.
 *
 * pc_menu's native pages use the row services for the settings model, formatted values and help.
 * Installing a native opener sends V / RB and explicit opens to those pages. Without an opener,
 * the legacy popup is drawn by the presenter over the finished frame. It is offered while an
 * Options page is on screen (pc_video_menu_page_seen every frame), and takes keyboard, mouse and
 * pad input until Escape / B / right click closes it.
 *
 * Threads: key / button / wheel / report filters run on whichever thread delivers the input (the
 * window thread, or the OHCI thread for scripted input and the pad); the overlay is rendered by
 * the presenter thread only. All state is behind one lock.
 */
#ifndef XBOXRECOMP_PC_VIDEO_MENU_H
#define XBOXRECOMP_PC_VIDEO_MENU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The rows of the page, top to bottom (the last two are actions). */
enum {
    PCV_ROW_MODE, PCV_ROW_WINDOW, PCV_ROW_INTERNAL, PCV_ROW_AA, PCV_ROW_AF, PCV_ROW_DOF, PCV_ROW_SHARPEN,
    PCV_ROW_AO_METHOD, PCV_ROW_AO_QUALITY, PCV_ROW_REFLECTIONS, PCV_ROW_RAY_TRACING, PCV_ROW_ASPECT,
    PCV_ROW_FOV, PCV_ROW_SCALE, PCV_ROW_FILTER, PCV_ROW_VSYNC, PCV_ROW_FPS, PCV_ROW_BLUR, PCV_ROW_CROSSHAIR,
    PCV_ROW_HITMARKERS, PCV_ROW_OPTIONAL_SECONDARIES, PCV_ROW_LOD_DISTANCE, PCV_ROW_BULLET_DAMAGE,
    PCV_ROW_ENEMY_HEALTH, PCV_ROW_HEADSHOT_INSTAKILL, PCV_ROW_AIM_MODE, PCV_ROW_SPRINT, PCV_ROW_JUMP, PCV_ROW_RESET, PCV_ROW_DONE,
    PCV_ROW_COUNT
};

/* Renderer-independent row services for the game's native menu pages. Labels, values and help are
 * UTF-8; value writes at most cap-1 bytes, always terminates when cap is nonzero, and returns the
 * number of bytes written. Values stay short; locks, resize/restart and GPU availability are in
 * help. Changing a row applies and persists one choice, returning 1 on success, 0 for a locked or
 * invalid row (or dir == 0). No UI callbacks are invoked by these services. */
const char *pc_video_menu_row_label(int row);
size_t pc_video_menu_row_value(int row, char *out, size_t cap);
const char *pc_video_menu_row_help(int row);
int pc_video_menu_row_locked(int row);
int pc_video_menu_row_change(int row, int dir);
void pc_video_menu_reset_settings(void);

/* Route V / RB and pc_video_menu_open through the native menu instead of the legacy popup.
 * The callback runs on the input/calling thread, outside this module's lock: it must only queue
 * an action for the game thread. NULL restores the legacy popup. Installing a delegate closes
 * any legacy popup immediately. */
void pc_video_menu_set_native_opener(void (*request)(void));

/* The game is showing an Options page (any thread; call it every frame the page is drawn). */
void pc_video_menu_page_seen(void);

/* Registers the input filters with pc_input. Called once by the presenter. */
void pc_video_menu_install(void);

int pc_video_menu_is_open(void);

/* Opens the page (the native Options page's VIDEO SETTINGS item). It stays open while the page under it is
 * reported with pc_video_menu_page_seen, like a page opened with V. */
void pc_video_menu_open(void);

/* A native menu draws its own way in: hide the [V] / [RB] hint while one is on screen (any thread). */
void pc_video_menu_hide_hint(int hide);

/* Presenter thread: what to draw on top of the frame for a client area of w x h pixels. Returns 1 when
 * something is to be drawn; *pixels is then w*h premultiplied BGRA pixels, valid until the next call,
 * and *changed says whether they differ from the previous call's. */
int pc_video_menu_overlay(unsigned w, unsigned h, const uint32_t **pixels, int *changed);

/* The overlay's actions, for tests: 0 up, 1 down, 2 left, 3 right, 4 accept, 5 back, 6 open. */
void pc_video_menu_test_action(int action);
void pc_video_menu_test_force_available(int on);
int pc_video_menu_test_selected(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_PC_VIDEO_MENU_H */
