/*
 * pc_input.c -- native keyboard and mouse for BLACK. See pc_input.h for the design.
 *
 * Contents: configuration and bindings; key names; shared state; host events; the pad report;
 * camera; prompts; scripted events for tests.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "pc_input.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#error "pc_input is Windows only (raw input, scancodes)"
#endif

/* ---- constants ----------------------------------------------------------------------------- */

/* Degrees of turn per raw mouse count at sensitivity 1.0. */
#define PC_DEGREES_PER_COUNT 0.06
/* BLACK's conventions, found in the camera update (sub_001BD3F0): yaw grows to the left and
 * pitch grows downward. The signs are multiplied in here so one constant can correct a finding. */
#define PC_YAW_SIGN (-1.0)
#define PC_PITCH_SIGN (1.0)

#define PC_SENSITIVITY_MIN 0.1
#define PC_SENSITIVITY_MAX 10.0
#define PC_LOOK_CLAMP 20000L            /* counts taken per camera update, at most */

/* A press is held at least this long so a tap shorter than the OHCI poll (20 ms) plus one game
 * frame (33 ms at the native 30 Hz) is never lost. */
#define PC_LATCH_MS 70
#define PC_WHEEL_MS 110
#define PC_PULSE_MS 150
#define PC_GAMEPLAY_MS 400              /* startup fallback and evidence for a new movement hold in a frame gap */
#define PC_LOOK_GAP_MS 150              /* camera-update gap that keeps the pointer and mouse motion (see below) */
#define PC_PENDING_MS 5000             /* safety expiry for a stalled/disconnected native consumer */
#define PC_PENDING_MAX 4               /* bounded distinct presses awaiting native samples */
#define PC_NATIVE_SAMPLE_MAX 4         /* bounded snapshots; stale completions cannot acknowledge new input */
#define PC_SWALLOW_MS 250               /* mouse presses right after (re)focus are not the game's */

/* BLACK's 28 controls (names table at 0x2AC290). */
enum {
    CTL_DPAD_UP = 0, CTL_DPAD_DOWN, CTL_DPAD_LEFT, CTL_DPAD_RIGHT,
    CTL_LS_UP, CTL_LS_DOWN, CTL_LS_LEFT, CTL_LS_RIGHT,
    CTL_RS_UP, CTL_RS_DOWN, CTL_RS_LEFT, CTL_RS_RIGHT,
    CTL_START, CTL_BACK, CTL_LTHUMB, CTL_RTHUMB,
    CTL_A, CTL_B, CTL_X, CTL_Y, CTL_BLACK, CTL_WHITE, CTL_LT, CTL_RT,
    CTL_COUNT_BUTTONS
};

/* ---- sources: everything that can be held ---------------------------------------------------- */

#define SRC_KEY_LIMIT 0x200        /* scancode | 0x100 when extended */
#define SRC_ANY_SHIFT 0x200
#define SRC_ANY_CTRL  0x201
#define SRC_ANY_ALT   0x202
#define SRC_MOUSE0    0x300        /* + button 0..4 */
#define SRC_WHEEL_UP  0x310
#define SRC_WHEEL_DOWN 0x311
#define SRC_COUNT     0x320

#define KEY_ESC        0x001
#define KEY_BACKSPACE  0x00E
#define KEY_ENTER      0x01C
#define KEY_SPACE      0x039
#define KEY_NUMENTER   0x11C
#define KEY_UP         0x148
#define KEY_LEFT       0x14B
#define KEY_RIGHT      0x14D
#define KEY_DOWN       0x150
#define KEY_LSHIFT     0x02A
#define KEY_RSHIFT     0x036
#define KEY_LCTRL      0x01D
#define KEY_RCTRL      0x11D
#define KEY_LALT       0x038
#define KEY_RALT       0x138
#define KEY_F11        0x057
#define KEY_X          0x02D
#define KEY_Y          0x015

typedef struct PcBinding { uint16_t src; } PcBinding;
typedef struct PcBindingList { int count; PcBinding b[PC_MAX_BINDINGS]; } PcBindingList;

/* Names are case-insensitive QWERTY key names (physical positions, as scancodes). */
static const struct KeyName { const char *name; unsigned id; const char *label; } k_keys[] = {
    {"A",0x1E,"A"},{"B",0x30,"B"},{"C",0x2E,"C"},{"D",0x20,"D"},{"E",0x12,"E"},{"F",0x21,"F"},
    {"G",0x22,"G"},{"H",0x23,"H"},{"I",0x17,"I"},{"J",0x24,"J"},{"K",0x25,"K"},{"L",0x26,"L"},
    {"M",0x32,"M"},{"N",0x31,"N"},{"O",0x18,"O"},{"P",0x19,"P"},{"Q",0x10,"Q"},{"R",0x13,"R"},
    {"S",0x1F,"S"},{"T",0x14,"T"},{"U",0x16,"U"},{"V",0x2F,"V"},{"W",0x11,"W"},{"X",0x2D,"X"},
    {"Y",0x15,"Y"},{"Z",0x2C,"Z"},
    {"1",0x02,"1"},{"2",0x03,"2"},{"3",0x04,"3"},{"4",0x05,"4"},{"5",0x06,"5"},{"6",0x07,"6"},
    {"7",0x08,"7"},{"8",0x09,"8"},{"9",0x0A,"9"},{"0",0x0B,"0"},
    {"F1",0x3B,"F1"},{"F2",0x3C,"F2"},{"F3",0x3D,"F3"},{"F4",0x3E,"F4"},{"F5",0x3F,"F5"},
    {"F6",0x40,"F6"},{"F7",0x41,"F7"},{"F8",0x42,"F8"},{"F9",0x43,"F9"},{"F10",0x44,"F10"},
    {"F12",0x58,"F12"},
    {"Space",KEY_SPACE,"SPACE"},{"Enter",KEY_ENTER,"ENTER"},{"Tab",0x0F,"TAB"},
    {"Backspace",KEY_BACKSPACE,"BACKSPACE"},{"CapsLock",0x3A,"CAPS LOCK"},
    {"Up",KEY_UP,"UP ARROW"},{"Down",KEY_DOWN,"DOWN ARROW"},{"Left",KEY_LEFT,"LEFT ARROW"},
    {"Right",KEY_RIGHT,"RIGHT ARROW"},
    {"Insert",0x152,"INSERT"},{"Delete",0x153,"DELETE"},{"Home",0x147,"HOME"},{"End",0x14F,"END"},
    {"PageUp",0x149,"PAGE UP"},{"PageDown",0x151,"PAGE DOWN"},
    {"Shift",SRC_ANY_SHIFT,"SHIFT"},{"LShift",KEY_LSHIFT,"LEFT SHIFT"},{"RShift",KEY_RSHIFT,"RIGHT SHIFT"},
    {"Ctrl",SRC_ANY_CTRL,"CTRL"},{"LCtrl",KEY_LCTRL,"LEFT CTRL"},{"RCtrl",KEY_RCTRL,"RIGHT CTRL"},
    {"Alt",SRC_ANY_ALT,"ALT"},{"LAlt",KEY_LALT,"LEFT ALT"},{"RAlt",KEY_RALT,"RIGHT ALT"},
    {"Numpad0",0x52,"NUM 0"},{"Numpad1",0x4F,"NUM 1"},{"Numpad2",0x50,"NUM 2"},{"Numpad3",0x51,"NUM 3"},
    {"Numpad4",0x4B,"NUM 4"},{"Numpad5",0x4C,"NUM 5"},{"Numpad6",0x4D,"NUM 6"},{"Numpad7",0x47,"NUM 7"},
    {"Numpad8",0x48,"NUM 8"},{"Numpad9",0x49,"NUM 9"},{"NumpadEnter",KEY_NUMENTER,"NUM ENTER"},
    {"NumpadPlus",0x4E,"NUM +"},{"NumpadMinus",0x4A,"NUM -"},{"NumpadMultiply",0x37,"NUM *"},
    {"NumpadDivide",0x135,"NUM /"},{"NumpadDecimal",0x53,"NUM ."},
    {"Minus",0x0C,"-"},{"Equals",0x0D,"="},{"LBracket",0x1A,"["},{"RBracket",0x1B,"]"},
    {"Semicolon",0x27,";"},{"Quote",0x28,"'"},{"Grave",0x29,"`"},{"Comma",0x33,","},
    {"Period",0x34,"."},{"Slash",0x35,"/"},{"Backslash",0x2B,"\\"},
    /* Mouse */
    {"Mouse1",SRC_MOUSE0 + 0,"LMB"},{"Mouse2",SRC_MOUSE0 + 1,"RMB"},{"Mouse3",SRC_MOUSE0 + 2,"MMB"},
    {"Mouse4",SRC_MOUSE0 + 3,"MOUSE 4"},{"Mouse5",SRC_MOUSE0 + 4,"MOUSE 5"},
    {"WheelUp",SRC_WHEEL_UP,"WHEEL UP"},{"WheelDown",SRC_WHEEL_DOWN,"WHEEL DOWN"},
};
#define KEY_NAME_COUNT ((int)(sizeof(k_keys) / sizeof(k_keys[0])))

/* Escape (pause / back) and F11 (window mode) belong to the host. */
static int key_name_reserved(const char *t)
{
    return !_stricmp(t, "Esc") || !_stricmp(t, "Escape") || !_stricmp(t, "F11");
}

/* ---- the actions --------------------------------------------------------------------------- */

