/*
 * pc_input.h -- native keyboard and mouse for BLACK.
 *
 * The game's only input route is an Xbox pad: XInputGetState -> xbox_InputGetState(0) ->
 * usb_gamepad_report -> OHCI -> the guest's statically linked XAPI -> the game's own 28-control
 * layer -> its action map. This module adds the keyboard and mouse to that route instead of
 * beside it:
 *
 *   - Keys and buttons resolve to BLACK's own action slots, and each slot to whatever physical
 *     control the live action map (or the active preset) binds it to, so an alternate controller
 *     preset changes where the keyboard lands exactly as it changes where the pad lands. The
 *     result is merged into the 20-byte pad report the guest already reads.
 *   - The mouse never goes through the right stick. Raw relative counts are accumulated on the
 *     UI thread and consumed once, by the guest's camera update, which adds degrees directly
 *     (no dead zone, no turn-rate cap, no smoothing).
 *   - Prompts: the game builds "press <button>" text from private-use tokens (U+F000 + action
 *     slot). While keyboard/mouse prompts are active, pc_input_expand_prompt() replaces those
 *     tokens with the real binding, inside a buffer whose capacity the caller proves.
 *
 * Threads: events arrive on the presenter (UI) thread; reports are built on the OHCI thread;
 * the camera update and prompt formatting run on the guest thread. Everything shared is either
 * a 32-bit interlocked word or written once at init.
 *
 * Configuration comes from RECOMP_BLACK_* environment variables (the launcher exports them):
 *   RECOMP_BLACK_INPUT_MODE        auto | keyboard_mouse | controller
 *   RECOMP_BLACK_PROMPT_MODE       auto | keyboard_mouse | controller
 *   RECOMP_BLACK_MOUSE_SENSITIVITY multiplier, 0.1 .. 10 (1 = 0.06 degrees per count)
 *   RECOMP_BLACK_MOUSE_INVERT_Y    0 | 1
 *   RECOMP_BLACK_BIND_<ACTION>     up to three of: A..Z 0..9 F1..F12 Space Enter Tab Backspace
 *                                  Up Down Left Right Shift LShift RShift Ctrl LCtrl RCtrl Alt
 *                                  LAlt RAlt Numpad0..9 Insert Delete Home End PageUp PageDown
 *                                  CapsLock Minus Equals LBracket RBracket Semicolon Quote Grave
 *                                  Comma Period Slash Backslash Mouse1..Mouse5 WheelUp WheelDown
 *                                  None; separated by commas.
 *   RECOMP_INPUT_SCRIPT            scripted host events for tests (see pc_input.c)
 */
#ifndef XBOXRECOMP_PC_INPUT_H
#define XBOXRECOMP_PC_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PcMode {
    PC_MODE_AUTO = 0,        /* keyboard/mouse and controller both play; prompts follow the last used */
    PC_MODE_KEYBOARD_MOUSE,  /* the pad is ignored; keyboard prompts */
    PC_MODE_CONTROLLER       /* keyboard/mouse gameplay is ignored; controller prompts */
} PcMode;

typedef enum PcDevice { PC_DEVICE_CONTROLLER = 0, PC_DEVICE_KEYBOARD_MOUSE = 1 } PcDevice;

/* The bindable actions: BLACK's own action slots (token U+F000 + slot). */
typedef enum PcAction {
    PC_ACTION_MOVE_FORWARD,
    PC_ACTION_MOVE_BACK,
    PC_ACTION_MOVE_LEFT,
    PC_ACTION_MOVE_RIGHT,
    PC_ACTION_LOOK_LEFT,
    PC_ACTION_LOOK_RIGHT,
    PC_ACTION_LOOK_UP,
    PC_ACTION_LOOK_DOWN,
    PC_ACTION_FIRE,
    PC_ACTION_AIM,
    PC_ACTION_RELOAD,
    PC_ACTION_CROUCH,
    PC_ACTION_USE,
    PC_ACTION_NEXT_WEAPON,
    PC_ACTION_PREV_WEAPON,
    PC_ACTION_GRENADE,
    PC_ACTION_FIRE_MODE,
    PC_ACTION_SUPPRESSOR,
    PC_ACTION_MELEE,
    PC_ACTION_HEALTH_PACK,
    PC_ACTION_CYCLE_ITEM,
    PC_ACTION_BACK,
    PC_ACTION_PAUSE,
    PC_ACTION_SPRINT,
    PC_ACTION_JUMP,
    PC_ACTION_COUNT
} PcAction;

