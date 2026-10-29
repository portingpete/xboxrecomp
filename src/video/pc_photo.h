/*
 * pc_photo.h -- photo mode: a free camera over the paused game, and photos of it.
 *
 * The native menus show a page marked `photo` (PhotoMode, opened from the pause menu) as a small panel over the
 * game picture instead of a full page, and photo mode is on while it is shown. Photo mode then takes the camera
 * controls ahead of the menus -- the panel keeps the arrows, Enter and Escape, the d-pad, A and B:
 *
 *   keyboard and mouse   WASD move, mouse look, Q / E down / up, Shift faster, Ctrl slower, wheel field of view,
 *                        Z / C roll, F12 takes a photo, H hides the panel
 *   controller           left stick move, right stick look, triggers down / up, White / Black field of view,
 *                        Y takes a photo, X hides the panel
 *
 * The camera starts where the game's camera is. Bounded ranges limit distance and height around that point;
 * FREE removes both limits to explore the loaded level. The game project owns guest memory: it asks pc_photo_camera for
 * the camera matrix every world-camera update and draws with it, scales its view window by pc_photo_zoom, and
 * writes the photos (PcPhotoHost).
 *
 * Threads: input filters on the input threads, pc_photo_camera on the game thread, the menu services on the game
 * thread. Shared state is a lock and interlocked words.
 */
#ifndef XBOXRECOMP_PC_PHOTO_H
#define XBOXRECOMP_PC_PHOTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcPhotoHost {
    /* Photo mode may start now (a paused mission, no scripted camera). */
    int (*can_start)(void);
    /* Write the next finished frame to this file (.png). */
    void (*capture)(const char *path);
    /* The last capture: 1 written, 0 still pending, -1 failed. */
    int (*capture_result)(void);
} PcPhotoHost;

void pc_photo_set_host(const PcPhotoHost *host);
/* Registers the input filter and the pointer capture with pc_input. Once, by the presenter. */
void pc_photo_install(void);

/* ---- the menus (game thread) ---- */

int pc_photo_allowed(void);
/* The photo page is shown (1) or not (0): photo mode follows it. */
void pc_photo_set_shown(int shown);
int pc_photo_active(void);
/* The panel is not drawn: the player put it away (H, X, HIDE PANEL) or is moving the camera. */
int pc_photo_panel_hidden(void);
/* Put away by the player, and brought back. */
void pc_photo_hide_panel(void);
int pc_photo_panel_put_away(void);
void pc_photo_show_panel(void);

/* The panel's rows. Values are short UTF-8; change applies dir -1 / +1 and returns 1 when something changed. */
enum { PCP_ROW_FOV, PCP_ROW_ROLL, PCP_ROW_WEAPON, PCP_ROW_SPEED, PCP_ROW_LIMIT, PCP_ROW_COUNT };
const char *pc_photo_row_label(int row);
size_t pc_photo_row_value(int row, char *out, size_t cap);
int pc_photo_row_change(int row, int dir);
/* Panel actions: take a photo; put the camera back where the game had it. */
int pc_photo_take(void);
void pc_photo_reset(void);
/* What the panel says under its rows: the controls, or what the last photo did. */
const char *pc_photo_help(void);
/* 1 once after each photo (the menu plays its sound for it). */
int pc_photo_shutter(void);

/* ---- the game (game thread) ---- */

/* The world camera as the game produced it this update: rows right, up, at (forward), position. While photo mode
 * is on, replaces them with the photo camera and returns 1. `half_y` is the camera's tan(vertical FOV / 2) after
 * pc_photo_zoom was applied, for the panel. */
int pc_photo_camera(float m[4][3], float half_y);
/* The first-person weapon is left out of the picture (the WEAPON row; hidden unless the player shows it). */
int pc_photo_weapon_hidden(void);
/* The view-window multiplier for the field of view: 1 = the game's, less is narrower. */
float pc_photo_zoom(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_PC_PHOTO_H */