const PcActionInfo pc_input_actions[PC_ACTION_COUNT] = {
    { PC_ACTION_MOVE_FORWARD, "MOVE_FORWARD", "Move forward",       1, "W" },
    { PC_ACTION_MOVE_BACK,    "MOVE_BACK",    "Move back",          2, "S" },
    { PC_ACTION_MOVE_LEFT,    "MOVE_LEFT",    "Move left",          3, "A" },
    { PC_ACTION_MOVE_RIGHT,   "MOVE_RIGHT",   "Move right",         4, "D" },
    { PC_ACTION_LOOK_LEFT,    "LOOK_LEFT",    "Look left (keys)",   5, "None" },
    { PC_ACTION_LOOK_RIGHT,   "LOOK_RIGHT",   "Look right (keys)",  6, "None" },
    { PC_ACTION_LOOK_UP,      "LOOK_UP",      "Look up (keys)",     7, "None" },
    { PC_ACTION_LOOK_DOWN,    "LOOK_DOWN",    "Look down (keys)",   8, "None" },
    { PC_ACTION_FIRE,         "FIRE",         "Fire",               9, "Mouse1" },
    { PC_ACTION_AIM,          "AIM",          "Aim / zoom",        16, "Mouse2" },
    { PC_ACTION_RELOAD,       "RELOAD",       "Reload",            11, "R" },
    { PC_ACTION_CROUCH,       "CROUCH",       "Crouch",            12, "LCtrl,C" },
    { PC_ACTION_USE,          "USE",          "Use / pick up",     13, "E" },
    { PC_ACTION_NEXT_WEAPON,  "NEXT_WEAPON",  "Switch weapon",     14, "Q,WheelUp" },
    { PC_ACTION_PREV_WEAPON,  "PREV_WEAPON",  "Switch weapon (other way)", 15, "WheelDown" },
    { PC_ACTION_GRENADE,      "GRENADE",      "Throw grenade",     27, "G" },
    { PC_ACTION_FIRE_MODE,    "FIRE_MODE",    "Fire mode",         30, "B" },
    { PC_ACTION_SUPPRESSOR,   "SUPPRESSOR",   "Suppressor",        31, "F" },
    { PC_ACTION_MELEE,        "MELEE",        "Melee attack",      32, "V,Mouse3" },
    { PC_ACTION_HEALTH_PACK,  "HEALTH_PACK",  "Use health pack",   33, "H" },
    { PC_ACTION_CYCLE_ITEM,   "CYCLE_ITEM",   "Cycle item (White)", 34, "X" },
    { PC_ACTION_BACK,         "BACK",         "Back / objectives", 36, "Tab" },
    { PC_ACTION_PAUSE,        "PAUSE",        "Start (pause)",     25, "P" },
    { PC_ACTION_SPRINT, "SPRINT", "Sprint", -1, "LShift" },
    { PC_ACTION_JUMP, "JUMP", "Jump", -1, "Space" },
};


/* ---- state -------------------------------------------------------------------------------------- */

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static PcMode s_input_mode, s_prompt_mode;
static double s_sensitivity = 1.0;
static int s_invert_y;
static int s_trace;
static int s_layout_labels = 1;
static PcBindingList s_bindings[PC_ACTION_COUNT];
static PcControlResolver s_resolver;
static int (*s_movie_active)(void);

static volatile LONG s_down[SRC_COUNT];
static volatile LONG64 s_latch[SRC_COUNT];          /* held at least until this time */
static volatile LONG64 s_pulse[CTL_COUNT_BUTTONS];   /* control forced on until this time */
static volatile LONG s_dx, s_dy;
static volatile LONG s_focused = 1, s_captured;
/* Movies own input ahead of the Video Settings page and native menus. */
/* Slots: 0 Video Settings, 1 native menus, 2 native movies, 3 photo mode (ahead of the menus it is drawn with). */
#define PC_FILTER_SLOTS 5
static const PcInputFilter *volatile s_filters[PC_FILTER_SLOTS];
static const unsigned s_filter_order[PC_FILTER_SLOTS] = { 2, 3, 0, 1, 4 };
static int (*s_capture_query)(void);

static int filter_key(unsigned id, int down, int repeat)
{
    int i;
    for (i = 0; i < PC_FILTER_SLOTS; i++) {
        const PcInputFilter *f = s_filters[s_filter_order[i]];
        if (f && f->key && f->key(id, down, repeat)) return 1;
    }
    return 0;
}

static int filter_button(int button, int down)
{
    int i;
    for (i = 0; i < PC_FILTER_SLOTS; i++) {
        const PcInputFilter *f = s_filters[s_filter_order[i]];
        if (f && f->button && f->button(button, down)) return 1;
    }
    return 0;
}

static int filter_wheel(int delta)
{
    int i;
    for (i = 0; i < PC_FILTER_SLOTS; i++) {
        const PcInputFilter *f = s_filters[s_filter_order[i]];
        if (f && f->wheel && f->wheel(delta)) return 1;
    }
    return 0;
}

static int filter_motion(long dx, long dy)
{
    int i;
    for (i = 0; i < PC_FILTER_SLOTS; i++) {
        const PcInputFilter *f = s_filters[s_filter_order[i]];
        if (f && f->motion && f->motion(dx, dy)) return 1;
    }
    return 0;
}

static int filter_report(uint8_t out[20], int pad_present)
{
    int i;
    for (i = 0; i < PC_FILTER_SLOTS; i++) {
        const PcInputFilter *f = s_filters[s_filter_order[i]];
        if (f && f->report && f->report(out, pad_present)) return 1;
    }
    return 0;
}
static volatile LONG s_swallow_mask;                 /* mouse buttons whose current press is ignored */
static volatile LONG64 s_mouse_ignore_until;
static volatile LONG64 s_gameplay_tick;
static volatile LONG s_frame_authority, s_gameplay_seen, s_gameplay_active, s_focus_pause_guard;
static int (*s_gameplay_blocked_query)(void);
typedef struct PcPending { uint64_t generation, until; int context; } PcPending;
static SRWLOCK s_pending_lock = SRWLOCK_INIT;
static PcPending s_pending[SRC_COUNT][PC_PENDING_MAX], s_pending_pulse[CTL_COUNT_BUTTONS][PC_PENDING_MAX];
static unsigned char s_pending_count[SRC_COUNT], s_pending_pulse_count[CTL_COUNT_BUTTONS];
static unsigned char s_source_context[SRC_COUNT];
static uint32_t s_source_ack[SRC_COUNT];
static uint64_t s_source_generation[SRC_COUNT];
typedef struct PcNativeSample {
    uint64_t token, press_generation;
    uint64_t held_generation[SRC_COUNT];
    uint32_t held_controls[SRC_COUNT];
} PcNativeSample;
static PcNativeSample s_native_samples[PC_NATIVE_SAMPLE_MAX];
static uint64_t s_native_sample_generation;
static volatile LONG64 s_press_generation;
static volatile LONG s_native_authority, s_neutral_barrier;
static volatile LONG s_active_device = -1;           /* PcDevice, or -1 until something has happened */
static volatile LONG s_pad_streak;
static uint64_t s_test_now;
static uint64_t s_trace_t0;
static int s_trace_level;
static uint32_t s_trace_last_bits = 0xFFFFFFFFu;

/* scripted events (tests) */
typedef struct ScriptEvent { uint64_t at_ms; char verb; unsigned arg; long a, b; } ScriptEvent;
#define SCRIPT_MAX 256
static ScriptEvent s_script[SCRIPT_MAX];
static int s_script_n, s_script_next;
static uint64_t s_script_t0;
/* Explicit laboratory isolation; a malformed or absent script cannot enable it. */
static int s_script_background;

static uint64_t now_ms(void)
{
    LARGE_INTEGER f, c;
    if (s_test_now) return s_test_now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000 / f.QuadPart) + 1;
}

static void trace(const char *fmt, ...)
{
    va_list ap;
    if (!s_trace) return;
    fprintf(stderr, "[INPUT +%llu ms] ", (unsigned long long)(now_ms() - s_trace_t0));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- configuration ---------------------------------------------------------------------------- */

static const struct KeyName *key_by_name(const char *t)
{
    int i;
    for (i = 0; i < KEY_NAME_COUNT; i++)
        if (!_stricmp(k_keys[i].name, t)) return &k_keys[i];
    return NULL;
}

static const struct KeyName *key_by_id(unsigned id)
{
    int i;
    for (i = 0; i < KEY_NAME_COUNT; i++)
        if (k_keys[i].id == id) return &k_keys[i];
    return NULL;
}

static PcMode parse_mode(const char *name, const char *value, PcMode fallback)
{
    if (!value || !*value) return fallback;
    if (!_stricmp(value, "auto")) return PC_MODE_AUTO;
    if (!_stricmp(value, "keyboard_mouse")) return PC_MODE_KEYBOARD_MOUSE;
    if (!_stricmp(value, "controller")) return PC_MODE_CONTROLLER;
    fprintf(stderr, "[INPUT] %s=%s is not auto, keyboard_mouse or controller; using auto\n", name, value);
    return fallback;
}

/* Parses "W,Mouse1,None". Unknown or reserved names are reported and skipped; if nothing valid
 * is left the caller keeps its default. Returns -1 for "no valid token at all". */
static int parse_bindings(const char *action, const char *value, PcBindingList *out)
{
    char buf[128], *tok, *ctx = NULL;
    int valid = 0, explicit_none = 0;

    out->count = 0;
    if (!value) return -1;
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        const struct KeyName *k;
        char *end;
        while (*tok == ' ') tok++;
        end = tok + strlen(tok);
        while (end > tok && end[-1] == ' ') *--end = 0;
        if (!*tok) continue;
        if (!_stricmp(tok, "None")) { explicit_none = 1; continue; }
        if (key_name_reserved(tok)) {
            fprintf(stderr, "[INPUT] RECOMP_BLACK_BIND_%s: '%s' is reserved (Escape pauses, F11 toggles the window)\n", action, tok);
            continue;
        }
        k = key_by_name(tok);
        if (!k) {
            fprintf(stderr, "[INPUT] RECOMP_BLACK_BIND_%s: unknown key '%s' ignored\n", action, tok);
            continue;
        }
        if (out->count < PC_MAX_BINDINGS) out->b[out->count++].src = (uint16_t)k->id;
        valid++;
    }
    if (valid || explicit_none) return valid;
    return -1;
}