#define PC_MAX_BINDINGS 3

typedef struct PcActionInfo {
    PcAction action;
    const char *name;            /* RECOMP_BLACK_BIND_<name> */
    const char *label;           /* for help text */
    int slot;                    /* BLACK's action slot; token U+F000 + slot */
    const char *default_binding; /* comma separated */
} PcActionInfo;

extern const PcActionInfo pc_input_actions[PC_ACTION_COUNT];

/* ---- lifecycle --------------------------------------------------------------------------- */

/* Reads the environment once (idempotent, thread safe) and logs the effective settings. */
void pc_input_init(void);
int pc_input_trace_enabled(void);   /* RECOMP_INPUT_TRACE: timestamped input/context transitions */
PcMode pc_input_input_mode(void);
PcMode pc_input_prompt_mode(void);

/* What a slot is bound to right now: the control id (0..27, BLACK's own numbering) or -1.
 * Installed by the game project, which owns the guest memory layout and reads its default preset
 * from the user's loaded XBE when the live map is not ready. Called on the OHCI thread. */
typedef int (*PcControlResolver)(int slot);
void pc_input_set_control_resolver(PcControlResolver resolver);
/* The game's aim hook publishes its current setting. Retain held Aim across
 * short camera gaps only in Hold mode; pause, movies and focus loss clear it. */
void pc_input_set_aim_hold_mode(int enabled);

/* ---- input filter -------------------------------------------------------------------------------- */

/* The native Video Settings page sits in front of the game's input. Each callback returns non-zero when
 * it took the event, and then the game never sees it. They run on whichever thread delivers the input
 * (the window thread; the OHCI thread for the pad and scripted events). `id` is the scancode, with
 * 0x100 set for an extended key. `report` gets the pad's 20 bytes before the keyboard merge; when it
 * takes the report the game is shown a neutral pad. */
typedef struct PcInputFilter {
    int (*key)(unsigned id, int down, int repeat);
    int (*button)(int button, int down);
    int (*wheel)(int delta);
    int (*report)(uint8_t out[20], int pad_present);
    void (*focus)(int focused); /* Optional notification; cancels transient confirmations. */
    /* Optional: raw mouse counts while the pointer is captured (the window thread). Taken counts never turn the
     * game's camera. */
    int (*motion)(long dx, long dy);
} PcInputFilter;
void pc_input_set_filter(const PcInputFilter *filter);
/* Priority: slot 2 (native movies), slot 3 (photo mode), slot 0 (Video Settings), slot 1 (native menus), slot 4
 * (split-screen's second player). */
void pc_input_set_filter_slot(int slot, const PcInputFilter *filter);
/* A module that wants the pointer captured (hidden, confined, raw motion) outside gameplay: photo mode. */
void pc_input_set_capture_query(int (*query)(void));

/* ---- host events (presenter/UI thread) ------------------------------------------------------ */

void pc_input_key_event(unsigned scancode, int extended, int down, int repeat);
void pc_input_mouse_button_event(int button /* 0..4 */, int down);
void pc_input_wheel_event(int delta);
void pc_input_mouse_motion(long dx, long dy);
/* Focus lost/regained or window minimised/closed: every held input, latch and delta is cleared
 * and, if a mission is being played, the game is paused once. */
void pc_input_focus_event(int focused);

/* The UI thread asks; it applies the answers to the real cursor. */
int pc_input_capture_wanted(void);     /* confine + hide the pointer now */
int pc_input_cursor_hidden_wanted(void);
void pc_input_capture_changed(int captured); /* told after the UI thread acted; clears stale state */
void pc_input_set_movie_active_query(int (*movie_active)(void));
void pc_input_set_gameplay_blocked_query(int (*blocked)(void));
/* One validated native completed frame, independent of UI presentation.
 * An unblocked frame without a camera tick suspends gameplay controls but keeps
 * physically held movement, Fire and Hold-mode Aim bindings. Within 400 ms of focused camera
 * evidence, those holds still report gameplay controls without entering the
 * menu layer or retaining released taps. Other pending taps/action holds are
 * discarded; pause/movie/focus loss discards movement and Fire holds too. */
void pc_input_frame_complete(void);

/* ---- the pad report (OHCI thread) ------------------------------------------------------------- */

/* `out` holds the 20-byte report built from the host pad (or neutral when `pad_present` is 0).
 * Applies the input mode, merges the keyboard and mouse buttons, and tracks pad activity. */
