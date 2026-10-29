/*
 * pc_photo.c -- photo mode: a free camera over the paused game, and photos of
 * it. See pc_photo.h.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "pc_photo.h"
#include "../input/pc_input.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlobj.h>
#include <windows.h>

#define PCP_PI 3.14159265358979323846
#define PCP_DEG (PCP_PI / 180.0)

/* ---- tuning ---- */

static const double k_speeds[] = {0.75, 1.5, 3.0,
                                  6.0}; /* metres a second: slow .. very fast */
static const char *const k_speed_names[] = {"SLOW", "NORMAL", "FAST",
                                            "VERY FAST"};
static const double k_ranges[] = {6.0, 12.0, 20.0, 30.0,
                                  0.0}; /* metres from the start; zero = free */
#define PCP_BELOW 3.0 /* how far below the start the camera may go */
#define PCP_MOUSE_DEG                                                          \
  0.06 /* degrees a mouse count, as gameplay at sensitivity 1 */
#define PCP_STICK_DEG 110.0 /* degrees a second, stick fully over */
#define PCP_ROLL_DEG 45.0   /* degrees a second while Z / C is held */
#define PCP_ZOOM_MIN                                                           \
  0.15 /* view window: about 11 degrees vertically at the game's 70 */
#define PCP_ZOOM_MAX 1.7
#define PCP_IDLE_SHOW                                                          \
  0.6 /* seconds without camera input before the panel comes back */
#define PCP_STATUS_SECONDS 4.0

/* ---- input (input threads) ---- */

enum {
  K_W = 1,
  K_A = 2,
  K_S = 4,
  K_D = 8,
  K_Q = 16,
  K_E = 32,
  K_Z = 64,
  K_C = 128,
  K_FAST = 256,
  K_SLOW = 512
};
static volatile LONG s_keys, s_mouse_dx, s_mouse_dy, s_wheel, s_shot_request,
    s_panel_toggle;
static volatile LONG
    s_pad[8]; /* left x, left y, right x, right y (-32768..32767), LT, RT,
                 White, Black (0..255) */
static volatile LONG s_pad_buttons; /* X, Y held: edges are taken here */
static volatile LONG s_active;      /* the photo page is on screen */
static volatile LONG
    s_shutter; /* a photo was taken: the menu plays its sound */
static volatile LONG64 s_input_at; /* when camera input last arrived (ms) */

static const PcPhotoHost *s_host;

/* ---- the camera (game thread) ---- */

static SRWLOCK s_lock = SRWLOCK_INIT;
static int
    s_started; /* the camera has taken the game's pose for this session */
static double s_pos[3], s_anchor[3], s_yaw, s_pitch, s_roll, s_zoom = 1.0;
static double s_hand = 1.0,
              s_up_sign =
                  1.0; /* the game's handedness, read from its own matrix */
static int s_speed = 1, s_range = 1, s_panel_hidden, s_show_weapon;
static float s_half_y;
static LONGLONG s_last_qpc;
static char s_status[160];
static ULONGLONG s_status_at;
static int s_waiting_result;

void pc_photo_set_host(const PcPhotoHost *host) { s_host = host; }

int pc_photo_active(void) {
  return InterlockedCompareExchange(&s_active, 0, 0) != 0;
}

static ULONGLONG now_ms(void) { return GetTickCount64(); }

static void note_input(void) {
  InterlockedExchange64(&s_input_at, (LONG64)now_ms());
}

int pc_photo_panel_hidden(void) {
  int hidden;
  if (!pc_photo_active())
    return 0;
  AcquireSRWLockShared(&s_lock);
  hidden = s_panel_hidden;
  ReleaseSRWLockShared(&s_lock);
  /* moving the camera clears the picture; the panel comes back when it stops */
  return hidden ||
         now_ms() - (ULONGLONG)InterlockedCompareExchange64(&s_input_at, 0, 0) <
             (ULONGLONG)(PCP_IDLE_SHOW * 1000);
}