static void load_config(void)
{
    char name[96];
    const char *v;
    int a;

    s_input_mode = parse_mode("RECOMP_BLACK_INPUT_MODE", getenv("RECOMP_BLACK_INPUT_MODE"), PC_MODE_AUTO);
    s_prompt_mode = parse_mode("RECOMP_BLACK_PROMPT_MODE", getenv("RECOMP_BLACK_PROMPT_MODE"), PC_MODE_AUTO);
    s_sensitivity = 1.0;
    v = getenv("RECOMP_BLACK_MOUSE_SENSITIVITY");
    if (v && *v) {
        char *end;
        double d = strtod(v, &end);
        if (end != v && !*end && isfinite(d) && d >= PC_SENSITIVITY_MIN && d <= PC_SENSITIVITY_MAX)
            s_sensitivity = d;
        else
            fprintf(stderr, "[INPUT] RECOMP_BLACK_MOUSE_SENSITIVITY=%s is not a number from %.1f to %.0f; using 1\n",
                    v, PC_SENSITIVITY_MIN, PC_SENSITIVITY_MAX);
    }
    v = getenv("RECOMP_BLACK_MOUSE_INVERT_Y");
    s_invert_y = v && (v[0] == '1' || !_stricmp(v, "true"));
    s_trace = getenv("RECOMP_INPUT_TRACE") != NULL;
    s_trace_level = s_trace ? atoi(getenv("RECOMP_INPUT_TRACE")) : 0;
    s_trace_t0 = now_ms();

    for (a = 0; a < PC_ACTION_COUNT; a++) {
        PcBindingList def, got;
        parse_bindings(pc_input_actions[a].name, pc_input_actions[a].default_binding, &def);
        snprintf(name, sizeof(name), "RECOMP_BLACK_BIND_%s", pc_input_actions[a].name);
        v = getenv(name);
        s_bindings[a] = def;
        if (v) {
            int r = parse_bindings(pc_input_actions[a].name, v, &got);
            if (r >= 0) s_bindings[a] = got;
            else fprintf(stderr, "[INPUT] %s=%s has no usable key; keeping the default\n", name, v);
        }
    }
}

static const char *mode_name(PcMode m)
{
    return m == PC_MODE_KEYBOARD_MOUSE ? "keyboard_mouse" : m == PC_MODE_CONTROLLER ? "controller" : "auto";
}

/* ---- bindings at run time (the Controls pages) ----------------------------------------------------- */

/* Changes come from the game thread while the input threads read the lists. A list only ever holds valid
 * sources, its count is written last when it grows and first when it shrinks, so a reader that races a change
 * sees the old binding or the new one for a single poll, never a source that was not bound. */
static volatile LONG s_bindings_generation;

static void binding_store(PcBindingList *list, const PcBindingList *want)
{
    int i;
    if (want->count < list->count) { list->count = want->count; MemoryBarrier(); }
    for (i = 0; i < PC_MAX_BINDINGS; i++) list->b[i] = want->b[i];
    MemoryBarrier();
    list->count = want->count;
    InterlockedIncrement(&s_bindings_generation);
}

unsigned pc_input_bindings_generation(void) { return (unsigned)InterlockedCompareExchange(&s_bindings_generation, 0, 0); }

int pc_input_source_bindable(unsigned source)
{
    if (source == KEY_ESC || source == KEY_F11 || source == SRC_ANY_SHIFT || source == SRC_ANY_CTRL || source == SRC_ANY_ALT)
        return 0;
    return key_by_id(source) != NULL;
}

unsigned pc_input_binding(PcAction action, int index)
{
    pc_input_init();
    if ((unsigned)action >= PC_ACTION_COUNT || index < 0 || index >= s_bindings[action].count) return 0;
    return s_bindings[action].b[index].src;
}

int pc_input_bind(PcAction action, int index, unsigned source)
{
    PcBindingList want;
    int a, i;
    pc_input_init();
    if ((unsigned)action >= PC_ACTION_COUNT || index < 0 || index >= PC_MAX_BINDINGS) return 0;
    if (source && !pc_input_source_bindable(source)) return 0;
    if (source) {
        /* one key does one thing: it leaves whatever it was bound to before */
        for (a = 0; a < PC_ACTION_COUNT; a++) {
            int k = 0;
            want.count = 0;
            for (i = 0; i < s_bindings[a].count; i++)
                if (s_bindings[a].b[i].src != source || (a == (int)action && i == index)) want.b[k++] = s_bindings[a].b[i];
            if (k == s_bindings[a].count) continue;
            for (i = k; i < PC_MAX_BINDINGS; i++) want.b[i].src = 0;
            want.count = k;
            if (a == (int)action && index > k) index = k;
            binding_store(&s_bindings[a], &want);
        }
    }
    want = s_bindings[action];
    if (!source) {
        if (index >= want.count) return 1;
        for (i = index; i + 1 < want.count; i++) want.b[i] = want.b[i + 1];
        want.b[--want.count].src = 0;
    } else if (index < want.count) {
        want.b[index].src = (uint16_t)source;
    } else {
        want.b[want.count++].src = (uint16_t)source;    /* a later column of an empty list fills the first free one */
    }
    binding_store(&s_bindings[action], &want);
    trace("bind %s[%d] = %s", pc_input_actions[action].name, index, source ? key_by_id(source)->name : "none");
    return 1;
}

void pc_input_bindings_reset(void)
{
    int a;
    pc_input_init();
    for (a = 0; a < PC_ACTION_COUNT; a++) {
        PcBindingList def;
        memset(&def, 0, sizeof(def));
        parse_bindings(pc_input_actions[a].name, pc_input_actions[a].default_binding, &def);
        binding_store(&s_bindings[a], &def);
    }
}

size_t pc_input_binding_names(PcAction action, char *out, size_t cap)
{
    size_t n = 0;
    int i;
    pc_input_init();
    if (!out || !cap) return 0;
    out[0] = 0;
    if ((unsigned)action >= PC_ACTION_COUNT) return 0;
    for (i = 0; i < s_bindings[action].count; i++) {
        const struct KeyName *k = key_by_id(s_bindings[action].b[i].src);
        int w;
        if (!k) continue;
        w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", k->name);
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
    if (!n) n = (size_t)snprintf(out, cap, "None");
    return n < cap ? n : cap - 1;
}

int pc_input_set_binding_names(PcAction action, const char *value)
{
    PcBindingList got;
    pc_input_init();
    if ((unsigned)action >= PC_ACTION_COUNT) return 0;
    memset(&got, 0, sizeof(got));
    if (parse_bindings(pc_input_actions[action].name, value, &got) < 0) return 0;
    binding_store(&s_bindings[action], &got);
    return 1;
}

static void load_script(void);

static uint64_t s_jump_seen[SRC_COUNT];

static void clear_state(int keep_focus_pause)
{
    int i;
    AcquireSRWLockExclusive(&s_pending_lock);
    for (i = 0; i < SRC_COUNT; i++) { s_down[i] = 0; s_latch[i] = 0; }
    for (i = 0; i < CTL_COUNT_BUTTONS; i++) {
        if (keep_focus_pause && i == CTL_START) continue;
        s_pulse[i] = 0; s_pending_pulse_count[i] = 0;
    }
    memset(s_pending_count, 0, sizeof(s_pending_count));
    memset(s_source_ack, 0, sizeof(s_source_ack));
    memset(s_source_generation, 0, sizeof(s_source_generation));
    memset(s_jump_seen, 0, sizeof(s_jump_seen));
    memset(s_native_samples, 0, sizeof(s_native_samples));
    s_neutral_barrier &= keep_focus_pause ? 1L << CTL_START : 0;
    s_dx = s_dy = 0;
    s_swallow_mask = 0;
    ReleaseSRWLockExclusive(&s_pending_lock);
}

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    load_config();
    load_script();
    fprintf(stderr, "[INPUT] native keyboard/mouse: input %s, prompts %s, sensitivity %.2f, invert Y %d\n",
            mode_name(s_input_mode), mode_name(s_prompt_mode), s_sensitivity, s_invert_y);
    return TRUE;
}

void pc_input_init(void)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
}

PcMode pc_input_input_mode(void) { pc_input_init(); return s_input_mode; }
PcMode pc_input_prompt_mode(void) { pc_input_init(); return s_prompt_mode; }

int pc_input_trace_enabled(void) { pc_input_init(); return s_trace; }

static volatile LONG s_aim_hold_mode;
void pc_input_set_aim_hold_mode(int enabled) { InterlockedExchange(&s_aim_hold_mode, enabled != 0); }
void pc_input_set_control_resolver(PcControlResolver resolver) { s_resolver = resolver; }
void pc_input_set_movie_active_query(int (*q)(void)) { s_movie_active = q; }
void pc_input_set_gameplay_blocked_query(int (*q)(void)) { s_gameplay_blocked_query = q; }

static int kbm_enabled(void) { return s_input_mode != PC_MODE_CONTROLLER; }
static int movie_active(void) { return s_movie_active && s_movie_active(); }

static int resolve_slot(int slot)
{
    int c = -1;
    if (slot < 0 || slot > 36) return -1;
    if (s_resolver) c = s_resolver(slot);
    if (c == -2) c = -1;   /* The game resolver reads its fallback from the user's loaded XBE. */
    return c;
}

/* ---- sources ------------------------------------------------------------------------------------- */

static int gameplay_hold_gap_context(unsigned source, uint64_t now);

static int src_active_raw(unsigned id, uint64_t now, int gameplay)
{
    int active;
    const int hold_gap = !gameplay && gameplay_hold_gap_context(id, now);
    if (id >= SRC_COUNT) return 0;
    if (!s_native_authority && !s_frame_authority) return s_down[id] || (uint64_t)s_latch[id] > now;
    AcquireSRWLockShared(&s_pending_lock);
    active = s_down[id] && (s_source_context[id] == 2 || s_source_context[id] == gameplay ||
                            (s_source_context[id] == 1 && hold_gap));
    if (!active && !s_native_authority)
        active = (uint64_t)s_latch[id] > now &&
                 (s_source_context[id] == 2 || s_source_context[id] == gameplay ||
                  (s_source_context[id] == 1 && hold_gap));
    else if (!active && s_pending_count[id]) {
        const PcPending *p = &s_pending[id][0];
        active = p->until > now && (p->context == 2 || p->context == gameplay ||
                                     (p->context == 1 && hold_gap));
    }
    ReleaseSRWLockShared(&s_pending_lock);
    return active;
}