void pc_input_apply_report(uint8_t out[20], int pad_present);
/* The primary native decoder brackets its real sample with an opaque token.
 * Only presses present before begin and observed in the 24 native down booleans
 * are acknowledged. Snapshots retain released holds without replaying taps;
 * a completion older than four sample starts is safely ignored. */
uint64_t pc_input_native_sample_begin(void);
void pc_input_native_sample_end(uint64_t token, uint32_t observed_controls);

/* ---- camera (guest thread) ---------------------------------------------------------------------- */

/* The guest's camera update ran: the player is in control. */
void pc_input_gameplay_tick(void);
int pc_input_gameplay_active(void);
/* Guest thread: host-only actions; jump is one fresh press, including a short tap.
 * Menu/movie/focus transitions discard requests. These never emit a guest pad control. */
void pc_input_movement(int *sprint_held, int *jump_pressed);
/* Takes every count accumulated since the last call. Returns 0 when there is nothing (or mouse
 * look is off). Otherwise *yaw/*pitch are degrees to add in BLACK's own convention: yaw grows
 * to the left, pitch grows downward; already scaled by sensitivity and Invert Y. */
int pc_input_consume_look(double *yaw_degrees, double *pitch_degrees);

/* ---- prompts --------------------------------------------------------------------------------- */

PcDevice pc_input_active_device(void);
PcDevice pc_input_prompt_device(void);      /* after applying RECOMP_BLACK_PROMPT_MODE */

/* ASCII text for a slot's binding, e.g. "PRESS R", "CLICK LMB", "SCROLL UP"; 0 if the slot is
 * not a bindable action. */
int pc_input_prompt_text(int slot, char *out, size_t cap);
/* The universal confirm key ("[ENTER]"). */
int pc_input_confirm_text(char *out, size_t cap);

/* The fixed menu keys the game's menu legends and "press ..." lines name: what a person presses
 * where the pad would press A, B, X or Y. */
typedef enum PcMenuKey { PC_MENU_CONFIRM, PC_MENU_BACK, PC_MENU_X, PC_MENU_Y } PcMenuKey;
int pc_input_menu_text(PcMenuKey key, char *out, size_t cap);

/* Replaces tokens U+F001..U+F025 in a UTF-16 buffer (NUL terminated, capacity `cap` code units
 * including the NUL) with keyboard/mouse text. The string is never longer than cap-1 afterwards;
 * a token whose text does not fit, or that has no keyboard meaning, is left alone for the game's
 * own formatter. With `confirm_only` every token becomes the confirm key. Returns the number of
 * tokens replaced. */
int pc_input_expand_prompt(uint16_t *buf, size_t cap, int confirm_only);

/* ---- bindings at run time ----------------------------------------------------------------------------- */

/* A source is a scancode (0x100 set for an extended key) or one of these. */
#define PC_SOURCE_MOUSE(button) (0x300u + (unsigned)(button))   /* 0 left .. 4 */
#define PC_SOURCE_WHEEL_UP      0x310u
#define PC_SOURCE_WHEEL_DOWN    0x311u

/* Any thread. Bumped by every change below. */
unsigned pc_input_bindings_generation(void);
/* Whether a source can be bound (Escape and F11 belong to the host). */
int pc_input_source_bindable(unsigned source);
/* The label a person reads for a source ("R" in their layout, "LMB", "WHEEL UP"). */
int pc_input_source_label(unsigned source, char *out, size_t cap);
/* The source at `index` (0 primary, 1 secondary ...) of an action, 0 for none. */
unsigned pc_input_binding(PcAction action, int index);
/* Game thread. Binds `source` at `index`, or clears that binding when `source` is 0 (later ones move up). A source
 * leaves any other action it was bound to, so one key does one thing. Returns 0 for a source that cannot be bound. */
int pc_input_bind(PcAction action, int index, unsigned source);
void pc_input_bindings_reset(void);
/* An action's bindings in the RECOMP_BLACK_BIND_<ACTION> form ("LCtrl,C", "None"), and back. */
size_t pc_input_binding_names(PcAction action, char *out, size_t cap);
int pc_input_set_binding_names(PcAction action, const char *value);

/* ---- tests ----------------------------------------------------------------------------------------- */

/* Re-reads the environment and clears all state (tests only; not thread safe). */
void pc_input_test_reset(void);
void pc_input_test_set_time(uint64_t now_ms); /* 0 = real clock */
void pc_input_test_set_layout_labels(int layout_aware);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_PC_INPUT_H */