static int key_bit(unsigned id) {
  switch (id) {
  case 0x11:
    return K_W;
  case 0x1E:
    return K_A;
  case 0x1F:
    return K_S;
  case 0x20:
    return K_D;
  case 0x10:
    return K_Q;
  case 0x12:
    return K_E;
  case 0x2C:
    return K_Z;
  case 0x2E:
    return K_C;
  case 0x2A:
  case 0x36:
    return K_FAST; /* either Shift */
  case 0x1D:
  case 0x11D:
    return K_SLOW; /* either Ctrl */
  default:
    return 0;
  }
}

static int f_key(unsigned id, int down, int repeat) {
  const int bit = key_bit(id);
  if (!pc_photo_active()) {
    /* a camera key held when photo mode ended is let go */
    if (!down && bit && (InterlockedCompareExchange(&s_keys, 0, 0) & bit)) {
      InterlockedAnd(&s_keys, ~bit);
      return 1;
    }
    return 0;
  }
  if (bit) {
    if (down)
      InterlockedOr(&s_keys, bit);
    else
      InterlockedAnd(&s_keys, ~bit);
    if (down && bit != K_FAST && bit != K_SLOW)
      note_input();
    return 1;
  }
  if (id == 0x58) { /* F12 */
    if (down && !repeat)
      InterlockedExchange(&s_shot_request, 1);
    return 1;
  }
  if (id == 0x23) { /* H */
    if (down && !repeat)
      InterlockedExchange(&s_panel_toggle, 1);
    return 1;
  }
  return 0;
}

static int f_wheel(int delta) {
  if (!pc_photo_active() || !delta)
    return 0;
  InterlockedExchangeAdd(&s_wheel, delta > 0 ? 1 : -1);
  note_input();
  return 1;
}

static int f_motion(long dx, long dy) {
  if (!pc_photo_active())
    return 0;
  if (dx)
    InterlockedExchangeAdd(&s_mouse_dx, dx);
  if (dy)
    InterlockedExchangeAdd(&s_mouse_dy, dy);
  if (labs(dx) + labs(dy) > 1)
    note_input();
  return 1;
}