static int src_active(unsigned id, uint64_t now, int gameplay)
{
    if (id == SRC_ANY_SHIFT) return src_active_raw(KEY_LSHIFT, now, gameplay) || src_active_raw(KEY_RSHIFT, now, gameplay);
    if (id == SRC_ANY_CTRL) return src_active_raw(KEY_LCTRL, now, gameplay) || src_active_raw(KEY_RCTRL, now, gameplay);
    if (id == SRC_ANY_ALT) return src_active_raw(KEY_LALT, now, gameplay) || src_active_raw(KEY_RALT, now, gameplay);
    return src_active_raw(id, now, gameplay);
}

/* A hold that began in gameplay may remain active through a short camera/frame gap.
 * It must not become a menu click just because gameplay is temporarily inactive. */
static int src_active_menu(unsigned id, uint64_t now)
{
    int active = 0;
    if (id >= SRC_COUNT) return 0;
    AcquireSRWLockShared(&s_pending_lock);
    if (s_source_context[id] == 0) {
        active = s_down[id] != 0;
        if (!active && !s_native_authority)
            active = (uint64_t)s_latch[id] > now;
        else if (!active && s_pending_count[id])
            active = s_pending[id][0].until > now && s_pending[id][0].context == 0;
    }
    ReleaseSRWLockShared(&s_pending_lock);
    return active;
}

/* Caller holds s_pending_lock. A second tap waits for an observed neutral sample,
 * so it can create a distinct native rising edge rather than extending the first. */
static uint64_t pending_push(PcPending *queue, unsigned char *count, uint64_t now, int context)
{
    const uint64_t generation = (uint64_t)InterlockedIncrement64(&s_press_generation);
    if (*count == PC_PENDING_MAX) return generation;
    queue[*count].generation = generation;
    queue[*count].until = now + PC_PENDING_MS;
    queue[*count].context = context;
    ++*count;
    return generation;
}

static void set_src(unsigned id, int down, uint64_t now, int latch_ms)
{
    int provisional = 0;
    if (id >= SRC_COUNT) return;
    AcquireSRWLockExclusive(&s_pending_lock);
    if (down && !s_down[id]) {
        int context = id == KEY_ENTER || id == KEY_NUMENTER ? 2 : pc_input_gameplay_active();
        if (!context && gameplay_hold_gap_context(id, now)) { context = 1; provisional = 1; }
        s_source_context[id] = (unsigned char)context;
        s_source_ack[id] = 0;
        /* A tentative gap press is only a physical hold. If released before
         * gameplay returns, it must not become a delayed movement tap. */
        s_source_generation[id] = provisional ? (uint64_t)InterlockedIncrement64(&s_press_generation) :
            pending_push(s_pending[id], &s_pending_count[id], now, context);
    } else if (!down && s_down[id] && s_native_authority && s_source_ack[id]) {
        InterlockedOr(&s_neutral_barrier, (LONG)s_source_ack[id]);
        s_latch[id] = 0;
    }
    InterlockedExchange(&s_down[id], down ? 1 : 0);
    if (down) InterlockedExchange64(&s_latch[id], provisional ? 0 : (LONG64)(now + (uint64_t)latch_ms));
    ReleaseSRWLockExclusive(&s_pending_lock);
}

/* A filter owns the whole press, including repeats/releases of a key the game
 * previously held. Do not let that physical hold or a pending tap resume later. */
static void discard_src(unsigned id)
{
    if (id >= SRC_COUNT) return;
    AcquireSRWLockExclusive(&s_pending_lock);
    if (s_native_authority && s_source_ack[id])
        InterlockedOr(&s_neutral_barrier, (LONG)s_source_ack[id]);
    s_down[id] = 0; s_latch[id] = 0; s_source_ack[id] = 0;
    s_pending_count[id] = 0;
    ReleaseSRWLockExclusive(&s_pending_lock);
}

static void pulse_control(int control, uint64_t now, int ms)
{
    if (control >= 0 && control < CTL_COUNT_BUTTONS) {
        AcquireSRWLockExclusive(&s_pending_lock);
        pending_push(s_pending_pulse[control], &s_pending_pulse_count[control], now, 2);
        InterlockedExchange64(&s_pulse[control], (LONG64)(now + (uint64_t)ms));
        ReleaseSRWLockExclusive(&s_pending_lock);
    }
}

static void mark_device(PcDevice d)
{
    LONG prev = InterlockedExchange(&s_active_device, (LONG)d);
    if (prev != (LONG)d) trace("active device: %s", d == PC_DEVICE_KEYBOARD_MOUSE ? "keyboard/mouse" : "controller");
}

/* ---- gameplay context -------------------------------------------------------------------------- */

static int source_matches(unsigned binding, unsigned source);

/* Held movement, Fire and (in Hold mode) Aim may survive an unblocked frame
 * without a camera update. A source also bound to a discrete action must not replay it. */
static int gameplay_hold_source(unsigned source)
{
    int action, i, hold = 0;
    if (source >= SRC_COUNT) return 0;
    for (action = 0; action < PC_ACTION_COUNT; ++action) {
        for (i = 0; i < s_bindings[action].count; ++i) {
            if (!source_matches(s_bindings[action].b[i].src, source)) continue;
            if (action > PC_ACTION_MOVE_RIGHT && action != PC_ACTION_FIRE &&
                action != PC_ACTION_SPRINT && action != PC_ACTION_JUMP &&
                !(action == PC_ACTION_AIM && s_aim_hold_mode)) return 0;
            hold = 1;
        }
    }
    return hold;
}

/* A new hold can arrive while the asynchronous flip has temporarily suspended
 * gameplay. Recent focused camera evidence lets it wait for a real gameplay
 * tick without turning discrete taps into delayed actions. */
static int gameplay_hold_gap_context(unsigned source, uint64_t now)
{
    const uint64_t tick = (uint64_t)s_gameplay_tick;
    if (!s_frame_authority || (!s_focused && !s_script_background) || s_focus_pause_guard || !tick ||
        now < tick || now - tick > PC_GAMEPLAY_MS || movie_active() ||
        (s_gameplay_blocked_query && s_gameplay_blocked_query())) return 0;
    return gameplay_hold_source(source);
}

/* Below full speed the renderer often completes a frame before the next camera
 * update, which ends gameplay for that frame. For keys and buttons that is the
 * intended boundary, but releasing the pointer there (and discarding the mouse
 * motion gathered meanwhile) made it flash and look choppy, worst on diagonals.
 * The pointer and the motion survive while the last camera update is recent and
 * nothing (pause, movie, focus loss) has ended gameplay for real. */
static int look_gap_recent(uint64_t now)
{
    const uint64_t tick = (uint64_t)s_gameplay_tick;
    if (!s_frame_authority || s_focus_pause_guard || !tick || now < tick || now - tick > PC_LOOK_GAP_MS) return 0;
    if (movie_active() || (s_gameplay_blocked_query && s_gameplay_blocked_query())) return 0;
    return 1;
}

void pc_input_gameplay_tick(void)
{
    uint64_t now = now_ms();
    int was = pc_input_gameplay_active();
    const int look_gap = look_gap_recent(now);
    if (!s_frame_authority && s_focused) s_focus_pause_guard = 0;
    if (!was && !s_focus_pause_guard) {
        /* control has just come back (or started): stale movement must not become a turn.
         * After only a short camera-update gap the motion is current: keep it. */
        if (!look_gap) {
            InterlockedExchange(&s_dx, 0);
            InterlockedExchange(&s_dy, 0);
        }
        trace("gameplay active");
        if (s_trace_level >= 2) {
            char line[512];
            int a, n = 0;
            for (a = 0; a < PC_ACTION_COUNT; a++)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%d", pc_input_actions[a].name, resolve_slot(pc_input_actions[a].slot));
            trace("live action map (name=control):%s", line);
        }
    }
    InterlockedExchange64(&s_gameplay_tick, (LONG64)now);
    InterlockedExchange(&s_gameplay_seen, 1);
    if (!s_focus_pause_guard) InterlockedExchange(&s_gameplay_active, 1);
}

int pc_input_gameplay_active(void)
{
    uint64_t t = (uint64_t)s_gameplay_tick;
    if (s_focus_pause_guard || (s_gameplay_blocked_query && s_gameplay_blocked_query())) return 0;
    if (s_frame_authority) return s_gameplay_active != 0;
    return t != 0 && now_ms() - t <= PC_GAMEPLAY_MS;
}

void pc_input_frame_complete(void)
{
    const int seen = InterlockedExchange(&s_gameplay_seen, 0) != 0;
    const int blocked = movie_active() || (s_gameplay_blocked_query && s_gameplay_blocked_query());
    pc_input_init();
    InterlockedExchange(&s_frame_authority, 1);
    if (blocked) InterlockedExchange64(&s_gameplay_tick, 0);
    if (!seen || blocked) {
        unsigned id;
        InterlockedExchange(&s_gameplay_active, 0);
        if (blocked || !look_gap_recent(now_ms())) {
            InterlockedExchange(&s_dx, 0); InterlockedExchange(&s_dy, 0);
        }
        AcquireSRWLockExclusive(&s_pending_lock);
        for (id = 0; id < SRC_COUNT; ++id) {
            unsigned read, write = 0;
            if (s_source_context[id] == 1) {
                /* Rendering can finish a frame before the next camera update.
                 * Keep real movement and Fire holds across that gap,
                 * context-gated out of menus, until its actual release. A
                 * confirmed pause/movie still discards it. No queued taps or
                 * minimum-duration latches survive either transition. */
                const int keep = !blocked && s_down[id] && gameplay_hold_source(id);
                if (!keep) {
                    if (id >= SRC_MOUSE0 && id < SRC_MOUSE0 + 5 && s_down[id])
                        InterlockedOr(&s_swallow_mask, 1L << (id - SRC_MOUSE0));
                    s_down[id] = 0; s_source_ack[id] = 0;
                }
                s_latch[id] = 0;
            }
            for (read = 0; read < s_pending_count[id]; ++read)
                if (s_pending[id][read].context != 1) s_pending[id][write++] = s_pending[id][read];
            s_pending_count[id] = (unsigned char)write;
        }
        ReleaseSRWLockExclusive(&s_pending_lock);
        /* A completed non-gameplay frame confirms the focus pause. Gaining
         * focus alone cannot recapture the pointer before that confirmation. */
        InterlockedExchange(&s_focus_pause_guard, 0);
    } else if (!s_focus_pause_guard) InterlockedExchange(&s_gameplay_active, 1);
}

/* ---- host events ------------------------------------------------------------------------------------- */

/* A menu may consume a legitimate event, but it still selects the device for AUTO prompts. Keep the same
 * eligibility as the unfiltered host paths; focus is checked by the caller (scripts need no window). */
static void observe_key_device(unsigned id, int down, int repeat)
{
    if (down && !repeat && kbm_enabled() && id != KEY_F11 && (id != KEY_ESC || !movie_active()))
        mark_device(PC_DEVICE_KEYBOARD_MOUSE);
}

static void observe_button_device(int button, int down, uint64_t now)
{
    if (button >= 0 && button < 5 && down && kbm_enabled() && now >= (uint64_t)s_mouse_ignore_until &&
        !(s_swallow_mask & (1L << button))) mark_device(PC_DEVICE_KEYBOARD_MOUSE);
}

static void observe_wheel_device(int delta)
{
    if (delta && kbm_enabled()) mark_device(PC_DEVICE_KEYBOARD_MOUSE);
}

static void key_event_internal(unsigned id, int down, int repeat, uint64_t now)
{
    if (repeat) return;
    if (id == KEY_ESC) {
        if (down && kbm_enabled() && !movie_active()) {
            /* Escape pauses the mission; in a menu it is "back". A movie keeps its own skip. */
            if (pc_input_gameplay_active()) pulse_control(CTL_START, now, PC_PULSE_MS);
            else pulse_control(CTL_B, now, PC_PULSE_MS);
            mark_device(PC_DEVICE_KEYBOARD_MOUSE);
        }
        return;
    }
    if (id == KEY_F11) return;
    if (!kbm_enabled()) return;
    set_src(id, down, now, PC_LATCH_MS);
    if (down) mark_device(PC_DEVICE_KEYBOARD_MOUSE);
}

void pc_input_key_event(unsigned scancode, int extended, int down, int repeat)
{
    pc_input_init();
    if (s_script_background) return;
    if (!s_focused) return;
    if (scancode == 0 || scancode > 0xFF) return;
    {
        const unsigned id = (scancode & 0xFF) | (extended ? 0x100u : 0u);
        observe_key_device(id, down, repeat);
        if (filter_key(id, down, repeat)) { discard_src(id); return; }
    }
    key_event_internal((scancode & 0xFF) | (extended ? 0x100u : 0u), down, repeat, now_ms());
}

static void button_event_internal(int button, int down, uint64_t now)
{
    unsigned id = SRC_MOUSE0 + (unsigned)button;
    if (button < 0 || button > 4 || !kbm_enabled()) return;
    if (down) {
        if (now < (uint64_t)s_mouse_ignore_until) {
            InterlockedOr(&s_swallow_mask, 1L << button);
            trace("mouse button %d down swallowed during focus guard", button + 1);
            return;
        }
        if (s_swallow_mask & (1L << button)) {
            trace("mouse button %d down swallowed until physical release", button + 1);
            return;
        }
        set_src(id, 1, now, PC_LATCH_MS);
        trace("mouse button %d down accepted (gameplay %d)", button + 1, pc_input_gameplay_active());
        mark_device(PC_DEVICE_KEYBOARD_MOUSE);
    } else {
        if (s_swallow_mask & (1L << button)) {
            InterlockedAnd(&s_swallow_mask, ~(1L << button));
            trace("mouse button %d release cleared swallow", button + 1);
            return;
        }
        set_src(id, 0, now, 0);
        trace("mouse button %d up", button + 1);
    }
}

void pc_input_mouse_button_event(int button, int down)
{
    uint64_t now;
    pc_input_init();
    if (s_script_background) return;
    if (!s_focused && down) return;
    now = now_ms();
    observe_button_device(button, down, now);
    if (filter_button(button, down)) {
        if (button >= 0 && button < 5) discard_src(SRC_MOUSE0 + (unsigned)button);
        return;
    }
    button_event_internal(button, down, now);
}

static void wheel_event_internal(int delta, uint64_t now)
{
    unsigned id;
    if (!delta || !kbm_enabled()) return;
    id = delta > 0 ? SRC_WHEEL_UP : SRC_WHEEL_DOWN;
    set_src(id, 1, now, PC_WHEEL_MS);
    set_src(id, 0, now, 0);
    mark_device(PC_DEVICE_KEYBOARD_MOUSE);
}

void pc_input_wheel_event(int delta)
{
    pc_input_init();
    if (s_script_background) return;
    if (s_focused) {
        observe_wheel_device(delta);
        if (filter_wheel(delta)) return;
        wheel_event_internal(delta, now_ms());
    }
}

static void motion_internal(long dx, long dy)
{
    if (dx) InterlockedExchangeAdd(&s_dx, dx);
    if (dy) InterlockedExchangeAdd(&s_dy, dy);
    if ((labs(dx) + labs(dy)) >= 4) mark_device(PC_DEVICE_KEYBOARD_MOUSE);
}

void pc_input_mouse_motion(long dx, long dy)
{
    pc_input_init();
    if (s_script_background) return;
    if (!s_focused || !s_captured) return;
    if (filter_motion(dx, dy)) return;
    if (!kbm_enabled()) return;
    motion_internal(dx, dy);
}

void pc_input_focus_event(int focused)
{
    uint64_t now;
    pc_input_init();
    focused = focused ? 1 : 0;
    if (focused == s_focused) return;   /* minimise and WM_KILLFOCUS can describe the same loss */
    if (s_script_background) {
        /* The script owns virtual input. Record real window focus for the
         * presenter, without cancelling script/filter state or injecting Start. */
        InterlockedExchange(&s_focused, focused);
        trace("laboratory window focus %s (script continues)", focused ? "gained" : "lost");
        return;
    }
    {
        int i;
        for (i = 0; i < PC_FILTER_SLOTS; ++i) {
            const PcInputFilter *f = s_filters[s_filter_order[i]];
            if (f && f->focus) f->focus(focused);
        }
    }
    now = now_ms();
    if (!focused) {
        int was = pc_input_gameplay_active() && !movie_active();
        InterlockedExchange64(&s_gameplay_tick, 0);
        InterlockedExchange(&s_focused, 0);
        clear_state(s_focus_pause_guard != 0);
        if (was) {
            InterlockedExchange(&s_focus_pause_guard, 1);
            InterlockedExchange(&s_gameplay_active, 0);
        }
        /* the game is not driven while you are elsewhere: stop it once, like a pause from the pad */
        if (was) pulse_control(CTL_START, now, PC_PULSE_MS);
        trace("focus lost%s", was ? " (pause requested)" : "");
    } else {
        InterlockedExchange64(&s_gameplay_tick, 0);
        InterlockedExchange(&s_focused, 1);
        InterlockedExchange64(&s_mouse_ignore_until, (LONG64)(now + PC_SWALLOW_MS));
        clear_state(s_focus_pause_guard != 0);
        trace("focus gained");
    }
}

int pc_input_capture_wanted(void)
{
    pc_input_init();
    if (!s_script_background && s_focused && s_capture_query && s_capture_query()) return 1;
    return !s_script_background && s_focused && kbm_enabled() &&
           (pc_input_gameplay_active() || look_gap_recent(now_ms())) && !movie_active();
}

int pc_input_cursor_hidden_wanted(void)
{
    pc_input_init();
    return !s_script_background && s_captured && s_focused && kbm_enabled() && !movie_active();
}

void pc_input_capture_changed(int captured)
{
    const uint64_t now = now_ms();
    pc_input_init();
    captured = captured ? 1 : 0;
    if ((LONG)captured == s_captured) return;
    InterlockedExchange(&s_captured, captured);
    if (s_script_background) return; /* physical pointer changes cannot cancel scripted look/holds */
    /* Counts gathered before capture are stale. Preserve only holds tied to
     * recent gameplay; a button already down in a menu remains a swallowed click. */
    InterlockedExchange(&s_dx, 0);
    InterlockedExchange(&s_dy, 0);
    if (captured) {
        LONG mask = 0;
        int b;
        AcquireSRWLockExclusive(&s_pending_lock);
        for (b = 0; b < 5; b++) {
            const unsigned source = SRC_MOUSE0 + (unsigned)b;
            const int keep_gameplay_hold = s_down[source] && s_source_context[source] == 1 &&
                                           gameplay_hold_gap_context(source, now);
            if (keep_gameplay_hold) trace("mouse button %d gameplay hold preserved across pointer capture", b + 1);
            if (s_down[source] && !keep_gameplay_hold) mask |= 1L << b;
            s_pending_count[source] = 0;
            if (!keep_gameplay_hold) {
                s_source_ack[source] = 0;
                s_down[source] = 0;
                s_latch[source] = 0; /* a menu press must not leak into gameplay as a shot */
            }
        }
        InterlockedOr(&s_swallow_mask, mask);
        ReleaseSRWLockExclusive(&s_pending_lock);
    }
    trace("pointer %s", captured ? "captured" : "released");
}

/* ---- the pad report --------------------------------------------------------------------------------------- */

static int pad_significant(const uint8_t *r)
{
    int i;
    if (r[2] & 0xFF) return 1;
    for (i = 4; i < 12; i++) if (r[i] > 100) return 1;
    for (i = 12; i < 20; i += 2) {
        int v = (int16_t)(r[i] | (r[i + 1] << 8));
        if (v > 16000 || v < -16000) return 1;
    }
    return 0;
}