static int stick(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

/* The camera's controls are taken out of the report; the d-pad, A, B and START
 * stay for the panel. */
static int f_report(uint8_t out[20], int pad_present) {
  static const int dead = 7800; /* about a quarter of the throw */
  LONG held, prev;
  int i, moving = 0;
  if (!pc_photo_active()) {
    for (i = 0; i < 8; i++)
      InterlockedExchange(&s_pad[i], 0);
    InterlockedExchange(&s_pad_buttons, 0);
    return 0;
  }
  if (!pad_present)
    return 0;
  for (i = 0; i < 4; i++) {
    int v = stick(out + 12 + i * 2);
    if (v > -dead && v < dead)
      v = 0;
    else
      moving = 1;
    InterlockedExchange(&s_pad[i], v);
    out[12 + i * 2] = out[13 + i * 2] = 0;
  }
  InterlockedExchange(&s_pad[4], out[10]);
  InterlockedExchange(&s_pad[5], out[11]); /* LT RT */
  InterlockedExchange(&s_pad[6], out[9]);
  InterlockedExchange(&s_pad[7], out[8]); /* White Black */
  if (out[10] > 30 || out[11] > 30 || out[8] > 30 || out[9] > 30)
    moving = 1;
  held = (out[6] > 100 ? 1 : 0) | (out[7] > 100 ? 2 : 0); /* X Y */
  prev = InterlockedExchange(&s_pad_buttons, held);
  if ((held & 2) && !(prev & 2))
    InterlockedExchange(&s_shot_request, 1);
  if ((held & 1) && !(prev & 1))
    InterlockedExchange(&s_panel_toggle, 1);
  for (i = 6; i <= 11; i++)
    out[i] = 0; /* X Y Black White LT RT */
  if (moving)
    note_input();
  return 0;
}

static void f_focus(int focused) {
  if (focused)
    return;
  InterlockedExchange(&s_keys, 0);
  InterlockedExchange(&s_mouse_dx, 0);
  InterlockedExchange(&s_mouse_dy, 0);
}

static int capture_wanted(void) { return pc_photo_active(); }

static const PcInputFilter k_filter = {f_key,    NULL,    f_wheel,
                                       f_report, f_focus, f_motion};

void pc_photo_install(void) {
  pc_input_set_filter_slot(3, &k_filter);
  pc_input_set_capture_query(capture_wanted);
}

/* ---- the menus ---- */

int pc_photo_allowed(void) {
  return s_host && s_host->can_start && s_host->can_start();
}

void pc_photo_set_shown(int shown) {
  shown = shown ? 1 : 0;
  if (InterlockedExchange(&s_active, shown) == shown)
    return;
  AcquireSRWLockExclusive(&s_lock);
  if (shown) {
    s_started = 0;
    s_panel_hidden = 0;
    s_status[0] = 0;
    s_waiting_result = 0;
  }
  ReleaseSRWLockExclusive(&s_lock);
  InterlockedExchange(&s_keys, 0);
  InterlockedExchange(&s_wheel, 0);
  InterlockedExchange(&s_mouse_dx, 0);
  InterlockedExchange(&s_mouse_dy, 0);
  InterlockedExchange(&s_shot_request, 0);
  InterlockedExchange(&s_panel_toggle, 0);
  InterlockedExchange64(&s_input_at, 0);
  fprintf(stderr, "[PHOTO] photo mode %s\n", shown ? "on" : "off");
}

const char *pc_photo_row_label(int row) {
  switch (row) {
  case PCP_ROW_FOV:
    return "FIELD OF VIEW";
  case PCP_ROW_ROLL:
    return "ROLL";
  case PCP_ROW_WEAPON:
    return "WEAPON";
  case PCP_ROW_SPEED:
    return "CAMERA SPEED";
  case PCP_ROW_LIMIT:
    return "CAMERA RANGE";
  default:
    return "";
  }
}

size_t pc_photo_row_value(int row, char *out, size_t cap) {
  int n = 0;
  if (!out || !cap)
    return 0;
  AcquireSRWLockShared(&s_lock);
  switch (row) {
  case PCP_ROW_FOV:
    n = s_half_y > 0
            ? snprintf(out, cap, "%d",
                       (int)floor(2.0 * atan((double)s_half_y) / PCP_DEG + 0.5))
            : snprintf(out, cap, "-");
    break;
  case PCP_ROW_ROLL:
    n = snprintf(out, cap, "%d",
                 (int)floor(s_roll / PCP_DEG + (s_roll >= 0 ? 0.5 : -0.5)));
    break;
  case PCP_ROW_WEAPON:
    n = snprintf(out, cap, "%s", s_show_weapon ? "SHOWN" : "HIDDEN");
    break;
  case PCP_ROW_SPEED:
    n = snprintf(out, cap, "%s", k_speed_names[s_speed]);
    break;
  case PCP_ROW_LIMIT:
    n = k_ranges[s_range] > 0
            ? snprintf(out, cap, "%d M", (int)k_ranges[s_range])
            : snprintf(out, cap, "FREE");
    break;
  default:
    out[0] = 0;
    break;
  }
  ReleaseSRWLockShared(&s_lock);
  return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

int pc_photo_row_change(int row, int dir) {
  int did = 0;
  if (!dir)
    return 0;
  AcquireSRWLockExclusive(&s_lock);
  switch (row) {
  case PCP_ROW_FOV: {
    const double z = clampd(s_zoom * (dir > 0 ? 1.08 : 1.0 / 1.08),
                            PCP_ZOOM_MIN, PCP_ZOOM_MAX);
    did = z != s_zoom;
    s_zoom = z;
    break;
  }
  case PCP_ROW_ROLL: {
    /* five-degree steps, snapping a hand-rolled angle onto the grid */
    const double steps =
        floor(s_roll / (5.0 * PCP_DEG) + 0.5) + (dir > 0 ? 1 : -1);
    s_roll = clampd(steps * 5.0 * PCP_DEG, -90.0 * PCP_DEG, 90.0 * PCP_DEG);
    did = 1;
    break;
  }
  case PCP_ROW_WEAPON:
    s_show_weapon = !s_show_weapon;
    did = 1;
    break;
  case PCP_ROW_SPEED: {
    const int v = s_speed + (dir > 0 ? 1 : -1);
    if (v >= 0 && v < (int)(sizeof(k_speeds) / sizeof(k_speeds[0]))) {
      s_speed = v;
      did = 1;
    }
    break;
  }
  case PCP_ROW_LIMIT: {
    const int v = s_range + (dir > 0 ? 1 : -1);
    if (v >= 0 && v < (int)(sizeof(k_ranges) / sizeof(k_ranges[0]))) {
      s_range = v;
      did = 1;
    }
    break;
  }
  default:
    break;
  }
  ReleaseSRWLockExclusive(&s_lock);
  return did;
}

void pc_photo_hide_panel(void) { InterlockedExchange(&s_panel_toggle, 1); }

int pc_photo_panel_put_away(void) {
  int hidden;
  AcquireSRWLockShared(&s_lock);
  hidden = s_panel_hidden;
  ReleaseSRWLockShared(&s_lock);
  return pc_photo_active() && hidden;
}

void pc_photo_show_panel(void) {
  AcquireSRWLockExclusive(&s_lock);
  s_panel_hidden = 0;
  ReleaseSRWLockExclusive(&s_lock);
}

int pc_photo_take(void) {
  if (!pc_photo_active())
    return 0;
  InterlockedExchange(&s_shot_request, 1);
  return 1;
}

void pc_photo_reset(void) {
  AcquireSRWLockExclusive(&s_lock);
  s_started = 0; /* the next camera update takes the game's pose again */
  s_zoom = 1.0;
  ReleaseSRWLockExclusive(&s_lock);
}

const char *pc_photo_help(void) {
  static char text[200];
  AcquireSRWLockExclusive(&s_lock);
  if (s_waiting_result && s_host && s_host->capture_result) {
    const int r = s_host->capture_result();
    if (r != 0) {
      if (r < 0)
        snprintf(s_status, sizeof(s_status), "THE PHOTO COULD NOT BE SAVED.");
      s_waiting_result = 0;
      s_status_at = now_ms();
    }
  }
  if (s_status[0] &&
      (s_waiting_result ||
       now_ms() - s_status_at < (ULONGLONG)(PCP_STATUS_SECONDS * 1000)))
    snprintf(text, sizeof(text), "%s", s_status);
  else if (pc_input_prompt_device() == PC_DEVICE_CONTROLLER)
    snprintf(text, sizeof(text),
             "LEFT STICK MOVES, RIGHT STICK LOOKS, TRIGGERS DOWN AND UP, WHITE "
             "AND BLACK ZOOM. Y TAKES A PHOTO, X HIDES THIS PANEL.");
  else
    snprintf(text, sizeof(text),
             "WASD MOVES, THE MOUSE LOOKS, Q AND E DOWN AND UP, SHIFT AND CTRL "
             "SPEED, WHEEL ZOOMS, Z AND C ROLL. F12 TAKES A PHOTO, H HIDES "
             "THIS PANEL.");
  ReleaseSRWLockExclusive(&s_lock);
  return text;
}

/* ---- photos ---- */

/* Pictures\BLACK (RECOMP_PHOTO_DIR moves it), created when missing; a name from
 * the local time, never an old one. */
static int photo_path(char *out, size_t cap, char *name, size_t name_cap) {
  char dir[MAX_PATH];
  const char *env = getenv("RECOMP_PHOTO_DIR");
  SYSTEMTIME t;
  int i;
  if (env && *env) {
    snprintf(dir, sizeof(dir), "%s", env);
  } else {
    PWSTR pictures = NULL;
    char base[MAX_PATH];
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Pictures, KF_FLAG_CREATE, NULL,
                                    &pictures)) ||
        !WideCharToMultiByte(CP_ACP, 0, pictures, -1, base, sizeof(base), NULL,
                             NULL)) {
      CoTaskMemFree(pictures);
      return 0;
    }
    CoTaskMemFree(pictures);
    snprintf(dir, sizeof(dir), "%s\\BLACK", base);
  }
  CreateDirectoryA(dir, NULL);
  GetLocalTime(&t);
  for (i = 1; i < 100; i++) {
    if (i == 1)
      snprintf(name, name_cap, "BLACK_%04u-%02u-%02u_%02u-%02u-%02u.png",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else
      snprintf(name, name_cap, "BLACK_%04u-%02u-%02u_%02u-%02u-%02u_%d.png",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, i);
    snprintf(out, cap, "%s\\%s", dir, name);
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES)
      return 1;
  }
  return 0;
}

static void shoot(void) {
  char path[MAX_PATH + 64], name[64];
  if (!s_host || !s_host->capture)
    return;
  if (!photo_path(path, sizeof(path), name, sizeof(name))) {
    snprintf(s_status, sizeof(s_status), "THE PHOTO COULD NOT BE SAVED.");
    s_status_at = now_ms();
    return;
  }
  s_host->capture(path);
  snprintf(s_status, sizeof(s_status), "PHOTO SAVED TO PICTURES\\BLACK: %s",
           name);
  s_status_at = now_ms();
  s_waiting_result = 1;
  InterlockedExchange(&s_shutter, 1);
  fprintf(stderr, "[PHOTO] photo %s\n", path);
}

/* ---- the camera ---- */

static void cross3(const double a[3], const double b[3], double out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

static double dot3(const double a[3], const double b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static int norm3(double v[3]) {
  const double l = sqrt(dot3(v, v));
  if (!(l > 1e-9))
    return 0;
  v[0] /= l;
  v[1] /= l;
  v[2] /= l;
  return 1;
}

/* The camera's frame from yaw and pitch (world Y is up): forward, and right /
 * up before roll. */
static void basis(double yaw, double pitch, double f[3], double r[3],
                  double u[3]) {
  static const double world_up[3] = {0, 1, 0};
  f[0] = cos(pitch) * sin(yaw);
  f[1] = sin(pitch);
  f[2] = cos(pitch) * cos(yaw);
  cross3(world_up, f, r);
  if (!norm3(r)) {
    r[0] = cos(yaw);
    r[1] = 0;
    r[2] = -sin(yaw);
  }
  r[0] *= s_hand;
  r[1] *= s_hand;
  r[2] *= s_hand;
  cross3(f, r, u);
  norm3(u);
  u[0] *= s_up_sign;
  u[1] *= s_up_sign;
  u[2] *= s_up_sign;
}

static void start(const float m[4][3]) {
  double f[3] = {m[2][0], m[2][1], m[2][2]}, r[3], u[3],
         gr[3] = {m[0][0], m[0][1], m[0][2]},
         gu[3] = {m[1][0], m[1][1], m[1][2]};
  int k;
  if (!norm3(f)) {
    f[0] = 0;
    f[1] = 0;
    f[2] = 1;
  }
  s_yaw = atan2(f[0], f[2]);
  s_pitch = asin(clampd(f[1], -1.0, 1.0));
  s_hand = 1.0;
  s_up_sign = 1.0;
  basis(s_yaw, s_pitch, f, r, u);
  if (dot3(r, gr) < 0)
    s_hand = -1.0; /* the game's right is the other cross product */
  basis(s_yaw, s_pitch, f, r, u);
  if (dot3(u, gu) < 0)
    s_up_sign = -1.0;
  s_roll = 0;
  for (k = 0; k < 3; k++)
    s_pos[k] = s_anchor[k] = m[3][k];
  s_started = 1;
  fprintf(stderr,
          "[PHOTO] camera from the game's at %.2f %.2f %.2f, yaw %.1f pitch "
          "%.1f (hand %+.0f, up %+.0f, game up y %.2f)\n",
          s_pos[0], s_pos[1], s_pos[2], s_yaw / PCP_DEG, s_pitch / PCP_DEG,
          s_hand, s_up_sign, (double)m[1][1]);
}

static double axis(LONG v) { return v / 32767.0; }

int pc_photo_camera(float m[4][3], float half_y) {
  LARGE_INTEGER q, freq;
  double dt, f[3], r[3], u[3], move[3] = {0, 0, 0}, speed, rr[3], ru[3], dx, dz,
                               dist;
  LONG keys, mdx, mdy, wheel;
  int k;
  if (!pc_photo_active())
    return 0;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&freq);
  AcquireSRWLockExclusive(&s_lock);
  s_half_y = half_y;
  if (!s_started) {
    start(m);
    s_last_qpc = q.QuadPart;
  }
  dt = clampd((double)(q.QuadPart - s_last_qpc) / (double)freq.QuadPart, 0.0,
              0.1);
  s_last_qpc = q.QuadPart;
  if (InterlockedExchange(&s_panel_toggle, 0))
    s_panel_hidden = !s_panel_hidden;

  /* looking: the mouse in counts, the right stick in degrees a second */
  keys = InterlockedCompareExchange(&s_keys, 0, 0);
  mdx = InterlockedExchange(&s_mouse_dx, 0);
  mdy = InterlockedExchange(&s_mouse_dy, 0);
  /* the screen's right is the camera matrix's left (r below): turning and
   * moving sideways go the other way */
  s_yaw -= s_hand * (mdx * PCP_MOUSE_DEG * PCP_DEG +
                     axis(s_pad[2]) * PCP_STICK_DEG * PCP_DEG * dt);
  s_pitch += -mdy * PCP_MOUSE_DEG * PCP_DEG +
             axis(s_pad[3]) * PCP_STICK_DEG * PCP_DEG * dt;
  s_pitch = clampd(s_pitch, -89.0 * PCP_DEG, 89.0 * PCP_DEG);
  s_yaw = fmod(s_yaw, 2.0 * PCP_PI);
  if (keys & K_Z)
    s_roll -= PCP_ROLL_DEG * PCP_DEG * dt;
  if (keys & K_C)
    s_roll += PCP_ROLL_DEG * PCP_DEG * dt;
  s_roll = clampd(s_roll, -90.0 * PCP_DEG, 90.0 * PCP_DEG);

  /* zoom: wheel notches, White / Black held */
  wheel = InterlockedExchange(&s_wheel, 0);
  s_zoom *= pow(1.08, -wheel);
  s_zoom *= exp(((s_pad[6] - s_pad[7]) / 255.0) * 0.9 * dt);
  s_zoom = clampd(s_zoom, PCP_ZOOM_MIN, PCP_ZOOM_MAX);

  /* moving: along where the camera looks, sideways level with the ground,
   * straight up and down */
  basis(s_yaw, s_pitch, f, r, u);
  {
    const double fwd =
        ((keys & K_W) ? 1 : 0) - ((keys & K_S) ? 1 : 0) + axis(s_pad[1]);
    const double side =
        ((keys & K_D) ? 1 : 0) - ((keys & K_A) ? 1 : 0) + axis(s_pad[0]);
    const double vert = ((keys & K_E) ? 1 : 0) - ((keys & K_Q) ? 1 : 0) +
                        (s_pad[5] - s_pad[4]) / 255.0;
    double flat[3] = {r[0], 0, r[2]};
    if (!norm3(flat)) {
      flat[0] = r[0];
      flat[2] = r[2];
    }
    for (k = 0; k < 3; k++)
      move[k] = f[k] * fwd - flat[k] * side;
    move[1] += vert;
  }
  speed = k_speeds[s_speed] * ((keys & K_FAST) ? 3.0 : 1.0) *
          ((keys & K_SLOW) ? 0.25 : 1.0);
  for (k = 0; k < 3; k++)
    s_pos[k] += clampd(move[k], -1.5, 1.5) * speed * dt;

  /* Bounded ranges stay near the start; FREE skips both distance and height
   * limits. */
  if (k_ranges[s_range] > 0) {
    dx = s_pos[0] - s_anchor[0];
    dz = s_pos[2] - s_anchor[2];
    dist = sqrt(dx * dx + dz * dz);
    if (dist > k_ranges[s_range]) {
      s_pos[0] = s_anchor[0] + dx * k_ranges[s_range] / dist;
      s_pos[2] = s_anchor[2] + dz * k_ranges[s_range] / dist;
    }
    s_pos[1] = clampd(s_pos[1], s_anchor[1] - PCP_BELOW,
                      s_anchor[1] + k_ranges[s_range] * 0.5);
  }

  if (InterlockedExchange(&s_shot_request, 0))
    shoot();

  /* roll turns right and up about forward */
  for (k = 0; k < 3; k++) {
    rr[k] = r[k] * cos(s_roll) + u[k] * sin(s_roll);
    ru[k] = u[k] * cos(s_roll) - r[k] * sin(s_roll);
  }
  for (k = 0; k < 3; k++) {
    m[0][k] = (float)rr[k];
    m[1][k] = (float)ru[k];
    m[2][k] = (float)f[k];
    m[3][k] = (float)s_pos[k];
  }
  ReleaseSRWLockExclusive(&s_lock);
  return 1;
}

int pc_photo_weapon_hidden(void) {
  int hidden;
  if (!pc_photo_active())
    return 0;
  AcquireSRWLockShared(&s_lock);
  hidden = !s_show_weapon;
  ReleaseSRWLockShared(&s_lock);
  return hidden;
}

float pc_photo_zoom(void) {
  double z;
  if (!pc_photo_active())
    return 1.0f;
  AcquireSRWLockShared(&s_lock);
  z = s_zoom;
  ReleaseSRWLockShared(&s_lock);
  return (float)z;
}

int pc_photo_shutter(void) { return InterlockedExchange(&s_shutter, 0) != 0; }

#else
void pc_photo_set_host(const PcPhotoHost *host) { (void)host; }
void pc_photo_install(void) {}
int pc_photo_allowed(void) { return 0; }
void pc_photo_set_shown(int shown) { (void)shown; }
int pc_photo_active(void) { return 0; }
int pc_photo_panel_hidden(void) { return 0; }
const char *pc_photo_row_label(int row) {
  (void)row;
  return "";
}
size_t pc_photo_row_value(int row, char *out, size_t cap) {
  (void)row;
  if (out && cap)
    out[0] = 0;
  return 0;
}
int pc_photo_row_change(int row, int dir) {
  (void)row;
  (void)dir;
  return 0;
}
int pc_photo_take(void) { return 0; }
void pc_photo_hide_panel(void) {}
int pc_photo_panel_put_away(void) { return 0; }
void pc_photo_show_panel(void) {}
void pc_photo_reset(void) {}
const char *pc_photo_help(void) { return ""; }
int pc_photo_camera(float m[4][3], float half_y) {
  (void)m;
  (void)half_y;
  return 0;
}
float pc_photo_zoom(void) { return 1.0f; }
int pc_photo_weapon_hidden(void) { return 0; }
int pc_photo_shutter(void) { return 0; }
#endif