static void put16(uint8_t *p, int v)
{
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static int get16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static int binding_active(int action, uint64_t now, int gameplay)
{
    int i;
    for (i = 0; i < s_bindings[action].count; i++)
        if (src_active(s_bindings[action].b[i].src, now, gameplay)) return 1;
    return 0;
}

static int gameplay_hold_gap_source_active(unsigned source, uint64_t now)
{
    int down;
    if (source >= SRC_COUNT) return 0;
    AcquireSRWLockShared(&s_pending_lock);
    down = s_down[source] && s_source_context[source] == 1;
    ReleaseSRWLockShared(&s_pending_lock);
    return down && gameplay_hold_gap_context(source, now);
}

static int gameplay_hold_gap_binding_active(int action, uint64_t now)
{
    int i;
    for (i = 0; i < s_bindings[action].count; ++i) {
        const unsigned source = s_bindings[action].b[i].src;
        if (source == SRC_ANY_SHIFT) {
            if (gameplay_hold_gap_source_active(KEY_LSHIFT, now) ||
                gameplay_hold_gap_source_active(KEY_RSHIFT, now)) return 1;
        } else if (source == SRC_ANY_CTRL) {
            if (gameplay_hold_gap_source_active(KEY_LCTRL, now) ||
                gameplay_hold_gap_source_active(KEY_RCTRL, now)) return 1;
        } else if (source == SRC_ANY_ALT) {
            if (gameplay_hold_gap_source_active(KEY_LALT, now) ||
                gameplay_hold_gap_source_active(KEY_RALT, now)) return 1;
        } else if (gameplay_hold_gap_source_active(source, now)) return 1;
    }
    return 0;
}

/* Every control the keyboard and mouse hold right now, as bit 0..23. */
static uint32_t collect_controls(int gameplay, uint64_t now)
{
    uint32_t bits = 0;
    int a, c;

    for (a = 0; a < PC_ACTION_COUNT; a++) {
        int active;
        if (gameplay) active = binding_active(a, now, gameplay);
        else if (a <= PC_ACTION_MOVE_RIGHT || a == PC_ACTION_FIRE || (a == PC_ACTION_AIM && s_aim_hold_mode))
            active = gameplay_hold_gap_binding_active(a, now);
        else active = 0;
        if (active) {
            c = resolve_slot(pc_input_actions[a].slot);
            if (c >= 0 && c < CTL_COUNT_BUTTONS) bits |= 1u << c;
        }
    }
    /* Enter confirms everywhere: menus, hints that say "to continue". */
    if (src_active(KEY_ENTER, now, gameplay) || src_active(KEY_NUMENTER, now, gameplay)) bits |= 1u << CTL_A;
    if (!gameplay) {
        /* Menus navigate with the D-pad and confirm/cancel with A and B. */
        if (src_active_menu(KEY_UP, now) || src_active_menu(SRC_WHEEL_UP, now)) bits |= 1u << CTL_DPAD_UP;
        if (src_active_menu(KEY_DOWN, now) || src_active_menu(SRC_WHEEL_DOWN, now)) bits |= 1u << CTL_DPAD_DOWN;
        if (src_active_menu(KEY_LEFT, now)) bits |= 1u << CTL_DPAD_LEFT;
        if (src_active_menu(KEY_RIGHT, now)) bits |= 1u << CTL_DPAD_RIGHT;
        if (src_active_menu(KEY_SPACE, now) || src_active_menu(SRC_MOUSE0, now)) bits |= 1u << CTL_A;
        if (src_active_menu(KEY_BACKSPACE, now) || src_active_menu(SRC_MOUSE0 + 1, now)) bits |= 1u << CTL_B;
        if (src_active_menu(KEY_X, now)) bits |= 1u << CTL_X;
        if (src_active_menu(KEY_Y, now)) bits |= 1u << CTL_Y;
    }
    return bits;
}

static void run_script(uint64_t now);

static int source_matches(unsigned binding, unsigned source)
{
    if (binding == SRC_ANY_SHIFT) return source == KEY_LSHIFT || source == KEY_RSHIFT;
    if (binding == SRC_ANY_CTRL) return source == KEY_LCTRL || source == KEY_RCTRL;
    if (binding == SRC_ANY_ALT) return source == KEY_LALT || source == KEY_RALT;
    return binding == source;
}

void pc_input_movement(int *sprint_held, int *jump_pressed)
{
    const uint64_t now = now_ms();
    const int active = kbm_enabled() && pc_input_gameplay_active();
    unsigned source;
    int i;
    *sprint_held = *jump_pressed = 0;
    AcquireSRWLockExclusive(&s_pending_lock);
    for (source = 1; source < SRC_COUNT; ++source) {
        const int gameplay = active && s_source_context[source] == 1;
        if (gameplay && s_down[source])
            for (i = 0; i < s_bindings[PC_ACTION_SPRINT].count; ++i)
                if (source_matches(s_bindings[PC_ACTION_SPRINT].b[i].src, source)) *sprint_held = 1;
        if (s_source_generation[source] != s_jump_seen[source]) {
            if (gameplay && (s_down[source] || (uint64_t)s_latch[source] > now))
                for (i = 0; i < s_bindings[PC_ACTION_JUMP].count; ++i)
                    if (source_matches(s_bindings[PC_ACTION_JUMP].b[i].src, source)) *jump_pressed = 1;
            s_jump_seen[source] = s_source_generation[source];
        }
    }
    ReleaseSRWLockExclusive(&s_pending_lock);
}

/* The same action map/menu layer as collect_controls, for one physical source.
 * Called with the pending lock held, without inspecting other source latches. */
static uint32_t source_controls(unsigned source, int gameplay)
{
    uint32_t bits = 0;
    int a, i, control;
    if (!kbm_enabled()) return 0;
    if (gameplay) for (a = 0; a < PC_ACTION_COUNT; ++a) {
        for (i = 0; i < s_bindings[a].count; ++i) {
            if (!source_matches(s_bindings[a].b[i].src, source)) continue;
            control = resolve_slot(pc_input_actions[a].slot);
            if (control >= 0 && control < CTL_COUNT_BUTTONS) bits |= 1u << control;
        }
    }
    if (source == KEY_ENTER || source == KEY_NUMENTER) bits |= 1u << CTL_A;
    if (!gameplay) {
        if (source == KEY_UP || source == SRC_WHEEL_UP) bits |= 1u << CTL_DPAD_UP;
        if (source == KEY_DOWN || source == SRC_WHEEL_DOWN) bits |= 1u << CTL_DPAD_DOWN;
        if (source == KEY_LEFT) bits |= 1u << CTL_DPAD_LEFT;
        if (source == KEY_RIGHT) bits |= 1u << CTL_DPAD_RIGHT;
        if (source == KEY_SPACE || source == SRC_MOUSE0) bits |= 1u << CTL_A;
        if (source == KEY_BACKSPACE || source == SRC_MOUSE0 + 1) bits |= 1u << CTL_B;
        if (source == KEY_X) bits |= 1u << CTL_X;
        if (source == KEY_Y) bits |= 1u << CTL_Y;
    }
    return bits;
}

static void pending_pop(PcPending *queue, unsigned char *count)
{
    if (*count) {
        --*count;
        memmove(queue, queue + 1, (size_t)*count * sizeof(*queue));
    }
}

uint64_t pc_input_native_sample_begin(void)
{
    PcNativeSample *sample;
    const uint64_t now = now_ms();
    uint64_t token;
    int gameplay;
    unsigned id;
    pc_input_init();
    InterlockedExchange(&s_native_authority, 1);
    gameplay = pc_input_gameplay_active();
    AcquireSRWLockExclusive(&s_pending_lock);
    sample = &s_native_samples[(++s_native_sample_generation) % PC_NATIVE_SAMPLE_MAX];
    memset(sample, 0, sizeof(*sample));
    sample->token = s_native_sample_generation;
    token = sample->token;
    sample->press_generation = (uint64_t)s_press_generation;
    /* Remember unacknowledged physical holds independently of input latches.
     * Their release during decode must force neutral, never replay a tap. */
    for (id = 0; id < SRC_COUNT; ++id) {
        const int source_gameplay = gameplay ||
            (s_source_context[id] == 1 && gameplay_hold_gap_context(id, now));
        if (!s_down[id] || s_source_ack[id] ||
            (s_source_context[id] != 2 && s_source_context[id] != source_gameplay)) continue;
        sample->held_generation[id] = s_source_generation[id];
        sample->held_controls[id] = source_controls(id, source_gameplay);
    }
    ReleaseSRWLockExclusive(&s_pending_lock);
    return token;
}

void pc_input_native_sample_end(uint64_t token, uint32_t observed_controls)
{
    const uint64_t now = now_ms();
    const int gameplay = pc_input_gameplay_active();
    uint32_t submitted = 0, cancelled = 0;
    unsigned char source_gameplay[SRC_COUNT];
    unsigned id;
    int control;
    PcNativeSample *sample;
    uint64_t press_generation;
    AcquireSRWLockExclusive(&s_pending_lock);
    sample = &s_native_samples[token % PC_NATIVE_SAMPLE_MAX];
    if (!token || sample->token != token) { ReleaseSRWLockExclusive(&s_pending_lock); return; }
    press_generation = sample->press_generation;
    /* A released button must actually reach neutral before the next queued
     * press can create a second native edge. Never mask a physical pad. */
    InterlockedAnd(&s_neutral_barrier, (LONG)observed_controls);
    for (id = 0; id < SRC_COUNT; ++id) {
        /* A recent gameplay hold reports gameplay controls even if the next
         * camera update has not arrived. Acknowledge the same map it emitted. */
        source_gameplay[id] = (unsigned char)(gameplay || (s_down[id] && s_source_context[id] == 1 &&
                                                          gameplay_hold_gap_context(id, now)));
        while (s_pending_count[id] &&
            (s_pending[id][0].until <= now ||
             (s_pending[id][0].context != 2 && s_pending[id][0].context != source_gameplay[id])))
            pending_pop(s_pending[id], &s_pending_count[id]);
        if ((s_pending_count[id] && s_pending[id][0].generation <= press_generation) ||
            (s_down[id] && !s_pending_count[id] && s_source_generation[id] && s_source_generation[id] <= press_generation &&
             (s_source_context[id] == 2 || s_source_context[id] == source_gameplay[id])))
            submitted |= source_controls(id, source_gameplay[id]);
        submitted |= sample->held_controls[id];
    }
    /* Opposing keys deliberately produce a neutral axis. Acknowledge that
     * observed neutral too, so two released opposing taps cannot stay stuck. */
    for (control = CTL_LS_UP; control < CTL_START; control += 2) {
        const uint32_t pair = 3u << control;
        if ((submitted & pair) == pair && !(observed_controls & pair)) cancelled |= pair;
    }
    for (id = 0; id < SRC_COUNT; ++id) {
        uint32_t required;
        uint64_t generation;
        unsigned sample_id;
        const int queued = s_pending_count[id] && s_pending[id][0].generation <= press_generation;
        const int held = !queued && sample->held_generation[id];
        if (!queued && !held) continue;
        generation = queued ? s_pending[id][0].generation : sample->held_generation[id];
        required = queued ? source_controls(id, source_gameplay[id]) : sample->held_controls[id];
        if (required & (uint32_t)s_neutral_barrier) continue;
        if ((observed_controls & (required & ~cancelled)) != (required & ~cancelled)) continue;
        if (queued) pending_pop(s_pending[id], &s_pending_count[id]);
        /* Overlapping decodes may snapshot the same press. Once acknowledged,
         * none of those older observations may force another neutral frame. */
        for (sample_id = 0; sample_id < PC_NATIVE_SAMPLE_MAX; ++sample_id) {
            PcNativeSample *older = &s_native_samples[sample_id];
            if (older->held_generation[id] && older->held_generation[id] <= generation) {
                older->held_generation[id] = 0;
                older->held_controls[id] = 0;
            }
        }
        /* An older queued press must not acknowledge a newer physical hold,
         * including a provisional press that has no queue entry of its own. */
        if (generation == s_source_generation[id]) s_source_ack[id] = required;
        if (!s_down[id] || s_pending_count[id] || generation != s_source_generation[id]) {
            s_latch[id] = 0;
            InterlockedOr(&s_neutral_barrier, (LONG)(required & ~cancelled));
        }
    }
    for (control = 0; control < CTL_COUNT_BUTTONS; ++control) {
        while (s_pending_pulse_count[control] && s_pending_pulse[control][0].until <= now)
            pending_pop(s_pending_pulse[control], &s_pending_pulse_count[control]);
        if (!s_pending_pulse_count[control] || s_pending_pulse[control][0].generation > press_generation ||
            ((uint32_t)s_neutral_barrier & (1u << control)) ||
            !(observed_controls & (1u << control))) continue;
        pending_pop(s_pending_pulse[control], &s_pending_pulse_count[control]);
        s_pulse[control] = 0;
        InterlockedOr(&s_neutral_barrier, 1L << control);
    }
    sample->token = 0;
    ReleaseSRWLockExclusive(&s_pending_lock);
}

void pc_input_apply_report(uint8_t out[20], int pad_present)
{
    uint64_t now;
    uint32_t bits;
    int gameplay, c, lx = 0, ly = 0, rx = 0, ry = 0;

    pc_input_init();
    now = now_ms();
    run_script(now);
    /* Observe the physical pad before a page consumes or changes its report. The keyboard-only input mode
     * ignores it; AUTO still requires two significant reports and ignores ordinary stick drift. */
    if (s_input_mode == PC_MODE_KEYBOARD_MOUSE) pad_present = 0;
    if (pad_present) {
        if (pad_significant(out)) {
            if (InterlockedIncrement(&s_pad_streak) >= 2) mark_device(PC_DEVICE_CONTROLLER);
        } else {
            InterlockedExchange(&s_pad_streak, 0);
        }
    } else {
        InterlockedExchange(&s_pad_streak, 0);
    }
    if (s_active_device < 0)
        InterlockedCompareExchange(&s_active_device,
            (pad_present && s_input_mode != PC_MODE_KEYBOARD_MOUSE) ? PC_DEVICE_CONTROLLER : PC_DEVICE_KEYBOARD_MOUSE, -1);

    if (filter_report(out, pad_present)) {
        memset(out + 2, 0, 18);              /* a settings page or a native menu holds the pad */
        return;
    }
    if (s_input_mode == PC_MODE_KEYBOARD_MOUSE) memset(out + 2, 0, 18);

    gameplay = pc_input_gameplay_active();
    /* A scripted run (tests) stands in for the keyboard and needs no window; a real player's keys
     * only arrive while the window has focus, and pc_input_focus_event clears them when it goes. */
    bits = kbm_enabled() && (s_focused || s_script_n > 0) ? collect_controls(gameplay, now) : 0;
    if (s_focus_pause_guard && !(s_gameplay_blocked_query && s_gameplay_blocked_query())) bits = 0;
    AcquireSRWLockShared(&s_pending_lock);
    for (c = 0; c < CTL_COUNT_BUTTONS; c++) {
        if (s_native_authority) {
            if (s_pending_pulse_count[c] && s_pending_pulse[c][0].until > now) bits |= 1u << c;
        } else if ((uint64_t)s_pulse[c] > now) bits |= 1u << c;
    }
    bits &= ~(uint32_t)s_neutral_barrier;
    ReleaseSRWLockShared(&s_pending_lock);
    if (s_trace_level >= 2 && bits != s_trace_last_bits) {
        s_trace_last_bits = bits;
        trace("keyboard/mouse controls now 0x%06X (gameplay %d)", bits, gameplay);
    }
    if (!bits) {
        return;
    }

    out[2] |= (uint8_t)((bits & 0x0F) | ((bits >> 12) & 0x0F) << 4);
    for (c = CTL_A; c <= CTL_RT; c++)
        if (bits & (1u << c)) out[4 + (c - CTL_A)] = 0xFF;

    lx = ((bits >> CTL_LS_RIGHT) & 1) - ((bits >> CTL_LS_LEFT) & 1);
    ly = ((bits >> CTL_LS_UP) & 1) - ((bits >> CTL_LS_DOWN) & 1);
    rx = ((bits >> CTL_RS_RIGHT) & 1) - ((bits >> CTL_RS_LEFT) & 1);
    ry = ((bits >> CTL_RS_UP) & 1) - ((bits >> CTL_RS_DOWN) & 1);
    {
        /* A circular stick at 45 degrees reads 0.707 on each axis; so does a diagonal key pair. */
        const double d = 0.70710678118654752;
        int axes[4] = { lx, ly, rx, ry }, i;
        for (i = 0; i < 4; i += 2) {
            double sx = axes[i], sy = axes[i + 1];
            int kx, ky, px, py;
            if (sx != 0 && sy != 0) { sx *= d; sy *= d; }
            kx = (int)(sx * 32767.0); ky = (int)(sy * 32767.0);
            px = get16(out + 12 + i * 2); py = get16(out + 14 + i * 2);
            if (abs(kx) > abs(px)) put16(out + 12 + i * 2, kx);
            if (abs(ky) > abs(py)) put16(out + 14 + i * 2, ky);
        }
    }
}

/* ---- camera ------------------------------------------------------------------------------------------------- */

int pc_input_consume_look(double *yaw_degrees, double *pitch_degrees)
{
    long dx = InterlockedExchange(&s_dx, 0), dy = InterlockedExchange(&s_dy, 0);
    double k;
    pc_input_init();
    if (!kbm_enabled() || (!dx && !dy)) return 0;
    if (dx > PC_LOOK_CLAMP) dx = PC_LOOK_CLAMP;
    if (dx < -PC_LOOK_CLAMP) dx = -PC_LOOK_CLAMP;
    if (dy > PC_LOOK_CLAMP) dy = PC_LOOK_CLAMP;
    if (dy < -PC_LOOK_CLAMP) dy = -PC_LOOK_CLAMP;
    k = PC_DEGREES_PER_COUNT * s_sensitivity;
    *yaw_degrees = PC_YAW_SIGN * (double)dx * k;
    *pitch_degrees = PC_PITCH_SIGN * (s_invert_y ? -1.0 : 1.0) * (double)dy * k;
    return 1;
}

/* ---- prompts ---------------------------------------------------------------------------------------------------- */

PcDevice pc_input_active_device(void)
{
    LONG d;
    pc_input_init();
    d = s_active_device;
    return d < 0 ? PC_DEVICE_KEYBOARD_MOUSE : (PcDevice)d;
}

PcDevice pc_input_prompt_device(void)
{
    pc_input_init();
    if (s_prompt_mode == PC_MODE_KEYBOARD_MOUSE) return PC_DEVICE_KEYBOARD_MOUSE;
    if (s_prompt_mode == PC_MODE_CONTROLLER) return PC_DEVICE_CONTROLLER;
    if (s_input_mode == PC_MODE_KEYBOARD_MOUSE) return PC_DEVICE_KEYBOARD_MOUSE;
    if (s_input_mode == PC_MODE_CONTROLLER) return PC_DEVICE_CONTROLLER;
    return pc_input_active_device();
}

static const char *src_label(unsigned id, char *buf, size_t cap)
{
    const struct KeyName *k = key_by_id(id);
    if (!k) { snprintf(buf, cap, "KEY %02X", id & 0xFF); return buf; }
    if (s_layout_labels && id < 0x100 && (isalpha((unsigned char)k->name[0]) || isdigit((unsigned char)k->name[0])) && !k->name[1]) {
        /* the letter printed on that physical key in the player's layout, when it is plain ASCII */
        UINT vk = MapVirtualKeyW(id & 0xFF, MAPVK_VSC_TO_VK_EX);
        UINT ch = vk ? MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) : 0;
        if (!(ch & 0x80000000u) && ch >= 0x20 && ch < 0x7F) {
            buf[0] = (char)toupper((int)ch); buf[1] = 0;
            return buf;
        }
    }
    snprintf(buf, cap, "%s", k->label);
    return buf;
}

int pc_input_source_label(unsigned source, char *out, size_t cap)
{
    if (!out || cap < 2 || !key_by_id(source)) return 0;
    src_label(source, out, cap);
    return 1;
}

int pc_input_prompt_text(int slot, char *out, size_t cap)
{
    int a;
    char label[32];
    pc_input_init();
    if (!out || cap < 4) return 0;
    for (a = 0; a < PC_ACTION_COUNT; a++) {
        if (pc_input_actions[a].slot != slot) continue;
        if (s_bindings[a].count > 0) {          /* the first binding is the one shown */
            src_label(s_bindings[a].b[0].src, label, sizeof(label));
            snprintf(out, cap, "[%s]", label);
        } else {
            snprintf(out, cap, "[UNBOUND]");
        }
        return 1;
    }
    return 0;
}

int pc_input_confirm_text(char *out, size_t cap)
{
    if (!out || cap < 4) return 0;
    snprintf(out, cap, "[ENTER]");
    return 1;
}

int pc_input_menu_text(PcMenuKey key, char *out, size_t cap)
{
    const char *text = key == PC_MENU_CONFIRM ? "[ENTER]" : key == PC_MENU_BACK ? "[ESC]" :
                       key == PC_MENU_X ? "[X]" : key == PC_MENU_Y ? "[Y]" : NULL;
    if (!out || cap < 8 || !text) return 0;
    snprintf(out, cap, "%s", text);
    return 1;
}

int pc_input_expand_prompt(uint16_t *buf, size_t cap, int confirm_only)
{
    enum { MAX_UNITS = 512 };
    uint16_t out[MAX_UNITS];
    size_t len = 0, o = 0, i;
    int replaced = 0;

    pc_input_init();
    if (!buf || cap < 2) return 0;
    if (cap > MAX_UNITS) cap = MAX_UNITS;
    while (len < cap && buf[len]) len++;
    if (len >= cap) return 0;             /* no terminator inside the capacity: not ours to touch */

    for (i = 0; i < len; i++) {
        uint16_t c = buf[i];
        char text[48];
        int have = 0;
        if (c >= 0xF001 && c <= 0xF025) {
            if (confirm_only || c == 0xF025) have = pc_input_confirm_text(text, sizeof(text));
            else have = pc_input_prompt_text((int)(c - 0xF000), text, sizeof(text));
        }
        if (have) {
            size_t n = strlen(text), tail = len - i - 1;
            if (o + n + tail <= cap - 1) {
                size_t k;
                for (k = 0; k < n; k++) out[o++] = (uint16_t)(unsigned char)text[k];
                replaced++;
                continue;
            }
        }
        out[o++] = c;
    }
    if (!replaced) return 0;
    out[o] = 0;
    memcpy(buf, out, (o + 1) * sizeof(uint16_t));
    return replaced;
}

/* ---- scripted events (tests) --------------------------------------------------------------------------------------- */

/*
 * RECOMP_INPUT_SCRIPT: host events the window would produce, on the OHCI thread's clock, so a run
 * can be driven the same way every time without touching the real keyboard or mouse. Comma
 * separated "ms:verb[:args]" with ms counted from the first pad report:
 *   down:KEY   up:KEY   tap:KEY        keys by binding name (Mouse1.. and WheelUp/Down included)
 *   move:DX:DY                         raw mouse counts (as if the pointer were captured)
 *   wheel:+1 | wheel:-1
 *   focus:0 | focus:1
 * e.g. "3000:down:W,5000:up:W,6000:move:400:0,7000:tap:Mouse1".
 * RECOMP_INPUT_SCRIPT_BACKGROUND=1 additionally isolates a fully valid nonempty
 * script from physical keyboard/mouse/focus and never requests pointer capture.
 * Ordinary player input and default scripted focus testing retain their behavior.
 */
static void load_script(void)
{
    const char *e = getenv("RECOMP_INPUT_SCRIPT");
    const char *background = getenv("RECOMP_INPUT_SCRIPT_BACKGROUND");
    int valid = 1;
    s_script_n = s_script_next = 0;
    s_script_t0 = 0;
    s_script_background = 0;
    while (e && *e && s_script_n < SCRIPT_MAX) {
        ScriptEvent *ev = &s_script[s_script_n];
        char *end;
        char verb[16], arg[32];
        size_t n;
        memset(ev, 0, sizeof(*ev));
        errno = 0;
        ev->at_ms = strtoull(e, &end, 10);
        if (!isdigit((unsigned char)*e) || end == e || errno == ERANGE) valid = 0;
        if (s_script_n && ev->at_ms < s_script[s_script_n - 1].at_ms) valid = 0;
        if (*end != ':') { valid = 0; break; }
        e = end + 1;
        n = strcspn(e, ":,");
        if (n == 0 || n >= sizeof(verb)) { valid = 0; break; }
        memcpy(verb, e, n); verb[n] = 0;
        e += n;
        arg[0] = 0;
        if (*e == ':') {
            e++;
            n = strcspn(e, ",");
            if (n >= sizeof(arg)) { valid = 0; break; }
            memcpy(arg, e, n); arg[n] = 0;
            e += n;
        }
        if (!_stricmp(verb, "down") || !_stricmp(verb, "up") || !_stricmp(verb, "tap")) {
            const struct KeyName *k = key_by_name(arg);
            unsigned id = k ? k->id : (!_stricmp(arg, "Esc") || !_stricmp(arg, "Escape")) ? KEY_ESC : 0;
            if (!id) { fprintf(stderr, "[INPUT] RECOMP_INPUT_SCRIPT: unknown key '%s'\n", arg); valid = 0; break; }
            ev->verb = (char)tolower((unsigned char)verb[0]); ev->arg = id;
        } else if (!_stricmp(verb, "move")) {
            char *check_end;
            errno = 0;
            (void)strtol(arg, &check_end, 10);
            if (check_end == arg || errno == ERANGE) valid = 0;
            if (*check_end == ':') {
                const char *second = check_end + 1;
                errno = 0;
                (void)strtol(second, &check_end, 10);
                if (check_end == second || errno == ERANGE) valid = 0;
            }
            if (*check_end) valid = 0;
            ev->verb = 'm'; ev->a = strtol(arg, &end, 10);
            ev->b = *end == ':' ? strtol(end + 1, NULL, 10) : 0;
        } else if (!_stricmp(verb, "wheel")) {
            errno = 0;
            (void)strtol(arg, &end, 10);
            if (end == arg || *end || errno == ERANGE) valid = 0;
            ev->verb = 'w'; ev->a = strtol(arg, NULL, 10);
        } else if (!_stricmp(verb, "focus")) {
            errno = 0;
            (void)strtol(arg, &end, 10);
            if (end == arg || *end || errno == ERANGE) valid = 0;
            ev->verb = 'f'; ev->a = strtol(arg, NULL, 10);
            if (ev->a != 0 && ev->a != 1) valid = 0;
        } else {
            fprintf(stderr, "[INPUT] RECOMP_INPUT_SCRIPT: unknown verb '%s'\n", verb);
            valid = 0;
            break;
        }
        s_script_n++;
        if (*e == ',') { e++; if (!*e) valid = 0; } else break;
    }
    if (s_script_n == SCRIPT_MAX && e && *e) valid = 0;
    s_script_background = valid && s_script_n > 0 && background && !strcmp(background, "1");
    if (s_script_n) fprintf(stderr, "[INPUT] %d scripted event(s) from RECOMP_INPUT_SCRIPT\n", s_script_n);
    if (s_script_background) fprintf(stderr, "[INPUT] laboratory background script isolated from physical input\n");
}

static void script_key(unsigned id, int down, uint64_t now)
{
    if (id >= SRC_MOUSE0 && id < SRC_MOUSE0 + 5) {
        observe_button_device((int)(id - SRC_MOUSE0), down, now);
        if (filter_button((int)(id - SRC_MOUSE0), down)) { discard_src(id); return; }
        button_event_internal((int)(id - SRC_MOUSE0), down, now);
    } else if (id == SRC_WHEEL_UP || id == SRC_WHEEL_DOWN) {
        if (down) {
            const int delta = id == SRC_WHEEL_UP ? 1 : -1;
            observe_wheel_device(delta);
            if (filter_wheel(delta)) return;
            wheel_event_internal(delta, now);
        }
    } else {
        observe_key_device(id, down, 0);
        if (filter_key(id, down, 0)) { discard_src(id); return; }
        key_event_internal(id, down, 0, now);
    }
}

static void run_script(uint64_t now)
{
    uint64_t t;
    if (s_script_next >= s_script_n) return;
    if (!s_script_t0) s_script_t0 = now;
    t = now - s_script_t0;
    while (s_script_next < s_script_n && s_script[s_script_next].at_ms <= t) {
        const ScriptEvent *ev = &s_script[s_script_next++];
        trace("script t=%llu verb %c", (unsigned long long)t, ev->verb);
        switch (ev->verb) {
        case 'd': script_key(ev->arg, 1, now); break;
        case 'u': script_key(ev->arg, 0, now); break;
        case 't': script_key(ev->arg, 1, now); script_key(ev->arg, 0, now); break; /* latch keeps it down */
        case 'm': if (!filter_motion(ev->a, ev->b) && kbm_enabled()) motion_internal(ev->a, ev->b); break;   /* as a captured pointer's counts */
        case 'w':
            observe_wheel_device((int)ev->a);
            if (!filter_wheel((int)ev->a)) wheel_event_internal((int)ev->a, now);
            break;
        case 'f': pc_input_focus_event((int)ev->a); break;
        default: break;
        }
    }
}

void pc_input_set_filter(const PcInputFilter *filter) { s_filters[0] = filter; }
void pc_input_set_filter_slot(int slot, const PcInputFilter *filter) { if (slot >= 0 && slot < PC_FILTER_SLOTS) s_filters[slot] = filter; }
void pc_input_set_capture_query(int (*query)(void)) { s_capture_query = query; }

/* ---- tests -------------------------------------------------------------------------------------------------------------- */

void pc_input_test_reset(void)
{
    int i;
    pc_input_init();                 /* consume the once-only init, then start from a clean slate */
    clear_state(0);
    for (i = 0; i < PC_ACTION_COUNT; i++) s_bindings[i].count = 0;
    s_resolver = NULL;
    s_aim_hold_mode = 0;
    s_movie_active = NULL;
    s_gameplay_blocked_query = NULL;
    s_gameplay_tick = 0;
    s_frame_authority = s_gameplay_seen = s_gameplay_active = s_focus_pause_guard = 0;
    s_native_authority = s_neutral_barrier = 0;
    s_press_generation = 0;
    s_native_sample_generation = 0;
    s_active_device = -1;
    s_pad_streak = 0;
    s_focused = 1;
    s_captured = 0;
    s_mouse_ignore_until = 0;
    s_script_n = s_script_next = 0;
    s_script_t0 = 0;
    s_script_background = 0;
    load_config();
    load_script();
}

void pc_input_test_set_time(uint64_t now) { s_test_now = now; }
void pc_input_test_set_layout_labels(int layout_aware) { s_layout_labels = layout_aware; }
