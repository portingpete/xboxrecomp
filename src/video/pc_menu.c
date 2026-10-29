/* Native, editable menus. See pc_menu.h.
 *
 * The page logic follows the menu classes the game's own pages are written with (BkMenuList,
 * BkMenuButton, BkValueList, BkMovieclipSlider in the APT movies): up/down move the focus along the
 * list and wrap, left/right change a choice or a slider, confirm runs the focused item's action and
 * back the page's back action, each with the same sound the original plays. A confirm on an item
 * without an action of its own goes back when the page's back action is a page (the original does
 * that too: "Going Backwards 2").
 */
#include "pc_menu.h"
#include "pc_video_menu.h"
#include "pc_movie_skip.h"
#include "pc_speech_subtitle.h"
#include "pc_photo.h"
#include "../input/pc_input.h"
#include "../input/pc_controls.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---- the definition model ------------------------------------------------------------------------------ */

#define PCM_NAME 40
#define PCM_SPEC 120
#define PCM_PAGES 24
#define PCM_ITEMS 12
#define PCM_TEXTS 10
#define PCM_CMDS 5
#define PCM_VARS 8
#define PCM_TEXT_CAP 160

enum { OP_NONE, OP_GAMECODE, OP_PAGE, OP_SET, OP_VIDEO, OP_BACK, OP_OPEN, OP_CLOSE, OP_EXIT, OP_RESET_VIDEO, OP_RESET_KEYS, OP_RESET_PAD,
       OP_PHOTO_SHOT, OP_PHOTO_RESET, OP_PHOTO_HIDE };
typedef struct Cmd { int op; int nargs; char arg[4][PCM_NAME]; } Cmd;
typedef struct Script { int n; Cmd cmd[PCM_CMDS]; } Script;

/* IT_KEY / IT_PAD: an action's keyboard keys (two columns) / controller button; IT_LAYOUT: the controller layout;
 * IT_PHOTO: a photo mode setting (pc_photo rows, in video_row). */
enum { IT_BUTTON, IT_TOGGLE, IT_SLIDER, IT_VIDEO, IT_KEY, IT_PAD, IT_LAYOUT, IT_PHOTO };
typedef struct Item {
    int kind;
    char text[PCM_SPEC];
    char value[PCM_SPEC];          /* a second column of text (objectives: &FE_BLACKMAILRATING) */
    Script act, on_focus;
    char var[PCM_NAME], call[PCM_NAME];
    int lo, hi, step;
    int video_row;
    int action;                    /* IT_KEY, IT_PAD: a PcAction */
    char cond[PCM_NAME];           /* shown only when the game's number is non-zero ("!" = zero) */
    char dim[PCM_NAME];            /* drawn in the dim colour when the game's number is non-zero ("!" = zero) */
    char off[PCM_NAME];            /* disabled (greyed, skipped by up / down) when the game's number is non-zero */
    float gap;                     /* extra space before the item */
} Item;

enum { TX_LABEL, TX_TEXT, TX_SMALL };
typedef struct Text {
    int role, right;
    float x, y, wrap, size;
    char spec[PCM_SPEC];
    char cond[PCM_NAME];
} Text;

typedef struct Page {
    char name[PCM_NAME];
    char title[PCM_SPEC];
    Text text[PCM_TEXTS];
    int ntext;
    Item item[PCM_ITEMS];
    int nitem;
    float list_x, list_y, step, col1, col2, item_size;
    float logo_x, logo_y, title_x, title_y, prompt_x, prompt_y, prompt_step, bar_w, rail_l, rail_r;
    int focus, no_logo, plain_focus;
    float values_x;                /* item values right-aligned at this x (0: at the first column, left-aligned) */
    float sep[4][3];               /* separator lines: x1, y, x2 */
    int nsep;
    Script back;
    char loading[PCM_NAME];
    char require[2][PCM_NAME];
    int nrequire;
    unsigned prompts;              /* 1 select, 2 confirm, 4 back */
    int start_back;                /* the pad's START is back here (the pause page), confirm elsewhere */
    int four_way;                  /* the select prompt shows all four arrows */
    int line;
    uint32_t column;
    int has_column;                /* front-end pages leave the movie unchanged between the rails */
    int photo;                     /* photo mode's page: a panel over the picture, photo mode on while shown */
    float panel[4];                /* its box: x, y, w, h */
} Page;

typedef struct Style {
    uint32_t shade, column, rail, item, focus, label, text, title, prompt, bar, line, choice, choice_on, bullet, bullet_on, logo, dim,
             disabled;
    float title_size, item_size, text_size, focus_scale, logo_w, intro, anim;
    int sounds;
} Style;

typedef struct Doc { Style style; Page page[PCM_PAGES]; int npage; } Doc;

static void style_defaults(Style *s)
{
    /* Measured from the original pages as this port shows them (640 x 480 frames of the pause menu). */
    s->shade = 0xD9091010u;        /* the game behind the menu: about 15% of it shows through */
    s->column = 0x1E1F201Eu;       /* between the rails it stays almost as it is */
    s->rail = 0xFF5A6057u;
    s->item = 0xFF36473Du;         /* the items' 0x6F917C at the brightness the frame shows */
    s->focus = 0xFF7E7E7Eu;
    s->label = 0xFF36473Du;
    s->text = 0xFF7F7F7Fu;
    s->title = 0xFF7F7F7Fu;
    s->prompt = 0xFF7F7F7Fu;
    s->bar = 0xF0090B0Au;
    s->line = 0xFF7F7F7Fu;
    s->choice = 0xFF36473Du;
    s->choice_on = 0xFF7F7F7Fu;
    s->bullet = 0xFF36473Du;
    s->bullet_on = 0xFF7F7F7Fu;
    s->logo = 0xFF7E7E7Eu;              /* the logo shows at the same half brightness as the rest of the page */
    s->dim = 0xFF2B362Bu;               /* a row that does not apply (objectives) */
    s->disabled = 0xFF202B26u;          /* an item that cannot be chosen now (mission failed: Continue) */
    s->title_size = 30.0f;
    s->item_size = 15.0f;
    s->text_size = 15.0f;
    s->focus_scale = 1.1f;
    s->logo_w = 192.0f;                 /* the 128-texel logo at the 1.5 scale the pages place it with */
    s->intro = 0.25f;
    s->anim = 0.3f;
    s->sounds = 1;
}

static void page_defaults(Page *p)
{
    memset(p, 0, sizeof(*p));
    p->list_x = 285; p->list_y = 241; p->step = 25; p->col1 = 0; p->col2 = 0; p->item_size = 0;
    p->logo_x = 250.5f; p->logo_y = 57; p->title_x = 266; p->title_y = 86;
    p->prompt_x = 285; p->prompt_y = 373; p->prompt_step = 28;
    p->bar_w = 248; p->rail_l = 73; p->rail_r = 233;
    p->prompts = 7;
}

/* ---- parsing ----------------------------------------------------------------------------------------------- */

typedef struct Parse { char *log; size_t cap, len; int line, errors; } Parse;

static void perr(Parse *ps, const char *fmt, const char *a)
{
    char msg[256];
    int n;
    ps->errors++;
    if (!ps->log || ps->len + 2 >= ps->cap) return;
    snprintf(msg, sizeof(msg), fmt, a ? a : "");
    n = snprintf(ps->log + ps->len, ps->cap - ps->len, "menus.ini:%d: %s\n", ps->line, msg);
    if (n > 0) ps->len += (size_t)n < ps->cap - ps->len ? (size_t)n : ps->cap - ps->len - 1;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* Splits on `sep` outside double quotes. Returns the number of fields (at most max). */
static int split(char *s, char sep, char **out, int max)
{
    int n = 0, q = 0;
    char *start = s;
    for (;; s++) {
        if (*s == '"') q = !q;
        if (!*s || (*s == sep && !q)) {
            const int end = !*s;
            *s = 0;
            if (n < max) out[n++] = trim(start);
            if (end) break;
            start = s + 1;
        }
    }
    return n;
}

static void copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int text_spec_ok(const char *s)
{
    const size_t n = strlen(s);
    if (!n) return 0;
    if (s[0] == '$' || s[0] == '&' || s[0] == '@') return n > 1;
    return s[0] == '"' && n >= 2 && s[n - 1] == '"';
}

static int parse_script(Parse *ps, char *text, Script *out)
{
    char *part[PCM_CMDS + 1];
    int n = split(text, ';', part, PCM_CMDS + 1), i;
    memset(out, 0, sizeof(*out));
    if (n > PCM_CMDS) { perr(ps, "too many actions (at most %s)", "5"); return 0; }
    for (i = 0; i < n; i++) {
        char *w[6];
        int k = 0, j;
        Cmd *c = &out->cmd[out->n];
        char *p = part[i];
        if (!*p) continue;
        while (*p && k < 6) {                     /* words, a quoted word kept whole */
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (*p == '"') {
                char *e = strchr(p + 1, '"');
                if (!e) { perr(ps, "unterminated quote in '%s'", part[i]); return 0; }
                w[k++] = p + 1; *e = 0; p = e + 1;
            } else {
                w[k++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = 0;
            }
        }
        if (!k) continue;
        if (!_stricmp(w[0], "gamecode")) c->op = OP_GAMECODE;
        else if (!_stricmp(w[0], "page")) c->op = OP_PAGE;
        else if (!_stricmp(w[0], "set")) c->op = OP_SET;
        else if (!_stricmp(w[0], "video")) c->op = OP_VIDEO;
        else if (!_stricmp(w[0], "back")) c->op = OP_BACK;
        else if (!_stricmp(w[0], "open")) c->op = OP_OPEN;
        else if (!_stricmp(w[0], "close")) c->op = OP_CLOSE;
        else if (!_stricmp(w[0], "exit")) c->op = OP_EXIT;
        else if (!_stricmp(w[0], "reset_video")) c->op = OP_RESET_VIDEO;
        else if (!_stricmp(w[0], "reset_keys")) c->op = OP_RESET_KEYS;
        else if (!_stricmp(w[0], "reset_pad")) c->op = OP_RESET_PAD;
        else if (!_stricmp(w[0], "photo_shot")) c->op = OP_PHOTO_SHOT;
        else if (!_stricmp(w[0], "photo_reset")) c->op = OP_PHOTO_RESET;
        else if (!_stricmp(w[0], "photo_hide")) c->op = OP_PHOTO_HIDE;
        else if (!_stricmp(w[0], "none")) c->op = OP_NONE;
        else { perr(ps, "unknown action '%s' (gamecode, page, open, close, set, video, reset_video, reset_keys, reset_pad, photo_shot, photo_reset, photo_hide, back, exit, none)", w[0]); return 0; }
        if ((c->op == OP_GAMECODE || c->op == OP_PAGE || c->op == OP_OPEN) && k < 2) { perr(ps, "'%s' needs a name", w[0]); return 0; }
        if (c->op == OP_SET && k != 3) { perr(ps, "'set' needs a name and a value", NULL); return 0; }
        if ((c->op == OP_PAGE || c->op == OP_OPEN) && k != 2) { perr(ps, "'%s' takes one name", w[0]); return 0; }
        if ((c->op == OP_CLOSE || c->op == OP_EXIT || c->op >= OP_RESET_VIDEO) && k != 1) { perr(ps, "'%s' takes nothing", w[0]); return 0; }
        if (k > 5) { perr(ps, "too many arguments for '%s' (at most 3)", w[0]); return 0; }
        c->nargs = k - 1;
        for (j = 1; j < k; j++) {
            if (strlen(w[j]) >= PCM_NAME) { perr(ps, "'%s' is too long", w[j]); return 0; }
            copy(c->arg[j - 1], PCM_NAME, w[j]);
        }
        if (c->op != OP_NONE) out->n++;     /* "none" is no action: confirm then does what the original does without one */
    }
    return 1;
}

static int parse_float(const char *s, float *out)
{
    char *e;
    double v = strtod(s, &e);
    if (e == s || !isfinite(v)) return 0;
    while (*e == ' ' || *e == '\t') e++;
    if (*e) return 0;
    *out = (float)v;
    return 1;
}

/* "X Y" or "X Y STEP" -- numbers separated by spaces. */
static int parse_floats(char *s, float *out, int n)
{
    char *w[8];
    int k = split(s, ' ', w, 8), i, j = 0;
    for (i = 0; i < k && j < n; i++) {
        if (!*w[i]) continue;
        if (!parse_float(w[i], &out[j])) return 0;
        j++;
    }
    for (; i < k; i++) if (*w[i]) return 0;
    return j == n;
}

static int parse_colour(const char *s, uint32_t *out)
{
    char *e;
    unsigned long v;
    size_t n = strlen(s);
    if (n != 6 && n != 8) return 0;
    v = strtoul(s, &e, 16);
    if (*e) return 0;
    *out = n == 6 ? (uint32_t)v | 0xFF000000u : (uint32_t)v;
    return 1;
}

/* Item options after the text: "if VAR", "unless VAR", "focus ACTIONS", "value TEXT", "gap N". */
static int parse_item_option(Parse *ps, char *opt, Item *it)
{
    char *sp = opt;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (*sp) *sp++ = 0;
    sp = trim(sp);
    if (!_stricmp(opt, "if") || !_stricmp(opt, "unless")) {
        if (!*sp || strlen(sp) + 2 >= PCM_NAME) { perr(ps, "'%s' needs a name", opt); return 0; }
        snprintf(it->cond, PCM_NAME, "%s%s", _stricmp(opt, "unless") ? "" : "!", sp);
        return 1;
    }
    if (!_stricmp(opt, "dim") || !_stricmp(opt, "disabled")) {
        char *w2 = sp;
        while (*w2 && *w2 != ' ' && *w2 != '\t') w2++;
        if (*w2) *w2++ = 0;
        w2 = trim(w2);
        if ((_stricmp(sp, "if") && _stricmp(sp, "unless")) || !*w2 || strlen(w2) + 2 >= PCM_NAME) { perr(ps, "%s needs: if NAME or unless NAME", opt); return 0; }
        snprintf(_stricmp(opt, "dim") ? it->off : it->dim, PCM_NAME, "%s%s", _stricmp(sp, "unless") ? "" : "!", w2);
        return 1;
    }
    if (!_stricmp(opt, "focus")) return parse_script(ps, sp, &it->on_focus);
    if (!_stricmp(opt, "value")) {
        if (!text_spec_ok(sp)) { perr(ps, "bad text '%s'", sp); return 0; }
        copy(it->value, PCM_SPEC, sp);
        return 1;
    }
    if (!_stricmp(opt, "gap")) {
        if (!parse_float(sp, &it->gap)) { perr(ps, "bad gap '%s'", sp); return 0; }
        return 1;
    }
    perr(ps, "unknown item option '%s' (if, unless, dim, disabled, focus, value, gap)", opt);
    return 0;
}

static int action_named(const char *name)
{
    int a;
    for (a = 0; a < PC_ACTION_COUNT; a++) if (!_stricmp(name, pc_input_actions[a].name)) return a;
    return -1;
}

static int photo_row_named(const char *name)
{
    static const char *const names[] = { "fov", "roll", "weapon", "speed", "range" };
    int i;
    for (i = 0; i < PCP_ROW_COUNT; i++) if (!_stricmp(name, names[i])) return i;
    return -1;
}

static int video_row_named(const char *name)
{
    static const char *const names[] = { "mode", "window", "internal", "aa", "af", "dof", "sharpen", "ao_method", "ao_quality", "reflections",
        "ray_tracing", "aspect", "fov", "scale", "filter", "vsync", "fps", "blur", "crosshair", "hitmarkers", "optional_secondaries",
        "lod_distance", "bullet_damage", "enemy_health", "headshot_instakill", "aim_mode", "sprint", "jump" };
    int i;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) if (!_stricmp(name, names[i])) return i;
    return -1;
}

static int parse_page_key(Parse *ps, Page *p, const char *key, char *val)
{
    char *f[8];
    int n, i;
    if (!_stricmp(key, "title")) {
        if (!text_spec_ok(val)) { perr(ps, "bad title '%s'", val); return 0; }
        copy(p->title, PCM_SPEC, val);
        return 1;
    }
    if (!_stricmp(key, "label") || !_stricmp(key, "text") || !_stricmp(key, "small")) {
        Text *t;
        char *w[5];
        int k;
        if (p->ntext >= PCM_TEXTS) { perr(ps, "too many texts on one page", NULL); return 0; }
        t = &p->text[p->ntext];
        memset(t, 0, sizeof(*t));
        t->role = !_stricmp(key, "label") ? TX_LABEL : !_stricmp(key, "text") ? TX_TEXT : TX_SMALL;
        n = split(val, '|', f, 8);
        k = split(f[0], ' ', w, 4);              /* X Y TEXT (a literal with spaces is quoted) */
        if (k != 3 || !parse_float(w[0], &t->x) || !parse_float(w[1], &t->y)) { perr(ps, "%s needs: X Y TEXT (quote a text with spaces)", key); return 0; }
        {
            /* everything after the second number is the text */
            char *rest = w[2];
            if (!text_spec_ok(rest)) { perr(ps, "bad text '%s'", rest); return 0; }
            copy(t->spec, PCM_SPEC, rest);
        }
        for (i = 1; i < n; i++) {
            char *o = f[i], *sp = o;
            while (*sp && *sp != ' ') sp++;
            if (*sp) *sp++ = 0;
            sp = trim(sp);
            if (!_stricmp(o, "wrap")) { if (!parse_float(sp, &t->wrap)) { perr(ps, "bad wrap '%s'", sp); return 0; } }
            else if (!_stricmp(o, "size")) { if (!parse_float(sp, &t->size)) { perr(ps, "bad size '%s'", sp); return 0; } }
            else if (!_stricmp(o, "if") || !_stricmp(o, "unless")) snprintf(t->cond, PCM_NAME, "%s%s", _stricmp(o, "unless") ? "" : "!", sp);
            else if (!_stricmp(o, "right")) t->right = 1;
            else { perr(ps, "unknown text option '%s' (wrap, size, right, if, unless)", o); return 0; }
        }
        p->ntext++;
        return 1;
    }
    if (!_stricmp(key, "item") || !_stricmp(key, "toggle") || !_stricmp(key, "slider") || !_stricmp(key, "setting") ||
        !_stricmp(key, "keybind") || !_stricmp(key, "padbind") || !_stricmp(key, "padlayout") || !_stricmp(key, "photo")) {
        Item *it;
        int first_opt;
        if (p->nitem >= PCM_ITEMS) { perr(ps, "too many items on one page (at most %s)", "12"); return 0; }
        it = &p->item[p->nitem];
        memset(it, 0, sizeof(*it));
        n = split(val, '|', f, 8);
        if (!text_spec_ok(f[0])) { perr(ps, "bad item text '%s'", f[0]); return 0; }
        copy(it->text, PCM_SPEC, f[0]);
        if (!_stricmp(key, "item")) {
            it->kind = IT_BUTTON;
            if (n >= 2 && !parse_script(ps, f[1], &it->act)) return 0;
            first_opt = 2;
        } else if (!_stricmp(key, "keybind") || !_stricmp(key, "padbind")) {
            it->kind = !_stricmp(key, "keybind") ? IT_KEY : IT_PAD;
            if (n < 2 || (it->action = action_named(f[1])) < 0 ||
                (it->kind == IT_PAD && !pc_controls_pad_action((PcAction)it->action))) {
                perr(ps, it->kind == IT_KEY ? "keybind needs: TEXT | ACTION (as RECOMP_BLACK_BIND_<ACTION>)" :
                                              "padbind needs: TEXT | a button ACTION", NULL);
                return 0;
            }
            first_opt = 2;
        } else if (!_stricmp(key, "padlayout")) {
            it->kind = IT_LAYOUT;
            first_opt = 1;
        } else if (!_stricmp(key, "photo")) {
            it->kind = IT_PHOTO;
            if (n < 2 || (it->video_row = photo_row_named(f[1])) < 0) { perr(ps, "photo needs: TEXT | fov, roll, weapon, speed or range", NULL); return 0; }
            first_opt = 2;
        } else if (!_stricmp(key, "setting")) {
            it->kind = IT_VIDEO;
            if (n < 2 || (it->video_row = video_row_named(f[1])) < 0) { perr(ps, "setting needs: TEXT | video row name", NULL); return 0; }
            first_opt = 2;
        } else {
            it->kind = !_stricmp(key, "toggle") ? IT_TOGGLE : IT_SLIDER;
            if (n < 3 || !*f[1] || !*f[2]) { perr(ps, "%s needs: TEXT | VALUE | FUNCTION", key); return 0; }
            copy(it->var, PCM_NAME, f[1]);
            copy(it->call, PCM_NAME, f[2]);
            first_opt = 3;
            if (it->kind == IT_SLIDER) {
                float r[3];
                if (n < 4 || !parse_floats(f[3], r, 3) || r[2] <= 0 || r[1] <= r[0]) { perr(ps, "slider needs: ... | MIN MAX STEP", NULL); return 0; }
                it->lo = (int)r[0]; it->hi = (int)r[1]; it->step = (int)r[2];
                first_opt = 4;
            }
        }
        for (i = first_opt; i < n; i++) if (*f[i] && !parse_item_option(ps, f[i], it)) return 0;
        p->nitem++;
        return 1;
    }
    if (!_stricmp(key, "photomode")) {
        p->photo = !_stricmp(val, "on");
        return 1;
    }
    if (!_stricmp(key, "panel")) {
        if (!parse_floats(val, p->panel, 4)) { perr(ps, "panel needs: X Y W H", NULL); return 0; }
        return 1;
    }
    if (!_stricmp(key, "column")) {
        if (!parse_colour(val, &p->column)) { perr(ps, "bad column colour '%s'", val); return 0; }
        p->has_column = 1;
        return 1;
    }
    if (!_stricmp(key, "list")) {
        float r[3];
        if (!parse_floats(val, r, 3)) { perr(ps, "list needs: X Y STEP", NULL); return 0; }
        p->list_x = r[0]; p->list_y = r[1]; p->step = r[2];
        return 1;
    }
    if (!_stricmp(key, "columns")) {
        float r[2];
        if (!parse_floats(val, r, 2)) { perr(ps, "columns needs: X1 X2", NULL); return 0; }
        p->col1 = r[0]; p->col2 = r[1];
        return 1;
    }
    if (!_stricmp(key, "logo") && !_stricmp(val, "none")) { p->no_logo = 1; return 1; }
    if (!_stricmp(key, "logo") || !_stricmp(key, "title_at") || !_stricmp(key, "rails")) {
        float r[2];
        if (!parse_floats(val, r, 2)) { perr(ps, "%s needs: X Y", key); return 0; }
        if (!_stricmp(key, "logo")) { p->logo_x = r[0]; p->logo_y = r[1]; p->no_logo = 0; }
        else if (!_stricmp(key, "title_at")) { p->title_x = r[0]; p->title_y = r[1]; }
        else { p->rail_l = r[0]; p->rail_r = r[1]; }
        return 1;
    }
    if (!_stricmp(key, "prompts_at")) {
        float r[3];
        if (!parse_floats(val, r, 3)) { perr(ps, "prompts_at needs: X Y STEP", NULL); return 0; }
        p->prompt_x = r[0]; p->prompt_y = r[1]; p->prompt_step = r[2];
        return 1;
    }
    if (!_stricmp(key, "selector")) {
        if (!parse_float(val, &p->bar_w)) { perr(ps, "selector needs a width", NULL); return 0; }
        return 1;
    }
    if (!_stricmp(key, "item_size")) {
        if (!parse_float(val, &p->item_size)) { perr(ps, "bad item_size", NULL); return 0; }
        return 1;
    }
    if (!_stricmp(key, "focus")) {
        float v;
        if (!parse_float(val, &v) || v < 1) { perr(ps, "focus is an item number from 1", NULL); return 0; }
        p->focus = (int)v - 1;
        return 1;
    }
    if (!_stricmp(key, "values")) {
        if (!parse_float(val, &p->values_x)) { perr(ps, "values needs the x their right edge lines up on", NULL); return 0; }
        return 1;
    }
    if (!_stricmp(key, "line")) {
        float r[3];
        if (p->nsep >= 4) { perr(ps, "at most %s lines on a page", "4"); return 0; }
        if (!parse_floats(val, r, 3)) { perr(ps, "line needs: X1 Y X2", NULL); return 0; }
        memcpy(p->sep[p->nsep++], r, sizeof(r));
        return 1;
    }
    if (!_stricmp(key, "highlight")) {
        if (!_stricmp(val, "off")) p->plain_focus = 1;
        else if (!_stricmp(val, "on")) p->plain_focus = 0;
        else { perr(ps, "highlight is on or off", NULL); return 0; }
        return 1;
    }
    if (!_stricmp(key, "back")) return parse_script(ps, val, &p->back);
    if (!_stricmp(key, "loading")) { copy(p->loading, PCM_NAME, val); return 1; }
    if (!_stricmp(key, "require")) {
        char *w[3];
        int k = split(val, ' ', w, 3), j;
        p->nrequire = 0;
        for (j = 0; j < k && p->nrequire < 2; j++) if (*w[j]) copy(p->require[p->nrequire++], PCM_NAME, w[j][0] == '@' ? w[j] + 1 : w[j]);
        return 1;
    }
    if (!_stricmp(key, "prompts")) {
        char *w[4];
        int k = split(val, ' ', w, 4), j;
        p->prompts = 0;
        for (j = 0; j < k; j++) {
            if (!_stricmp(w[j], "select")) p->prompts |= 1;
            else if (!_stricmp(w[j], "confirm")) p->prompts |= 2;
            else if (!_stricmp(w[j], "back")) p->prompts |= 4;
            else if (!_stricmp(w[j], "none") || !*w[j]) {}
            else { perr(ps, "unknown prompt '%s' (select, confirm, back, none)", w[j]); return 0; }
        }
        return 1;
    }
    if (!_stricmp(key, "arrows")) { p->four_way = !_stricmp(val, "4"); return 1; }
    if (!_stricmp(key, "start")) {
        if (!_stricmp(val, "back")) p->start_back = 1;
        else if (!_stricmp(val, "confirm")) p->start_back = 0;
        else { perr(ps, "start is back or confirm", NULL); return 0; }
        return 1;
    }
    perr(ps, "unknown page setting '%s'", key);
    return 0;
}

static int parse_style_key(Parse *ps, Style *s, const char *key, const char *val)
{
    static const struct { const char *name; size_t off; } colours[] = {
        { "shade", offsetof(Style, shade) }, { "column", offsetof(Style, column) }, { "rail", offsetof(Style, rail) },
        { "item", offsetof(Style, item) }, { "focus", offsetof(Style, focus) }, { "label", offsetof(Style, label) },
        { "text", offsetof(Style, text) }, { "title", offsetof(Style, title) }, { "prompt", offsetof(Style, prompt) },
        { "selector", offsetof(Style, bar) }, { "line", offsetof(Style, line) }, { "choice", offsetof(Style, choice) },
        { "choice_on", offsetof(Style, choice_on) }, { "bullet", offsetof(Style, bullet) }, { "bullet_on", offsetof(Style, bullet_on) },
        { "logo", offsetof(Style, logo) }, { "dim", offsetof(Style, dim) }, { "disabled", offsetof(Style, disabled) },
    };
    static const struct { const char *name; size_t off; } numbers[] = {
        { "title_size", offsetof(Style, title_size) }, { "item_size", offsetof(Style, item_size) },
        { "text_size", offsetof(Style, text_size) }, { "focus_scale", offsetof(Style, focus_scale) },
        { "logo_width", offsetof(Style, logo_w) }, { "intro", offsetof(Style, intro) }, { "animation", offsetof(Style, anim) },
    };
    size_t i;
    for (i = 0; i < sizeof(colours) / sizeof(colours[0]); i++)
        if (!_stricmp(key, colours[i].name)) {
            if (!parse_colour(val, (uint32_t *)((char *)s + colours[i].off))) { perr(ps, "bad colour '%s' (RRGGBB or AARRGGBB)", val); return 0; }
            return 1;
        }
    for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
        if (!_stricmp(key, numbers[i].name)) {
            if (!parse_float(val, (float *)((char *)s + numbers[i].off))) { perr(ps, "bad number '%s'", val); return 0; }
            return 1;
        }
    if (!_stricmp(key, "sounds")) { s->sounds = !_stricmp(val, "on") || !strcmp(val, "1"); return 1; }
    perr(ps, "unknown style setting '%s'", key);
    return 0;
}

/* Parses `text` into doc. A page with an error is dropped (the game shows its own); returns the page count. */
static int parse_doc(const char *text, Doc *doc, char *log, size_t log_cap)
{
    Parse ps = { log, log_cap, 0, 0, 0 };
    char *buf, *line, *next;
    int section = 0;                       /* 0 none, 1 style, 2 page */
    int page_bad = 0;
    Page *cur = NULL;
    if (log && log_cap) log[0] = 0;
    memset(doc, 0, sizeof(*doc));
    style_defaults(&doc->style);
    if (!text) return 0;
    buf = _strdup(text);
    if (!buf) return 0;
    for (line = buf; line; line = next) {
        char *eq, *key, *val;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        ps.line++;
        line = trim(line);
        if (!*line || *line == ';' || *line == '#') continue;
        if (*line == '[') {
            char *end = strchr(line, ']');
            if (cur && page_bad) doc->npage--;   /* drop the previous page */
            cur = NULL; page_bad = 0;
            if (!end) { perr(&ps, "missing ']'", NULL); section = 0; continue; }
            *end = 0;
            line = trim(line + 1);
            if (!_stricmp(line, "style")) { section = 1; continue; }
            if (!_strnicmp(line, "page ", 5) && *trim(line + 5)) {
                char *name = trim(line + 5);
                int j;
                section = 2;
                for (j = 0; j < doc->npage; j++)
                    if (!_stricmp(doc->page[j].name, name)) break;
                if (j < doc->npage) { perr(&ps, "page '%s' is listed twice", name); section = 0; continue; }
                if (doc->npage >= PCM_PAGES) { perr(&ps, "too many pages", NULL); section = 0; continue; }
                cur = &doc->page[doc->npage++];
                page_defaults(cur);
                copy(cur->name, PCM_NAME, name);
                cur->line = ps.line;
                continue;
            }
            perr(&ps, "unknown section [%s] (style, page NAME)", line);
            section = 0;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) { perr(&ps, "expected 'name = value': %s", line); if (cur) page_bad = 1; continue; }
        *eq = 0;
        key = trim(line);
        val = trim(eq + 1);
        {   /* a ';' after the value starts a comment, unless it is inside quotes or part of an action list */
            int q = 0;
            char *c;
            for (c = val; *c; c++) {
                if (*c == '"') q = !q;
                if (!q && *c == ';' && (c == val || c[-1] == ' ' || c[-1] == '\t') && (c[1] == ' ' || c[1] == 0) &&
                    strncmp(key, "item", 4) && strncmp(key, "back", 4) && strncmp(key, "toggle", 6) && strncmp(key, "slider", 6) && strncmp(key, "setting", 7) &&
                    strncmp(key, "keybind", 7) && strncmp(key, "padbind", 7) && strncmp(key, "padlayout", 9) &&
                    strncmp(key, "photo", 5)) {
                    *c = 0; val = trim(val); break;
                }
            }
        }
        if (section == 1) parse_style_key(&ps, &doc->style, key, val);
        else if (section == 2 && cur) { if (!parse_page_key(&ps, cur, key, val)) page_bad = 1; }
        else perr(&ps, "setting outside a section: %s", key);
    }
    if (cur && page_bad) doc->npage--;
    {   /* a page with no items cannot take input; keep it only when it says so on purpose (prompts none) */
        int j;
        for (j = 0; j < doc->npage; j++) {
            Page *p = &doc->page[j];
            if (p->focus >= p->nitem) p->focus = p->nitem ? p->nitem - 1 : 0;
        }
    }
    free(buf);
    return doc->npage;
}

int pc_menu_check_definition(const char *text, char *log, size_t log_cap)
{
    Doc *d = (Doc *)malloc(sizeof(Doc));
    int n;
    if (!d) return 0;
    n = parse_doc(text, d, log, log_cap);
    free(d);
    return n;
}

/* ---- assets ------------------------------------------------------------------------------------------------- */

typedef struct Font {
    uint32_t *pixels;                    /* premultiplied */
    unsigned w, h;
    float nominal, cap_oy;
    PcMenuGlyph *glyphs;
    unsigned count;
    uint16_t *map;                       /* code -> glyph index + 1 */
} Font;
typedef struct Image { uint32_t *pixels; unsigned w, h; } Image;

static Font m_font[PCM_FONT_COUNT];
static volatile LONG m_font_ready;
static Image m_image[PCM_IMAGE_COUNT];

static uint32_t premultiply(uint32_t c)
{
    const unsigned a = c >> 24, r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

int pc_menu_set_font(int slot, const PcMenuFont *f)
{
    Font nf;
    unsigned i;
    if (slot < 0 || slot >= PCM_FONT_COUNT || !f || !f->pixels || !f->width || !f->height || !f->glyphs || !f->count ||
        !(f->nominal > 0)) return 0;
    memset(&nf, 0, sizeof(nf));
    nf.pixels = (uint32_t *)malloc((size_t)f->width * f->height * 4);
    nf.glyphs = (PcMenuGlyph *)malloc(sizeof(PcMenuGlyph) * f->count);
    nf.map = (uint16_t *)calloc(65536, sizeof(uint16_t));
    if (!nf.pixels || !nf.glyphs || !nf.map) { free(nf.pixels); free(nf.glyphs); free(nf.map); return 0; }
    for (i = 0; i < f->width * f->height; i++) nf.pixels[i] = premultiply(f->pixels[i]);
    memcpy(nf.glyphs, f->glyphs, sizeof(PcMenuGlyph) * f->count);
    nf.w = f->width; nf.h = f->height; nf.nominal = f->nominal; nf.count = f->count;
    for (i = 0; i < f->count && i < 65535; i++)
        if (f->glyphs[i].code && !nf.map[f->glyphs[i].code]) nf.map[f->glyphs[i].code] = (uint16_t)(i + 1);
    nf.cap_oy = nf.map['A'] ? nf.glyphs[nf.map['A'] - 1].oy : 0.0f;
    free(m_font[slot].pixels); free(m_font[slot].glyphs); free(m_font[slot].map);
    m_font[slot] = nf;
    InterlockedOr(&m_font_ready, 1L << slot);
    return 1;
}

int pc_menu_has_font(int slot)
{
    return slot >= 0 && slot < PCM_FONT_COUNT && (InterlockedCompareExchange(&m_font_ready, 0, 0) & (1L << slot)) != 0;
}

int pc_menu_set_image(int slot, const uint32_t *pixels, unsigned w, unsigned h)
{
    uint32_t *p;
    unsigned i;
    if (slot < 0 || slot >= PCM_IMAGE_COUNT || !pixels || !w || !h) return 0;
    p = (uint32_t *)malloc((size_t)w * h * 4);
    if (!p) return 0;
    for (i = 0; i < w * h; i++) p[i] = premultiply(pixels[i]);
    free(m_image[slot].pixels);
    m_image[slot].pixels = p; m_image[slot].w = w; m_image[slot].h = h;
    return 1;
}

static const PcMenuGlyph *glyph(const Font *f, uint16_t c)
{
    unsigned i;
    if (!f->map) return NULL;
    i = f->map[c];
    if (!i && c >= 'a' && c <= 'z') i = f->map[c - 32];
    if (!i && c >= 0xE0 && c <= 0xFE && c != 0xF7) i = f->map[c - 32];
    /* The retail font omits A-tilde. Retain the letter if an older atlas
     * without the composed Portuguese glyph is registered. */
    if (!i && (c == 0xC3 || c == 0xE3)) i = f->map['A'];
    return i ? &f->glyphs[i - 1] : NULL;
}

/* Where a glyph's cell lands, relative to a text prim at the given size: x, y (from the prim's top of capitals), w, h
 * in menu units. 0 when the font has no such glyph. */
static int glyph_box(int font, uint16_t code, float size, float box[4])
{
    const Font *f = &m_font[font];
    const PcMenuGlyph *g = f->pixels ? glyph(f, code) : NULL;
    const float u = f->nominal > 0 ? size / f->nominal : 0;
    if (!g || u <= 0) return 0;
    box[0] = g->ox * u; box[1] = (g->oy - f->cap_oy) * u; box[2] = g->w * u; box[3] = g->h * u;
    return 1;
}

/* Width of text in menu units at the given size. */
static float text_width(int font, const uint16_t *t, float size)
{
    const Font *f = &m_font[font];
    float w = 0;
    if (!f->pixels) return 0;
    for (; *t; t++) {
        const PcMenuGlyph *g = glyph(f, *t);
        if (g) w += g->advance;
    }
    return w * size / f->nominal;
}

/* ---- the display list ------------------------------------------------------------------------------------- */

enum { PR_RECT, PR_TEXT, PR_IMAGE, PR_TRI };
enum { PF_EXT_LEFT = 1, PF_EXT_RIGHT = 2, PF_EXT_UP = 4, PF_EXT_DOWN = 8, PF_STORE = 16 };
typedef struct Prim {
    uint8_t type, font, flags, image;
    float x, y, w, h, size, scale;       /* menu units; text: (x, y) the top of the capitals at the left;
                                          * a triangle's corners are (x, y), (w, h), (size, scale) */
    uint32_t colour;
    uint16_t text[PCM_TEXT_CAP];
} Prim;

#define PCM_PRIMS 400                 /* the rails alone are about a hundred (ruler ticks) */
typedef struct Display { int n; float slide; Prim prim[PCM_PRIMS]; } Display;

/* ---- runtime state ---------------------------------------------------------------------------------------- */

static SRWLOCK m_lock = SRWLOCK_INIT;
static const char *m_default_text;
static Doc *m_doc;
static char m_path[512];
static FILETIME m_mtime;
static int m_loaded, m_disabled = -1;

static int m_page = -1;                  /* index in m_doc, -1 = none */
static char m_page_name[PCM_NAME];
/* Native subpages retain each caller's focus. -1 is the game's original APT page. */
static struct { int page, focus; } m_stack[8];
static int m_depth;
static int m_focus = -1, m_prev_focus = -1;
/* The Controls pages: the keyboard column in focus (0 primary, 1 secondary), and a capture in progress -- the next
 * key, mouse button or pad button goes to the focused row. The input threads read m_capture to route their events. */
enum { CAP_KEY = 1, CAP_PAD = 2 };
static int m_bind_col;
static volatile LONG m_capture;
static double m_capture_t0;
/* Actions the input threads queue besides PCM_ACT_*: clear the focused binding, cancel a capture, and what a capture
 * caught (ACT_KEY | a pc_input source, ACT_PAD | a control). */
enum { ACT_CLEAR = 32, ACT_CANCEL, ACT_KEY = 0x10000, ACT_PAD = 0x20000, ACT_KIND = 0x30000 };
#define PCM_CAPTURE_SECONDS 8.0
static double m_focus_t0, m_page_t0;
static int m_slider[PCM_ITEMS];          /* the sliders' own values, as BkMovieclipSlider keeps them */
static char m_var_name[PCM_VARS][PCM_NAME], m_var_val[PCM_VARS][PCM_NAME];
static int m_queue[32], m_qhead, m_qtail;
static volatile LONG m_active;
static ULONGLONG m_active_tick;
static Display m_display, m_scratch;     /* m_display under m_lock; m_scratch is the game thread's */
/* Where each item of the shown page is, for the pointer: in menu units, written by build (game thread) into
 * m_hit_scratch and published with the display list. kind is IT_*, split the x where a click changes meaning
 * (OFF of a toggle, the lit end of a slider), or 0. m_xf maps menu units to client pixels (presenter thread). */
typedef struct Hit { float x0, y0, x1, y1, split; int kind, ok; } Hit;
static Hit m_hit[PCM_ITEMS], m_hit_scratch[PCM_ITEMS];
static int m_hit_n, m_hit_scratch_n;
static float m_xf_ox, m_xf_oy, m_xf_s;
static int m_pointer_item = -1, m_pointer_known;
static int m_pointer_x, m_pointer_y;     /* saved client pixels, also valid after a page change or resize */
static float m_pointer_mx;
#define PCM_ACT_HOVER 64                 /* + item index: the pointer moved onto that item */
static unsigned m_generation;
/* The presenter asks for the overlay every frame it shows. A native page needs it (it is what draws the page and
 * installs the input filter): without it -- the game started without the presenter window -- pages stay the game's. */
static volatile ULONGLONG m_overlay_seen;
static int m_overlay_assumed;              /* tests: no presenter */
static volatile LONG m_input_seen;         /* the player used a page since pc_menu_take_input last asked */
static volatile LONG m_movie_skip_prompt;
static const uint64_t *m_replaced;         /* earlier built-in definitions (FNV-1a 64 of the file) */
static int m_nreplaced;

/* m_lock held: clicks always use the currently published rows and window mapping. */
static void pointer_hit_locked(void)
{
    float my;
    int i;
    m_pointer_item = -1;
    if (!m_pointer_known || m_xf_s <= 0) return;
    m_pointer_mx = ((float)m_pointer_x - m_xf_ox) / m_xf_s;
    my = ((float)m_pointer_y - m_xf_oy) / m_xf_s;
    for (i = 0; i < m_hit_n; i++) {
        const Hit *h = &m_hit[i];
        if (h->ok && m_pointer_mx >= h->x0 && m_pointer_mx < h->x1 && my >= h->y0 && my < h->y1) {
            m_pointer_item = i;
            break;
        }
    }
}

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

void pc_menu_set_default_definition(const char *text) { m_default_text = text; }
void pc_menu_set_replaced_defaults(const uint64_t *fnv1a, int count) { m_replaced = fnv1a; m_nreplaced = count; }
void pc_menu_set_movie_skip_prompt(int visible) { InterlockedExchange(&m_movie_skip_prompt, visible ? 1 : 0); }

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001B3ull; }
    return h;
}

const char *pc_menu_definition_path(void)
{
    if (!m_path[0]) {
        const char *e = getenv("RECOMP_MENUS_INI");
        snprintf(m_path, sizeof(m_path), "%s", e && *e ? e : "local\\menus.ini");
    }
    return m_path;
}

static int disabled(void)
{
    if (m_disabled < 0) {
        const char *e = getenv("RECOMP_NATIVE_MENUS");
        m_disabled = e && *e == '0';
        if (m_disabled) fprintf(stderr, "[MENU] RECOMP_NATIVE_MENUS=0: the game's own menus\n");
    }
    return m_disabled;
}

static char *read_file(const char *path, FILETIME *mtime)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    char *buf;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 1024 * 1024) { CloseHandle(h); return NULL; }
    buf = (char *)malloc((size_t)size.QuadPart + 1);
    if (buf && !ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL)) got = 0;
    if (mtime) GetFileTime(h, NULL, NULL, mtime);
    CloseHandle(h);
    if (!buf) return NULL;
    buf[got] = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        memmove(buf, buf + 3, got - 2);
    return buf;
}

static void write_default_file(const char *path, int replace)
{
    char dir[512], tmp[540], *slash;
    FILE *f;
    if (!m_default_text) return;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) { *slash = 0; CreateDirectoryA(dir, NULL); }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(m_default_text, 1, strlen(m_default_text), f);
    if (fclose(f) == 0 && MoveFileExA(tmp, path, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
        fprintf(stderr, "[MENU] wrote the default menu definition to %s\n", path);
    else DeleteFileA(tmp);
}

/* Loads (or reloads when the file changed) the definition. Game thread. */
static void load_doc(int check_changed)
{
    const char *path = pc_menu_definition_path();
    FILETIME mt = { 0, 0 };
    char *text, log[2048];
    Doc *d;
    int n;
    if (m_loaded && check_changed) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return;   /* deleted: keep what we have */
        if (!CompareFileTime(&a.ftLastWriteTime, &m_mtime)) return;
    } else if (m_loaded) return;
    text = read_file(path, &mt);
    if (!text && !m_loaded) {
        write_default_file(path, 0);
        text = read_file(path, &mt);
    }
    if (text && m_default_text && strcmp(text, m_default_text)) {
        /* the default of an earlier build, never edited: nothing of the player's is lost by bringing it up to date */
        const uint64_t h = fnv1a(text);
        int i;
        for (i = 0; i < m_nreplaced; i++)
            if (m_replaced[i] == h) {
                fprintf(stderr, "[MENU] %s is an earlier build's default, never edited: replaced with this build's\n", path);
                write_default_file(path, 1);
                free(text);
                text = read_file(path, &mt);
                break;
            }
    }
    d = (Doc *)malloc(sizeof(Doc));
    if (!d) { free(text); return; }
    n = parse_doc(text ? text : m_default_text, d, log, sizeof(log));
    if (log[0]) fprintf(stderr, "[MENU] %s problems (those pages are left to the game):\n%s", path, log);
    /* Edited definitions keep their pages and style. New PC settings remain available even when an older
     * custom file predates them; the supplemental pages are in memory only, never written over the file. */
    if (text && m_default_text && strcmp(text, m_default_text)) {
        Doc *defaults = (Doc *)malloc(sizeof(Doc));
        if (defaults) {
            int i, j;
            parse_doc(m_default_text, defaults, NULL, 0);
            for (i = 0; i < defaults->npage && d->npage < PCM_PAGES; i++) {
                const Page *p = &defaults->page[i];
                if (_stricmp(p->name, "VideoSettings") && _stricmp(p->name, "VideoDisplay") && _stricmp(p->name, "VideoGraphics") &&
                    _strnicmp(p->name, "Controls", 8) && _stricmp(p->name, "PhotoMode") && _stricmp(p->name, "GameplaySettings")) continue;
                for (j = 0; j < d->npage; j++) if (!_stricmp(p->name, d->page[j].name)) break;
                if (j == d->npage) d->page[d->npage++] = *p;
            }
            free(defaults);
            n = d->npage;
        }
    }
    fprintf(stderr, "[MENU] %d native page(s) from %s\n", n, text ? path : "the built-in definition");
    free(text);
    AcquireSRWLockExclusive(&m_lock);
    free(m_doc);
    m_doc = d;
    m_mtime = mt;
    m_loaded = 1;
    ReleaseSRWLockExclusive(&m_lock);
}

static const char *var_get(const char *name)
{
    int i;
    for (i = 0; i < PCM_VARS; i++) if (m_var_name[i][0] && !_stricmp(m_var_name[i], name)) return m_var_val[i];
    return NULL;
}

static void var_set(const char *name, const char *value)
{
    int i, free_slot = -1;
    for (i = 0; i < PCM_VARS; i++) {
        if (m_var_name[i][0] && !_stricmp(m_var_name[i], name)) { copy(m_var_val[i], PCM_NAME, value); return; }
        if (!m_var_name[i][0] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    copy(m_var_name[free_slot], PCM_NAME, name);
    copy(m_var_val[free_slot], PCM_NAME, value);
}

static void vars_clear(void) { memset(m_var_name, 0, sizeof(m_var_name)); memset(m_var_val, 0, sizeof(m_var_val)); }

/* A menu variable: what a native page set, else what the game's own pages keep under that name. */
static const char *var_lookup(const PcMenuHost *host, const char *name)
{
    static char buf[4][PCM_NAME];
    static int next;
    const char *v = var_get(name);
    if (v) return v;
    if (host && host->get_var) {
        char *b = buf[next++ & 3];
        b[0] = 0;
        if (host->get_var(host->ctx, name, b, PCM_NAME) && b[0]) return b;
    }
    return NULL;
}

/* "@name" reads a menu variable; anything else is itself. */
static const char *resolve(const PcMenuHost *host, const char *s)
{
    if (s[0] == '@') { const char *v = var_lookup(host, s + 1); return v ? v : ""; }
    return s;
}

/* "NAME" is true when the game's number is non-zero, "NAME=N" when it is N; "!" in front turns it round.
 * "@NAME" is a menu variable (set by an earlier page). */
static int cond_true(const PcMenuHost *host, const char *cond)
{
    int v = 0, neg = cond[0] == '!', is;
    char name[PCM_NAME];
    const char *eq;
    if (!cond[0]) return 1;
    copy(name, PCM_NAME, cond + neg);
    eq = strchr(cond + neg, '=');
    if (eq) name[eq - (cond + neg)] = 0;
    if (name[0] == '@') {
        const char *s = var_lookup(host, name + 1);
        is = eq ? (s && !strcmp(s, eq + 1)) : (s && s[0] && strcmp(s, "0"));
    } else {
        if (!host->value || !host->value(host->ctx, name, &v)) v = 0;
        is = eq ? v == atoi(eq + 1) : v != 0;
    }
    return neg ? !is : is;
}

static int item_visible(const PcMenuHost *host, const Item *it) { return cond_true(host, it->cond); }
static int item_disabled(const PcMenuHost *host, const Item *it) { return it->off[0] && cond_true(host, it->off); }
static int item_focusable(const PcMenuHost *host, const Item *it) { return item_visible(host, it) && !item_disabled(host, it); }

static void play(const PcMenuHost *host, const char *clip)
{
    if (m_doc && m_doc->style.sounds && host->sound) host->sound(host->ctx, clip);
}

static int open_page(const PcMenuHost *host, const char *name);
static int close_page(const PcMenuHost *host);

/* Runs a script; returns 1 when something happened (the original's PressedConfirm / PressedBack). */
static int run(const PcMenuHost *host, const Page *p, const Script *s, int force, int depth)
{
    int i, did = 0;
    for (i = 0; i < s->n; i++) {
        const Cmd *c = &s->cmd[i];
        switch (c->op) {
        case OP_GAMECODE: {
            const char *a[3] = { NULL, NULL, NULL };
            int j;
            char name[PCM_NAME];
            copy(name, PCM_NAME, resolve(host, c->arg[0]));
            for (j = 1; j < c->nargs && j < 4; j++) a[j - 1] = resolve(host, c->arg[j]);
            if (name[0] && host->gamecode && host->gamecode(host->ctx, name, a, force)) did = 1;
            break;
        }
        case OP_PAGE: {
            const char *name = resolve(host, c->arg[0]);
            if (name[0] && host->page) { host->page(host->ctx, name); did = 1; }
            break;
        }
        case OP_SET: {
            const char *v = resolve(host, c->arg[1]);
            var_set(c->arg[0], v);
            if (host->set_var) host->set_var(host->ctx, c->arg[0], v);
            break;
        }
        case OP_VIDEO:
            if (open_page(host, "VideoSettings")) did = 1;
            else if (host->video) { host->video(host->ctx); did = 1; }
            break;
        case OP_RESET_VIDEO: pc_video_menu_reset_settings(); did = 1; break;
        case OP_RESET_KEYS: pc_controls_key_reset(); did = 1; break;
        case OP_RESET_PAD: pc_controls_pad_reset(); did = 1; break;
        case OP_PHOTO_SHOT: did = pc_photo_take(); break;
        case OP_PHOTO_RESET: pc_photo_reset(); did = 1; break;
        case OP_PHOTO_HIDE: pc_photo_hide_panel(); did = 1; break;
        case OP_BACK: if (depth < 2) did |= run(host, p, &p->back, 1, depth + 1); break;
        case OP_OPEN: did |= open_page(host, resolve(host, c->arg[0])); break;
        case OP_CLOSE: did |= close_page(host); break;
        case OP_EXIT: if (host->quit) { host->quit(host->ctx); did = 1; } break;
        default: break;
        }
    }
    return did;
}

static int script_has(const Script *s, int op)
{
    int i;
    for (i = 0; i < s->n; i++) if (s->cmd[i].op == op) return 1;
    return 0;
}

static void set_focus(const PcMenuHost *host, const Page *p, int target, double t)
{
    m_prev_focus = m_focus;
    m_focus = target;
    m_focus_t0 = t;
    if (target >= 0 && p->item[target].on_focus.n) run(host, p, &p->item[target].on_focus, 1, 0);
}

static int step_focus(const PcMenuHost *host, const Page *p, int from, int dir)
{
    int i, k = from;
    for (i = 0; i < p->nitem; i++) {
        k = (k + dir + p->nitem) % p->nitem;
        if (item_focusable(host, &p->item[k])) return k;
    }
    return from;
}

static int read_value(const PcMenuHost *host, const char *name, int *out)
{
    return host->value && host->value(host->ctx, name, out);
}

static int find_page(const char *name);

/* Shows page idx of the file: the focus is chosen on the next tick, the sliders start at the game's values. */
static void page_enter(const PcMenuHost *host, int idx, double t)
{
    const Page *p = &m_doc->page[idx];
    int i;
    m_page = idx;
    m_focus = m_prev_focus = -1;
    m_page_t0 = t;
    InterlockedExchange(&m_capture, 0);
    for (i = 0; i < p->nitem; i++) {
        int v = 0;
        m_slider[i] = p->item[i].kind == IT_SLIDER && read_value(host, p->item[i].var, &v) ? v : 0;
    }
}

/* "open NAME": a page of the file that the game does not have, shown over the game's page (which stays where it is,
 * hidden). "close" goes back to the game page's own native page, with its focus where it was. */
static int open_page(const PcMenuHost *host, const char *name)
{
    const int idx = find_page(name);
    int i;
    if (idx < 0 || idx == m_page) { fprintf(stderr, "[MENU] open %s: not a page of the file\n", name); return 0; }
    if (m_doc->page[idx].photo && !pc_photo_allowed()) { fprintf(stderr, "[MENU] open %s: photo mode is not available now\n", name); return 0; }
    for (i = 0; i < m_doc->page[idx].nrequire; i++) if (!var_lookup(host, m_doc->page[idx].require[i])) return 0;
    if (m_depth >= (int)(sizeof(m_stack) / sizeof(m_stack[0]))) return 0;
    m_stack[m_depth].page = m_page; m_stack[m_depth].focus = m_focus; m_depth++;
    page_enter(host, idx, now_s());
    play(host, "SNDOpen");                  /* every original page plays it as it opens (Prepare) */
    fprintf(stderr, "[MENU] page %s: native (%d items), over %s\n", m_doc->page[idx].name, m_doc->page[idx].nitem, m_page_name);
    return 1;
}

static int close_page(const PcMenuHost *host)
{
    const double t = now_s();
    if (!m_depth) return 0;
    m_depth--;
    if (m_stack[m_depth].page >= 0) page_enter(host, m_stack[m_depth].page, t);
    else m_page = -1;
    m_focus = m_prev_focus = m_stack[m_depth].focus;
    m_focus_t0 = t - 10;
    play(host, "SNDOpen");                  /* as the page would, shown again */
    fprintf(stderr, "[MENU] page %s: %s again\n", m_page >= 0 ? m_doc->page[m_page].name : m_page_name,
        m_page >= 0 ? "native" : "the game's own");
    return 1;
}

static void apply(const PcMenuHost *host, const Page *p, int action, double t)
{
    const Item *it = m_focus >= 0 && m_focus < p->nitem ? &p->item[m_focus] : NULL;
    if (InterlockedCompareExchange(&m_capture, 0, 0)) {
        /* waiting for a key or button: only what was caught, or a cancel, does anything */
        const LONG cap = m_capture;
        int ok = -1;
        if (action == ACT_CANCEL) { InterlockedExchange(&m_capture, 0); play(host, "SNDBack"); return; }
        if (it && cap == CAP_KEY && it->kind == IT_KEY && (action & ACT_KIND) == ACT_KEY)
            ok = pc_controls_key_bind((PcAction)it->action, m_bind_col, (unsigned)(action & 0xFFFF));
        else if (it && cap == CAP_PAD && it->kind == IT_PAD && (action & ACT_KIND) == ACT_PAD)
            ok = pc_controls_pad_bind((PcAction)it->action, action & 0xFFFF);
        if (ok == 1) InterlockedExchange(&m_capture, 0);
        if (ok >= 0) play(host, ok ? "SNDSelect" : "SNDError");
        return;
    }
    if (action & ACT_KIND) return;
    if (action == ACT_CLEAR) {
        int did = 0;
        if (it && it->kind == IT_KEY) did = pc_input_binding((PcAction)it->action, m_bind_col) &&
                                            pc_controls_key_bind((PcAction)it->action, m_bind_col, 0);
        else if (it && it->kind == IT_PAD) did = pc_controls_pad_bind((PcAction)it->action, -1);
        if (it && (it->kind == IT_KEY || it->kind == IT_PAD)) play(host, did ? "SNDChange" : "SNDError");
        return;
    }
    if (p->photo && pc_photo_panel_put_away() && action != PCM_ACT_BACK && action != PCM_ACT_START) {
        pc_photo_show_panel();
        play(host, "SNDChange");
        return;
    }
    if (action == PCM_ACT_START) action = p->start_back ? PCM_ACT_BACK : PCM_ACT_CONFIRM;
    switch (action) {
    case PCM_ACT_UP: case PCM_ACT_DOWN: {
        int target;
        if (!p->nitem) break;
        target = m_focus < 0 ? p->focus : step_focus(host, p, m_focus, action == PCM_ACT_UP ? -1 : 1);
        if (target != m_focus) { play(host, "SNDChange"); set_focus(host, p, target, t); }
        break;
    }
    case PCM_ACT_LEFT: case PCM_ACT_RIGHT:
        if (!it) break;
        if (it->kind == IT_VIDEO) {
            const int did = pc_video_menu_row_change(it->video_row, action == PCM_ACT_LEFT ? -1 : 1);
            play(host, did ? "SNDChange" : "SNDError");
        } else if (it->kind == IT_KEY) {
            const int col = action == PCM_ACT_LEFT ? 0 : 1;
            if (col != m_bind_col) { m_bind_col = col; play(host, "SNDChange"); }
        } else if (it->kind == IT_LAYOUT) {
            play(host, pc_controls_pad_layout_step(action == PCM_ACT_LEFT ? -1 : 1) ? "SNDChange" : "SNDError");
        } else if (it->kind == IT_PHOTO) {
            play(host, pc_photo_row_change(it->video_row, action == PCM_ACT_LEFT ? -1 : 1) ? "SNDChange" : "SNDError");
        } else if (it->kind == IT_TOGGLE) {
            int cur = 0, want = action == PCM_ACT_LEFT ? 1 : 0;      /* ON is on the left, OFF on the right */
            read_value(host, it->var, &cur);
            if ((cur != 0) != want) {
                char v[8];
                const char *a[3];
                snprintf(v, sizeof(v), "%d", want);
                a[0] = v; a[1] = "0"; a[2] = NULL;
                /* the original plays SNDValueChange here, which the in-game options page does not have: silence */
                if (host->gamecode) host->gamecode(host->ctx, it->call, a, 1);
            }
        } else if (it->kind == IT_SLIDER) {
            int *v = &m_slider[m_focus], nv = *v + (action == PCM_ACT_LEFT ? -it->step : it->step);
            if (nv > it->hi) nv = it->hi;
            if (nv < it->lo) nv = it->lo;
            if (nv != *v) {
                char s[16];
                const char *a[3];
                *v = nv;
                snprintf(s, sizeof(s), "%d", nv);
                a[0] = s; a[1] = "0"; a[2] = NULL;
                if (host->gamecode) host->gamecode(host->ctx, it->call, a, 1);
            }
        }
        break;
    case PCM_ACT_CONFIRM: {
        int did = 0;
        if (it && it->kind == IT_VIDEO) did = pc_video_menu_row_change(it->video_row, 1);
        else if (it && it->kind == IT_LAYOUT) did = pc_controls_pad_layout_step(1);
        else if (it && (it->kind == IT_KEY || it->kind == IT_PAD)) {
            m_capture_t0 = t;
            InterlockedExchange(&m_capture, it->kind == IT_KEY ? CAP_KEY : CAP_PAD);
            did = 1;
        }
        else if (it && it->kind == IT_PHOTO) did = pc_photo_row_change(it->video_row, 1);
        else if (it && it->kind == IT_BUTTON && it->act.n) did = run(host, p, &it->act, 0, 0);
        else if (script_has(&p->back, OP_PAGE)) did = run(host, p, &p->back, 1, 0);   /* the original's "Going Backwards 2" */
        play(host, did ? "SNDSelect" : "SNDError");
        break;
    }
    case PCM_ACT_BACK: {
        const int did = run(host, p, &p->back, 1, 0);
        play(host, did ? "SNDBack" : "SNDError");
        break;
    }
    default: break;
    }
}

/* ---- building the display list (game thread) ---------------------------------------------------------------- */

static Prim *add(Display *d, int type)
{
    Prim *p;
    if (d->n >= PCM_PRIMS) return NULL;
    p = &d->prim[d->n++];
    memset(p, 0, sizeof(*p));               /* whole: the lists are compared byte for byte */
    p->type = (uint8_t)type;
    p->scale = 1.0f;
    return p;
}

static void rect(Display *d, float x, float y, float w, float h, uint32_t c, unsigned flags)
{
    Prim *p = add(d, PR_RECT);
    if (!p) return;
    p->x = x; p->y = y; p->w = w; p->h = h; p->colour = c; p->flags = (uint8_t)flags;
}

static void tri(Display *d, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c)
{
    Prim *p = add(d, PR_TRI);
    if (!p) return;
    p->x = x1; p->y = y1; p->w = x2; p->h = y2; p->size = x3; p->scale = y3; p->colour = c;
}

static void utf8_to_utf16(const char *s, uint16_t *out, unsigned cap)
{
    unsigned n = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n + 1 < cap) {
        unsigned c = *u++;
        if (c >= 0xC0 && c < 0xE0 && (*u & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*u++ & 0x3F);
        else if (c >= 0xE0 && c < 0xF0 && (u[0] & 0xC0) == 0x80 && (u[1] & 0xC0) == 0x80) {
            c = ((c & 0x0F) << 12) | ((u[0] & 0x3F) << 6) | (u[1] & 0x3F); u += 2;
        }
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
}

static int pc_menu_locale(void)
{
    const char *language = getenv("RECOMP_LANGUAGE");
    if (!language || !_stricmp(language, "en-US")) return 0;
    if (!_stricmp(language, "pt-BR")) return 1;
    if (!_stricmp(language, "es")) return 2;
    if (!_stricmp(language, "fr")) return 3;
    if (!_stricmp(language, "de")) return 4;
    if (!_stricmp(language, "it")) return 5;
    return 0;
}

/* The Controls pages' own words. Portuguese is reviewed; the other languages are machine drafts, like the launcher's. */
static const char *controls_text(const char *text)
{
    static const struct { const char *english, *pt, *es, *fr, *de, *it; } words[] = {
        { "CONTROLS", "CONTROLES", "CONTROLES", "COMMANDES", "STEUERUNG", "COMANDI" },
        { "KEYBOARD AND MOUSE", "TECLADO E MOUSE", "TECLADO Y RATÓN", "CLAVIER ET SOURIS", "TASTATUR UND MAUS", "TASTIERA E MOUSE" },
        { "CONTROLLER", "CONTROLE", "MANDO", "MANETTE", "CONTROLLER", "CONTROLLER" },
        { "MOVEMENT", "MOVIMENTO", "MOVIMIENTO", "DÉPLACEMENT", "BEWEGUNG", "MOVIMENTO" },
        { "COMBAT", "COMBATE", "COMBATE", "COMBAT", "KAMPF", "COMBATTIMENTO" },
        { "ITEMS AND MENUS", "ITENS E MENUS", "OBJETOS Y MENÚS", "OBJETS ET MENUS", "GEGENSTÄNDE UND MENÜS", "OGGETTI E MENU" },
        { "BUTTON LAYOUT", "LAYOUT DOS BOTÕES", "DISTRIBUCIÓN", "DISPOSITION", "TASTENBELEGUNG", "CONFIGURAZIONE" },
        { "RESET TO DEFAULTS", "RESTAURAR PADRÕES", "RESTABLECER VALORES", "RÉINITIALISER", "ZURÜCKSETZEN", "RIPRISTINA PREDEFINITI" },
        { "BACK", "VOLTAR", "ATRÁS", "RETOUR", "ZURÜCK", "INDIETRO" },
        { "MOVE FORWARD", "ANDAR PARA FRENTE", "AVANZAR", "AVANCER", "VORWÄRTS", "AVANTI" },
        { "MOVE BACK", "ANDAR PARA TRÁS", "RETROCEDER", "RECULER", "RÜCKWÄRTS", "INDIETRO" },
        { "MOVE LEFT", "ANDAR PARA A ESQUERDA", "IZQUIERDA", "GAUCHE", "LINKS", "SINISTRA" },
        { "MOVE RIGHT", "ANDAR PARA A DIREITA", "DERECHA", "DROITE", "RECHTS", "DESTRA" },
        { "LOOK LEFT", "OLHAR PARA A ESQUERDA", "MIRAR A LA IZQUIERDA", "REGARDER À GAUCHE", "NACH LINKS SEHEN", "GUARDA A SINISTRA" },
        { "LOOK RIGHT", "OLHAR PARA A DIREITA", "MIRAR A LA DERECHA", "REGARDER À DROITE", "NACH RECHTS SEHEN", "GUARDA A DESTRA" },
        { "LOOK UP", "OLHAR PARA CIMA", "MIRAR ARRIBA", "REGARDER EN HAUT", "NACH OBEN SEHEN", "GUARDA IN ALTO" },
        { "LOOK DOWN", "OLHAR PARA BAIXO", "MIRAR ABAJO", "REGARDER EN BAS", "NACH UNTEN SEHEN", "GUARDA IN BASSO" },
        { "FIRE", "ATIRAR", "DISPARAR", "TIRER", "FEUERN", "SPARA" },
        { "AIM", "MIRAR", "APUNTAR", "VISER", "ZIELEN", "MIRA" },
        { "RELOAD", "RECARREGAR", "RECARGAR", "RECHARGER", "NACHLADEN", "RICARICA" },
        { "CROUCH", "AGACHAR", "AGACHARSE", "S'ACCROUPIR", "DUCKEN", "ACCOVACCIATI" },
        { "USE", "USAR", "USAR", "UTILISER", "BENUTZEN", "USA" },
        { "NEXT WEAPON", "PRÓXIMA ARMA", "ARMA SIGUIENTE", "ARME SUIVANTE", "NÄCHSTE WAFFE", "ARMA SUCCESSIVA" },
        { "PREVIOUS WEAPON", "ARMA ANTERIOR", "ARMA ANTERIOR", "ARME PRÉCÉDENTE", "VORHERIGE WAFFE", "ARMA PRECEDENTE" },
        { "GRENADE", "GRANADA", "GRANADA", "GRENADE", "GRANATE", "GRANATA" },
        { "FIRE MODE", "MODO DE DISPARO", "MODO DE DISPARO", "MODE DE TIR", "FEUERMODUS", "MODALITÀ DI FUOCO" },
        { "SUPPRESSOR", "SILENCIADOR", "SILENCIADOR", "SILENCIEUX", "SCHALLDÄMPFER", "SILENZIATORE" },
        { "MELEE", "CORPO A CORPO", "CUERPO A CUERPO", "CORPS À CORPS", "NAHKAMPF", "CORPO A CORPO" },
        { "HEALTH PACK", "KIT MÉDICO", "BOTIQUÍN", "TROUSSE DE SOINS", "MEDIKIT", "KIT MEDICO" },
        { "CYCLE ITEM", "TROCAR ITEM", "CAMBIAR OBJETO", "CHANGER D'OBJET", "GEGENSTAND WECHSELN", "CAMBIA OGGETTO" },
        { "OBJECTIVES", "OBJETIVOS", "OBJETIVOS", "OBJECTIFS", "ZIELE", "OBIETTIVI" },
        { "PAUSE", "PAUSAR", "PAUSA", "PAUSE", "PAUSE", "PAUSA" },
        { "PRESS A KEY", "PRESSIONE UMA TECLA", "PULSA UNA TECLA", "APPUIE SUR UNE TOUCHE", "TASTE DRÜCKEN", "PREMI UN TASTO" },
        { "PRESS A BUTTON", "PRESSIONE UM BOTÃO", "PULSA UN BOTÓN", "APPUIE SUR UN BOUTON", "TASTE DRÜCKEN", "PREMI UN PULSANTE" },
        { "PRESET", "PREDEFINIÇÃO", "PREAJUSTE", "PRÉRÉGLAGE", "VORGABE", "PRESET" },
        /* help texts stay under PCM_TEXT_CAP (160) in every language */
        { "Enter changes the key and Delete clears it. Left and right pick the first or second key. A key moves from any action that had it.",
          "Enter troca a tecla e Delete a apaga. Esquerda e direita escolhem a primeira ou a segunda. Uma tecla sai da ação que a tinha.",
          "Intro cambia la tecla y Supr la borra. Izquierda y derecha eligen la primera o la segunda. Una tecla deja la acción que la tenía.",
          "Entrée change la touche, Suppr l'efface. Gauche et droite choisissent la première ou la seconde. Une touche quitte l'action qui l'avait.",
          "Eingabe ändert die Taste, Entf löscht sie. Links und rechts wählen die erste oder zweite. Eine Taste verlässt die Aktion, die sie hatte.",
          "Invio cambia il tasto, Canc lo cancella. Sinistra e destra scelgono il primo o il secondo. Un tasto lascia l'azione che lo aveva." },
        { "Press the key or mouse button to use. Escape cancels.",
          "Pressione a tecla ou o botão do mouse a usar. Esc cancela.",
          "Pulsa la tecla o el botón del ratón que quieras usar. Esc cancela.",
          "Appuie sur la touche ou le bouton de souris à utiliser. Échap annule.",
          "Drücke die gewünschte Taste oder Maustaste. Esc bricht ab.",
          "Premi il tasto o il pulsante del mouse da usare. Esc annulla." },
        { "Confirm, then press the button to use. Taking another action's button swaps the two. X puts back the layout's button.",
          "Confirme e pressione o botão a usar. Pegar o botão de outra ação troca as duas. X volta ao botão do layout.",
          "Confirma y pulsa el botón que quieras usar. Tomar el botón de otra acción intercambia las dos. X devuelve el botón de la distribución.",
          "Confirme, puis appuie sur le bouton à utiliser. Prendre le bouton d'une autre action échange les deux. X remet le bouton de la disposition.",
          "Bestätigen, dann die gewünschte Taste drücken. Die Taste einer anderen Aktion tauscht beide. X stellt die Taste der Belegung wieder her.",
          "Conferma, poi premi il pulsante da usare. Prendere il pulsante di un'altra azione scambia le due. X rimette il pulsante della configurazione." },
        { "Press the button to use. Start or Escape cancels.",
          "Pressione o botão a usar. Start ou Esc cancela.",
          "Pulsa el botón que quieras usar. Start o Esc cancela.",
          "Appuie sur le bouton à utiliser. Start ou Échap annule.",
          "Drücke die gewünschte Taste. Start oder Esc bricht ab.",
          "Premi il pulsante da usare. Start o Esc annulla." },
        { "The game's four button layouts; preset 4 swaps the sticks. Choosing one drops your own button changes.",
          "Os quatro layouts de botões do jogo; a predefinição 4 inverte os analógicos. Escolher um desfaz suas mudanças de botões.",
          "Las cuatro distribuciones del juego; el preajuste 4 intercambia los sticks. Elegir una descarta tus cambios de botones.",
          "Les quatre dispositions du jeu ; le préréglage 4 inverse les sticks. En choisir une annule tes changements de boutons.",
          "Die vier Tastenbelegungen des Spiels; Vorgabe 4 tauscht die Sticks. Eine Auswahl verwirft deine eigenen Änderungen.",
          "Le quattro configurazioni del gioco; il preset 4 inverte le levette. Sceglierne una annulla le tue modifiche." },
    };
    const int locale = pc_menu_locale();
    int i;
    if (!locale) return text;
    for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])); ++i)
        if (!strcmp(text, words[i].english)) {
            const char *values[] = { words[i].english, words[i].pt, words[i].es, words[i].fr, words[i].de, words[i].it };
            return values[locale];
        }
    return text;
}

static const char *localized_video_literal(const char *text)
{
    const char *active_page = m_page >= 0 && m_doc ? m_doc->page[m_page].name : m_page_name;
    int locale = pc_menu_locale();
    if (!strcmp(text, "SPRINT") || !strcmp(text, "JUMP")) {
        static const char *const sprint[] = { "SPRINT", "CORRER", "CORRER", "COURIR", "SPRINTEN", "CORRI" };
        static const char *const jump[] = { "JUMP", "PULAR", "SALTAR", "SAUTER", "SPRINGEN", "SALTA" };
        return !strcmp(text, "SPRINT") ? sprint[locale] : jump[locale];
    }
    static const struct { const char *english, *pt, *es, *fr, *de, *it; } words[] = {
        { "VIDEO SETTINGS", "CONFIGURAÇÕES DE VÍDEO", "AJUSTES DE VÍDEO", "PARAMÈTRES VIDÉO", "VIDEOEINSTELLUNGEN", "IMPOSTAZIONI VIDEO" },
        { "PC SETTINGS", "CONFIGURAÇÕES PC", "AJUSTES PC", "PARAMÈTRES PC", "PC-EINSTELLUNGEN", "IMPOSTAZIONI PC" },
        { "GAMEPLAY SETTINGS", "JOGABILIDADE", "JUGABILIDAD", "JOUABILITÉ", "SPIELABLAUF", "GIOCO" },
        { "DISPLAY", "TELA", "PANTALLA", "AFFICHAGE", "ANZEIGE", "SCHERMO" },
        { "GRAPHICS", "GRÁFICOS", "GRÁFICOS", "GRAPHIQUES", "GRAFIK", "GRAFICA" },
        { "RESET TO DEFAULTS", "RESTAURAR PADRÕES", "RESTABLECER VALORES", "RÉINITIALISER", "ZURÜCKSETZEN", "RIPRISTINA PREDEFINITI" },
        { "BACK", "VOLTAR", "ATRÁS", "RETOUR", "ZURÜCK", "INDIETRO" },
        { "WINDOW MODE", "MODO DE JANELA", "MODO DE VENTANA", "MODE FENÊTRÉ", "FENSTERMODUS", "MODALITÀ FINESTRA" },
        { "WINDOW SIZE", "RESOLUÇÃO", "RESOLUCIÓN", "RÉSOLUTION", "AUFLÖSUNG", "RISOLUZIONE" },
        { "INTERNAL RESOLUTION", "RESOLUÇÃO INTERNA", "RESOLUCIÓN INTERNA", "RÉSOLUTION INTERNE", "INTERNE AUFLÖSUNG", "RISOLUZIONE INTERNA" },
        { "ASPECT RATIO", "PROPORÇÃO", "RELACIÓN DE ASPECTO", "FORMAT D’IMAGE", "BILDFORMAT", "PROPORZIONI" },
        { "SCALING", "ESCALA DA IMAGEM", "ESCALADO", "MISE À L’ÉCHELLE", "SKALIERUNG", "RIDIMENSIONAMENTO" },
        { "SCALING FILTER", "FILTRO DE ESCALA", "FILTRO DE ESCALADO", "FILTRE DE MISE À L’ÉCHELLE", "SKALIERUNGSFILTER", "FILTRO DI RIDIMENSIONAMENTO" },
        { "VERTICAL SYNC", "SINCRONIZAÇÃO VERTICAL", "SINCRONIZACIÓN VERTICAL", "SYNCHRONISATION VERTICALE", "VERTIKALE SYNCHRONISIERUNG", "SINCRONIZZAZIONE VERTICALE" },
        { "FRAME LIMIT", "LIMITE DE QUADROS", "LÍMITE DE FPS", "LIMITE D’IMAGES/S", "BILDERLIMIT", "LIMITE FPS" },
        { "ANTI-ALIASING", "SUAVIZAÇÃO DE SERRILHADO", "SUAVIZADO", "ANTICRÉNELAGE", "ANTIALIASING", "ANTIALIASING" },
        { "ANISOTROPIC FILTERING", "FILTRAGEM ANISOTRÓPICA", "FILTRADO ANISOTRÓPICO", "FILTRAGE ANISOTROPE", "ANISOTROPE FILTERUNG", "FILTRO ANISOTROPICO" },
        { "DEPTH OF FIELD", "PROFUNDIDADE DE CAMPO", "PROFUNDIDAD DE CAMPO", "PROFONDEUR DE CHAMP", "SCHÄRFENTIEFE", "PROFONDITÀ DI CAMPO" },
        { "SHARPENING", "NITIDEZ", "NITIDEZ", "NETTETÉ", "SCHÄRFEN", "NITIDEZZA" },
        { "AMBIENT OCCLUSION", "OCLUSÃO DE AMBIENTE", "OCLUSIÓN AMBIENTAL", "OCCLUSION AMBIANTE", "UMGEBUNGSVERDECKUNG", "OCCLUSIONE AMBIENTALE" },
        { "AO QUALITY", "QUALIDADE DA OCLUSÃO", "CALIDAD DE OCLUSIÓN", "QUALITÉ DE L’OCCLUSION", "AO-QUALITÄT", "QUALITÀ AO" },
        { "REFLECTIONS", "REFLEXOS", "REFLEJOS", "RÉFLEXIONS", "SPIEGELUNGEN", "RIFLESSI" },
        { "RAY TRACING (EXPERIMENTAL)", "RAY TRACING (EXPERIMENTAL)", "TRAZADO DE RAYOS (EXPERIMENTAL)", "RAY TRACING (EXPÉRIMENTAL)",
          "RAYTRACING (EXPERIMENTELL)", "RAY TRACING (SPERIMENTALE)" },
        { "FIELD OF VIEW", "FOV", "CAMPO DE VISIÓN", "CHAMP DE VISION", "SICHTFELD", "CAMPO VISIVO" },
        { "MOTION BLUR", "DESFOQUE DE MOVIMENTO", "DESENFOQUE DE MOVIMIENTO", "FLOU DE MOUVEMENT", "BEWEGUNGSUNSCHÄRFE", "MOSSO DI MOVIMENTO" },
        { "CROSSHAIR", "MIRA", "PUNTO DE MIRA", "RÉTICULE", "FADENKREUZ", "MIRINO" },
        { "HIT MARKERS", "MARCADORES DE ACERTO", "MARCADORES DE IMPACTO", "MARQUEURS D’IMPACT", "TREFFERMARKER", "INDICATORI DI COLPO" },
        /* the players before the difficulty (split-screen) */
        { "PLAYERS", "JOGADORES", "JUGADORES", "JOUEURS", "SPIELER", "GIOCATORI" },
        { "ONE PLAYER", "UM JOGADOR", "UN JUGADOR", "UN JOUEUR", "EIN SPIELER", "UN GIOCATORE" },
        { "TWO PLAYERS (EXPERIMENTAL)", "DOIS JOGADORES (EXPERIMENTAL)", "DOS JUGADORES (EXPERIMENTAL)", "DEUX JOUEURS (EXPÉRIMENTAL)",
          "ZWEI SPIELER (EXPERIMENTELL)", "DUE GIOCATORI (SPERIMENTALE)" },
        { "TWO PLAYERS: PLAYER TWO IN THE BOTTOM HALF, ON THE SECOND CONTROLLER OR THE ARROWS AND THE NUMPAD.",
          "DOIS JOGADORES: O JOGADOR DOIS NA METADE DE BAIXO, NO SEGUNDO CONTROLE OU NAS SETAS E NO TECLADO NUMÉRICO.",
          "DOS JUGADORES: EL JUGADOR DOS EN LA MITAD DE ABAJO, CON EL SEGUNDO MANDO O LAS FLECHAS Y EL TECLADO NUMÉRICO.",
          "DEUX JOUEURS : LE JOUEUR DEUX EN BAS, À LA DEUXIÈME MANETTE OU AUX FLÈCHES ET AU PAVÉ NUMÉRIQUE.",
          "ZWEI SPIELER: SPIELER ZWEI UNTEN, MIT DEM ZWEITEN CONTROLLER ODER DEN PFEILTASTEN UND DEM ZIFFERNBLOCK.",
          "DUE GIOCATORI: IL GIOCATORE DUE IN BASSO, CON IL SECONDO CONTROLLER O LE FRECCE E IL TASTIERINO NUMERICO." },
        /* photo mode (Portuguese reviewed; the others machine drafts, like the launcher's) */
        { "PHOTO MODE", "MODO FOTO", "MODO FOTO", "MODE PHOTO", "FOTOMODUS", "MODALITÀ FOTO" },
        { "WEAPON", "ARMA", "ARMA", "ARME", "WAFFE", "ARMA" },
        { "HIDDEN", "OCULTA", "OCULTA", "MASQUÉE", "AUSGEBLENDET", "NASCOSTA" },
        { "SHOWN", "VISÍVEL", "VISIBLE", "VISIBLE", "SICHTBAR", "VISIBILE" },
        { "ROLL", "INCLINAÇÃO", "GIRO", "ROULIS", "NEIGUNG", "ROLLIO" },
        { "CAMERA SPEED", "VELOCIDADE DA CÂMERA", "VELOCIDAD DE CÁMARA", "VITESSE DE LA CAMÉRA", "KAMERATEMPO", "VELOCITÀ CAMERA" },
        { "CAMERA RANGE", "ALCANCE DA CÂMERA", "ALCANCE DE CÁMARA", "PORTÉE DE LA CAMÉRA", "KAMERAREICHWEITE", "RAGGIO CAMERA" },
        { "FREE", "LIVRE", "LIBRE", "LIBRE", "FREI", "LIBERA" },
        { "TAKE PHOTO", "TIRAR FOTO", "HACER FOTO", "PRENDRE UNE PHOTO", "FOTO AUFNEHMEN", "SCATTA FOTO" },
        { "RESET CAMERA", "REDEFINIR CÂMERA", "REINICIAR CÁMARA", "RÉINITIALISER LA CAMÉRA", "KAMERA ZURÜCKSETZEN", "RIPRISTINA CAMERA" },
        { "HIDE PANEL", "OCULTAR PAINEL", "OCULTAR PANEL", "MASQUER LE PANNEAU", "LEISTE AUSBLENDEN", "NASCONDI PANNELLO" },
        { "EXIT PHOTO MODE", "SAIR DO MODO FOTO", "SALIR DEL MODO FOTO", "QUITTER LE MODE PHOTO", "FOTOMODUS VERLASSEN", "ESCI DALLA MODALITÀ FOTO" },
        { "SLOW", "LENTA", "LENTA", "LENTE", "LANGSAM", "LENTA" },
        { "NORMAL", "NORMAL", "NORMAL", "NORMALE", "NORMAL", "NORMALE" },
        { "FAST", "RÁPIDA", "RÁPIDA", "RAPIDE", "SCHNELL", "VELOCE" },
        { "VERY FAST", "MUITO RÁPIDA", "MUY RÁPIDA", "TRÈS RAPIDE", "SEHR SCHNELL", "MOLTO VELOCE" },
        { "WASD MOVES, THE MOUSE LOOKS, Q AND E DOWN AND UP, SHIFT AND CTRL SPEED, WHEEL ZOOMS, Z AND C ROLL. F12 TAKES A PHOTO, H HIDES THIS PANEL.",
          "WASD MOVE, O MOUSE OLHA, Q E E DESCEM E SOBEM, SHIFT E CTRL MUDAM A VELOCIDADE, A RODA DÁ ZOOM, Z E C INCLINAM. F12 TIRA UMA FOTO, H OCULTA O PAINEL.",
          "WASD MUEVE, EL RATÓN MIRA, Q Y E BAJAN Y SUBEN, SHIFT Y CTRL VELOCIDAD, LA RUEDA HACE ZOOM, Z Y C GIRAN. F12 HACE UNA FOTO, H OCULTA EL PANEL.",
          "WASD DÉPLACE, LA SOURIS REGARDE, Q ET E DESCENDENT ET MONTENT, MAJ ET CTRL VITESSE, MOLETTE ZOOM, Z ET C ROULIS. F12 PHOTO, H MASQUE LE PANNEAU.",
          "WASD BEWEGT, DIE MAUS SCHAUT, Q UND E RUNTER UND HOCH, SHIFT UND STRG TEMPO, MAUSRAD ZOOM, Z UND C NEIGEN. F12 MACHT EIN FOTO, H BLENDET AUS.",
          "WASD MUOVE, IL MOUSE GUARDA, Q ED E SCENDONO E SALGONO, SHIFT E CTRL VELOCITÀ, ROTELLA ZOOM, Z E C ROLLIO. F12 SCATTA, H NASCONDE IL PANNELLO." },
        { "LEFT STICK MOVES, RIGHT STICK LOOKS, TRIGGERS DOWN AND UP, WHITE AND BLACK ZOOM. Y TAKES A PHOTO, X HIDES THIS PANEL.",
          "ANALÓGICO ESQUERDO MOVE, O DIREITO OLHA, GATILHOS DESCEM E SOBEM, BRANCO E PRETO DÃO ZOOM. Y TIRA UMA FOTO, X OCULTA O PAINEL.",
          "STICK IZQUIERDO MUEVE, EL DERECHO MIRA, GATILLOS BAJAN Y SUBEN, BLANCO Y NEGRO ZOOM. Y HACE UNA FOTO, X OCULTA EL PANEL.",
          "STICK GAUCHE DÉPLACE, LE DROIT REGARDE, GÂCHETTES DESCENDENT ET MONTENT, BLANC ET NOIR ZOOM. Y PHOTO, X MASQUE LE PANNEAU.",
          "LINKER STICK BEWEGT, DER RECHTE SCHAUT, TRIGGER RUNTER UND HOCH, WEISS UND SCHWARZ ZOOM. Y MACHT EIN FOTO, X BLENDET AUS.",
          "LEVETTA SINISTRA MUOVE, LA DESTRA GUARDA, GRILLETTI SCENDONO E SALGONO, BIANCO E NERO ZOOM. Y SCATTA, X NASCONDE IL PANNELLO." },
        { "THE PHOTO COULD NOT BE SAVED.", "NÃO FOI POSSÍVEL SALVAR A FOTO.", "NO SE PUDO GUARDAR LA FOTO.", "IMPOSSIBLE D'ENREGISTRER LA PHOTO.",
          "DAS FOTO KONNTE NICHT GESPEICHERT WERDEN.", "IMPOSSIBILE SALVARE LA FOTO." },
        { "PHOTO SAVED TO PICTURES\\BLACK:", "FOTO SALVA EM IMAGENS\\BLACK:", "FOTO GUARDADA EN IMÁGENES\\BLACK:", "PHOTO ENREGISTRÉE DANS IMAGES\\BLACK :",
          "FOTO GESPEICHERT IN BILDER\\BLACK:", "FOTO SALVATA IN IMMAGINI\\BLACK:" }
    };
    int i;
    if (!_strnicmp(active_page, "Controls", 8) || !strcmp(text, "CONTROLS")) return controls_text(text);
    if (!locale || (_stricmp(active_page, "VideoSettings") && _stricmp(active_page, "VideoDisplay") && _stricmp(active_page, "VideoGraphics") &&
                    _stricmp(active_page, "VideoLauncher") && _stricmp(active_page, "PauseMenu") &&
                    _stricmp(active_page, "PhotoMode") && _stricmp(active_page, "Players"))) return text;
    for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])); ++i)
        if (!strcmp(text, words[i].english)) {
            const char *values[] = { words[i].english, words[i].pt, words[i].es, words[i].fr, words[i].de, words[i].it };
            return values[locale];
        }
    return text;
}

/* Resolves a text spec to UTF-16. Returns 0 when there is nothing to show. */
static int spec_text(const PcMenuHost *host, const char *spec, uint16_t *out, unsigned cap)
{
    out[0] = 0;
    if (spec[0] == '"') {
        char tmp[PCM_SPEC];
        size_t n = strlen(spec);
        copy(tmp, sizeof(tmp), spec + 1);
        if (n >= 2) tmp[n - 2] = 0;
        utf8_to_utf16(localized_video_literal(tmp), out, cap);
    } else if (!strcmp(spec, "&VIDEO_HELP")) {
        const Item *it = m_page >= 0 && m_focus >= 0 && m_focus < m_doc->page[m_page].nitem ? &m_doc->page[m_page].item[m_focus] : NULL;
        utf8_to_utf16(it && it->kind == IT_VIDEO ? pc_video_menu_row_help(it->video_row) : "", out, cap);
    } else if (!strcmp(spec, "&CONTROLS_HELP")) {
        const Item *it = m_page >= 0 && m_focus >= 0 && m_focus < m_doc->page[m_page].nitem ? &m_doc->page[m_page].item[m_focus] : NULL;
        const LONG cap_mode = InterlockedCompareExchange(&m_capture, 0, 0);
        const char *help = "";
        if (it && it->kind == IT_KEY) help = cap_mode ? "Press the key or mouse button to use. Escape cancels." :
            "Enter changes the key and Delete clears it. Left and right pick the first or second key. A key moves from any action that had it.";
        else if (it && it->kind == IT_PAD) help = cap_mode ? "Press the button to use. Start or Escape cancels." :
            "Confirm, then press the button to use. Taking another action's button swaps the two. X puts back the layout's button.";
        else if (it && it->kind == IT_LAYOUT) help = "The game's four button layouts; preset 4 swaps the sticks. Choosing one drops your own button changes.";
        utf8_to_utf16(controls_text(help), out, cap);
    } else if (!strcmp(spec, "&PHOTO_HELP")) {
        /* the controls, or the last photo: "PHOTO SAVED TO PICTURES\BLACK: <name>" translates up to its name */
        const char *help = pc_photo_help(), *colon = strstr(help, "BLACK: ");
        char line[PCM_TEXT_CAP * 2];
        if (colon) {
            char head[64];
            const size_t n = (size_t)(colon - help) + 6;
            snprintf(head, sizeof(head), "%.*s", (int)(n < sizeof(head) ? n : sizeof(head) - 1), help);
            snprintf(line, sizeof(line), "%s%s", localized_video_literal(head), colon + 6);
        } else {
            snprintf(line, sizeof(line), "%s", localized_video_literal(help));
        }
        utf8_to_utf16(line, out, cap);
    } else if (spec[0] == '@') {
        const char *v = var_lookup(host, spec + 1);
        utf8_to_utf16(v ? v : "", out, cap);
    } else if (host->text) {
        int n = host->text(host->ctx, spec, out, cap);
        if (n < 0) { out[0] = 0; return 0; }
    }
    {   /* the game's strings end texts with returns; a line break is drawn by the layout */
        unsigned n = 0;
        while (out[n]) n++;
        while (n && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
        return n != 0;
    }
}

static void text_prim(Display *d, int font, float x, float y, float size, float scale, uint32_t c, const uint16_t *t)
{
    Prim *p = add(d, PR_TEXT);
    unsigned i;
    if (!p) return;
    p->font = (uint8_t)font; p->x = x; p->y = y; p->size = size; p->scale = scale; p->colour = c;
    for (i = 0; t[i] && i + 1 < PCM_TEXT_CAP; i++) p->text[i] = t[i];
    p->text[i] = 0;
}

/* Text that wraps at `wrap` units (0 = no wrapping) and breaks at returns; `right` lines each line's end up on x.
 * Returns the lines drawn. */
static int text_block(Display *d, int font, float x, float y, float size, float wrap, float line_h, uint32_t c, const uint16_t *t,
                      int right)
{
    uint16_t line[PCM_TEXT_CAP];
    int lines = 0;
    while (*t) {
        unsigned n = 0, last_space = 0, i;
        while (t[n] && t[n] != '\r' && t[n] != '\n' && n + 1 < PCM_TEXT_CAP) {
            line[n] = t[n]; line[n + 1] = 0;
            if (t[n] == ' ') last_space = n;
            if (wrap > 0 && text_width(font, line, size) > wrap && last_space) { n = last_space; break; }
            n++;
        }
        line[n] = 0;
        for (i = n; i && line[i - 1] == ' '; i--) line[i - 1] = 0;
        if (line[0]) text_prim(d, font, right ? x - text_width(font, line, size) : x, y + lines * line_h, size, 1.0f, c, line);
        lines++;
        t += n;
        while (*t == ' ') t++;
        if (*t == '\r') t++;
        if (*t == '\n') t++;
    }
    return lines;
}

static float ease(float u) { if (u <= 0) return 0; if (u >= 1) return 1; return u * u * (3 - 2 * u); }

static uint32_t mix(uint32_t a, uint32_t b, float u)
{
    uint32_t r = 0;
    int s;
    for (s = 0; s < 32; s += 8) {
        const float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        r |= (uint32_t)(ca + (cb - ca) * u + 0.5f) << s;
    }
    return r;
}

static void rails(Display *d, const Style *st, const Page *p)
{
    /* Two rails down the left of the menu with a ruler's ticks, as on every original page. The space
     * between them shows the game almost as it is; the rest is shaded. */
    const float l = p->rail_l, r = p->rail_r;
    int i;
    rect(d, -2000, 0, l + 2000, 480, st->shade, PF_EXT_LEFT | PF_EXT_UP | PF_EXT_DOWN | PF_STORE);
    rect(d, l, 0, r - l, 480, p->has_column ? p->column : st->column, PF_EXT_UP | PF_EXT_DOWN | PF_STORE);
    rect(d, r, 0, 640 - r, 480, st->shade, PF_EXT_RIGHT | PF_EXT_UP | PF_EXT_DOWN | PF_STORE);
    rect(d, l, 0, 7, 480, st->rail, PF_EXT_UP | PF_EXT_DOWN);
    rect(d, l + 10, 0, 2, 480, mix(0xFF000000u, st->rail, 0.68f), PF_EXT_UP | PF_EXT_DOWN);
    rect(d, r - 14, 0, 2, 480, st->rail, PF_EXT_UP | PF_EXT_DOWN);
    rect(d, r - 9, 0, 2, 480, st->rail, PF_EXT_UP | PF_EXT_DOWN);
    rect(d, r - 4, 0, 5, 480, st->rail, PF_EXT_UP | PF_EXT_DOWN);
    for (i = 0; i < 48; i++) {
        /* a ruler: every ten units, long and short in turn, the two sides out of step */
        rect(d, l - ((i & 1) ? 3.0f : 9.0f), 7.0f + i * 10.0f, (i & 1) ? 3.0f : 9.0f, 1, st->rail, 0);
        rect(d, r + 1, 4.0f + i * 10.0f, (i & 1) ? 9.0f : 3.0f, 1, st->rail, 0);
    }
}

static void prompt_icon(Display *d, float x, float y, uint32_t c, uint16_t a, uint16_t b)
{
    /* The prompts' button art is in the small font (25-texel cells); the original shows it at about half size. */
    uint16_t t[3] = { a, b, 0 };
    text_prim(d, PCM_FONT_SMALL, x, y + 4.0f, 15.0f * 0.56f, 1.0f, c, t);   /* centred on the text's capitals */
}

/* Text at x, made smaller when it would be wider than `room`. */
static void fit_text(Display *d, float x, float y, float size, float room, uint32_t c, const uint16_t *t)
{
    const float width = text_width(PCM_FONT_SMALL, t, size);
    if (room > 0 && width > room) size *= room / width;
    text_prim(d, PCM_FONT_SMALL, x, y, size, 1.0f, c, t);
}

/* A controller control as the prompts draw it: the menu font's button art, with its mark (LT, START, L ...) on the
 * blank button or stick. Returns the width it took. */
static float pad_icon(Display *d, float x, float y, int control)
{
    const char *mark;
    const uint16_t code = pc_controls_pad_glyph(control, &mark);
    const float size = 15.0f * 0.56f;              /* prompt_icon's size; it draws the glyph at y + 4 */
    uint16_t m[8];
    float box[4];
    if (!code || !glyph_box(PCM_FONT_SMALL, code, size, box)) return 0;
    prompt_icon(d, x, y, 0xFFFFFFFFu, code, 0);
    if (mark) {
        /* centred on the cell; capitals are about 0.72 of the text size tall */
        const float ms = strlen(mark) > 2 ? 5.0f : 7.0f;
        const float cx = x + box[0] + box[2] * 0.5f, cy = y + 4.0f + box[1] + box[3] * 0.5f;
        utf8_to_utf16(mark, m, 8);
        text_prim(d, PCM_FONT_SMALL, cx - text_width(PCM_FONT_SMALL, m, ms) * 0.5f, cy - ms * 0.36f, ms, 1.0f, 0xFFC8C8C8u, m);
    }
    return box[0] + box[2];
}

static void build(Display *d, const PcMenuHost *host, const Page *p, int ready, double t)
{
    const Style *st = &m_doc->style;
    const float isize = p->item_size > 0 ? p->item_size : st->item_size;
    const int keyboard = pc_input_prompt_device() == PC_DEVICE_KEYBOARD_MOUSE;
    uint16_t buf[PCM_TEXT_CAP];
    float y, ys[PCM_ITEMS];
    int i, loading = 0;
    d->n = 0;
    {   /* the original pages slide in from the right; a short slide keeps that without delaying the page */
        const float u = st->intro > 0 ? (float)((t - m_page_t0) / st->intro) : 1.0f;
        d->slide = (1.0f - ease(u)) * 40.0f;
    }
    if (p->photo) {
        /* photo mode: a panel over the picture, or nothing while the camera moves or the player hid it */
        if (pc_photo_panel_hidden()) { d->n = 0; return; }
        rect(d, p->panel[0], p->panel[1], p->panel[2], p->panel[3], st->bar, 0);
        rect(d, p->panel[0], p->panel[1], 5, p->panel[3], st->rail, 0);
        rect(d, p->panel[0] + 8, p->panel[1], 2, p->panel[3], mix(0xFF000000u, st->rail, 0.68f), 0);
        rect(d, p->panel[0], p->panel[1], p->panel[2], 1, st->line, 0);
        rect(d, p->panel[0], p->panel[1] + p->panel[3] - 1, p->panel[2], 1, st->line, 0);
    } else {
        rails(d, st, p);
    }
    if (m_image[PCM_IMAGE_LOGO].pixels && st->logo_w > 0 && !p->no_logo && !p->photo) {
        Prim *q = add(d, PR_IMAGE);
        if (q) {
            q->image = PCM_IMAGE_LOGO; q->x = p->logo_x; q->y = p->logo_y; q->w = st->logo_w;
            q->h = st->logo_w * (float)m_image[PCM_IMAGE_LOGO].h / (float)m_image[PCM_IMAGE_LOGO].w;
            q->colour = st->logo;
        }
    }
    if (p->title[0] && spec_text(host, p->title, buf, PCM_TEXT_CAP))
        text_prim(d, PCM_FONT_BIG, p->title_x, p->title_y, st->title_size, 1.0f, st->title, buf);
    for (i = 0; i < p->ntext; i++) {
        const Text *tx = &p->text[i];
        const float size = tx->size > 0 ? tx->size : tx->role == TX_SMALL ? 14.0f : st->text_size;
        if (!cond_true(host, tx->cond) || !spec_text(host, tx->spec, buf, PCM_TEXT_CAP)) continue;
        text_block(d, PCM_FONT_SMALL, tx->x, tx->y, size, tx->wrap, size,
                   tx->role == TX_LABEL ? st->label : st->text, buf, tx->right);
    }
    for (i = 0; i < p->nsep; i++)
        rect(d, p->sep[i][0], p->sep[i][1], p->sep[i][2] - p->sep[i][0], 2, st->line, 0);
    if (p->loading[0]) { int v = 0; loading = read_value(host, p->loading, &v) && v == 1; }
    /* the item column */
    y = p->list_y;
    for (i = 0; i < p->nitem; i++) {
        y += p->item[i].gap;
        ys[i] = y;
        if (item_visible(host, &p->item[i])) y += p->step + (p->item[i].kind == IT_SLIDER ? 37.0f : 0.0f);   /* a slider's bar */
        else ys[i] = -1;
    }
    m_hit_scratch_n = p->nitem;
    for (i = 0; i < p->nitem; i++) {
        /* the selector bar's area (and a slider's row of bullets below it) */
        Hit *h = &m_hit_scratch[i];
        const Item *it = &p->item[i];
        const float bw = p->bar_w > 0 ? p->bar_w : 300.0f;
        h->ok = ys[i] >= 0 && !loading && item_focusable(host, it);
        h->kind = it->kind;
        h->x0 = p->list_x - 7; h->x1 = h->x0 + bw;
        /* a row's values belong to it even where a narrow selector does not reach them */
        if (it->kind == IT_TOGGLE && p->col2 > 0 && p->col2 + 60.0f > h->x1) h->x1 = p->col2 + 60.0f;
        if (p->values_x > 0 && p->values_x + 8.0f > h->x1) h->x1 = p->values_x + 8.0f;
        h->y0 = ys[i] - 8; h->y1 = ys[i] + 18 + (it->kind == IT_SLIDER ? 37.0f : 0.0f);
        h->split = 0;
        if (it->kind == IT_TOGGLE && p->col2 > p->col1) h->split = p->col2 - 6.0f;
        else if (it->kind == IT_SLIDER && it->step > 0) h->split = p->list_x - 2 + (float)((m_slider[i] - it->lo) / it->step) * 10.0f;
    }
    {   /* the selector: a dark bar behind the focused item, brackets at its ends, lines out to the rail and the edge */
        const float u = st->anim > 0 ? ease((float)((t - m_focus_t0) / st->anim)) : 1.0f;
        if (m_focus >= 0 && m_focus < p->nitem && ys[m_focus] >= 0 && !loading) {
            float sy = ys[m_focus];
            if (m_prev_focus >= 0 && m_prev_focus < p->nitem && ys[m_prev_focus] >= 0) sy = ys[m_prev_focus] + (sy - ys[m_prev_focus]) * u;
            {
                const float bx = p->list_x - 7, bw = p->bar_w, by = sy - 8, bh = 26, mid = sy + 4;
                rect(d, bx, by, bw, bh, st->bar, 0);
                rect(d, bx - 1, by, 1, bh, st->line, 0);
                rect(d, bx + bw, by, 1, bh, st->line, 0);
                if (!p->photo) {                      /* out to the rails; a panel keeps its selector inside */
                    rect(d, p->rail_r + 1, mid, bx - 1 - (p->rail_r + 1), 2, st->line, 0);
                    rect(d, bx + bw + 1, mid, 640 - (bx + bw + 1), 2, st->line, PF_EXT_RIGHT);
                }
            }
        }
        for (i = 0; i < p->nitem; i++) {
            const Item *it = &p->item[i];
            float k = 0;                                   /* 0 = plain, 1 = focused */
            uint32_t c;
            if (ys[i] < 0) continue;
            if (i == m_focus) k = (m_prev_focus == m_focus) ? 1.0f : u;
            else if (i == m_prev_focus) k = 1.0f - u;
            if (loading || p->plain_focus) k = 0;
            c = item_disabled(host, it) ? st->disabled : it->dim[0] && cond_true(host, it->dim) ? st->dim : mix(st->item, st->focus, k);
            if (spec_text(host, it->text, buf, PCM_TEXT_CAP)) {
                const float sc = 1.0f + (st->focus_scale - 1.0f) * k;
                text_prim(d, PCM_FONT_SMALL, p->list_x, ys[i] - 5.5f * (sc - 1.0f), isize, sc, c, buf);
            }
            if (it->value[0] && spec_text(host, it->value, buf, PCM_TEXT_CAP)) {
                /* a value is the game's live text: as bright as the page's texts unless the row is dimmed */
                const uint32_t vc = c == st->dim || c == st->disabled ? c : st->text;
                if (p->values_x > 0) text_prim(d, PCM_FONT_SMALL, p->values_x - text_width(PCM_FONT_SMALL, buf, isize), ys[i], isize, 1.0f, vc, buf);
                else if (p->col1 > 0) text_prim(d, PCM_FONT_SMALL, p->col1, ys[i], isize, 1.0f, vc, buf);
            }
            if (it->kind == IT_VIDEO) {
                char value[96];
                const float right = p->values_x > 0 ? p->values_x : p->list_x + p->bar_w - 12.0f;
                float size = isize, width, room = right - p->list_x - 205.0f;
                pc_video_menu_row_value(it->video_row, value, sizeof(value));
                utf8_to_utf16(value, buf, PCM_TEXT_CAP);
                width = text_width(PCM_FONT_SMALL, buf, size);
                if (room > 0 && width > room) size *= room / width;
                text_prim(d, PCM_FONT_SMALL, right - text_width(PCM_FONT_SMALL, buf, size), ys[i], size, 1.0f,
                    pc_video_menu_row_locked(it->video_row) ? mix(st->item, st->focus, k) : st->text, buf);
            } else if (it->kind == IT_KEY || it->kind == IT_PAD) {
                /* a capture shows its request where the binding is, pulsing; the focused keyboard column is framed */
                const int capturing = i == m_focus && InterlockedCompareExchange(&m_capture, 0, 0) &&
                                      !loading;
                const uint32_t pulse = ((int)((t - m_capture_t0) * 2.5)) & 1 ? st->item : st->focus;
                const float room = (p->col2 > p->col1 ? p->col2 - p->col1 : 100.0f) - 10.0f;
                if (it->kind == IT_KEY) {
                    int col;
                    for (col = 0; col < 2; col++) {
                        const float cx = col ? p->col2 : p->col1;
                        const unsigned source = pc_input_binding((PcAction)it->action, col);
                        char label[48];
                        uint32_t vc = st->text;
                        if (capturing && col == m_bind_col) {
                            utf8_to_utf16(controls_text("PRESS A KEY"), buf, PCM_TEXT_CAP);
                            vc = pulse;
                        } else if (source && pc_input_source_label(source, label, sizeof(label))) {
                            utf8_to_utf16(label, buf, PCM_TEXT_CAP);
                        } else {
                            buf[0] = '-'; buf[1] = 0;
                            vc = st->item;
                        }
                        fit_text(d, cx, ys[i], isize, room, vc, buf);
                        if (i == m_focus && col == m_bind_col && k > 0.5f) {
                            const float fx = cx - 6, fy = ys[i] - 6, fw = room + 8, fh = 22;
                            rect(d, fx, fy, fw, 1, st->line, 0);
                            rect(d, fx, fy + fh - 1, fw, 1, st->line, 0);
                            rect(d, fx, fy, 1, fh, st->line, 0);
                            rect(d, fx + fw - 1, fy, 1, fh, st->line, 0);
                        }
                    }
                } else {
                    const int control = pc_controls_pad_control((PcAction)it->action);
                    if (capturing) {
                        utf8_to_utf16(controls_text("PRESS A BUTTON"), buf, PCM_TEXT_CAP);
                        fit_text(d, p->col1, ys[i], isize, room * 2, pulse, buf);
                    } else if (control >= 0) {
                        /* the names in one column, whatever the icon's width (the blank button is the widest) */
                        const float w = pad_icon(d, p->col1, ys[i] - 4.0f, control), name_x = w > 24.0f ? w + 6 : 30.0f;
                        utf8_to_utf16(pc_controls_pad_name(control), buf, PCM_TEXT_CAP);
                        fit_text(d, p->col1 + name_x, ys[i], isize, room * 2 - name_x, st->text, buf);
                    }
                }
            } else if (it->kind == IT_LAYOUT) {
                char value[48];
                const float right = p->values_x > 0 ? p->values_x : p->list_x + p->bar_w - 12.0f;
                snprintf(value, sizeof(value), "%s %d", controls_text("PRESET"), pc_controls_pad_layout() + 1);
                utf8_to_utf16(value, buf, PCM_TEXT_CAP);
                text_prim(d, PCM_FONT_SMALL, right - text_width(PCM_FONT_SMALL, buf, isize), ys[i], isize, 1.0f, st->text, buf);
            } else if (it->kind == IT_PHOTO) {
                char value[48];
                const float right = p->values_x > 0 ? p->values_x : p->list_x + p->bar_w - 12.0f;
                pc_photo_row_value(it->video_row, value, sizeof(value));
                utf8_to_utf16(localized_video_literal(value), buf, PCM_TEXT_CAP);
                text_prim(d, PCM_FONT_SMALL, right - text_width(PCM_FONT_SMALL, buf, isize), ys[i], isize, 1.0f, st->text, buf);
            } else if (it->kind == IT_TOGGLE) {
                int v = 0;
                static const uint16_t on[] = { 'o', 'n', 0 }, off[] = { 'o', 'f', 'f', 0 };
                uint16_t ton[16], toff[16];
                read_value(host, it->var, &v);
                if (!spec_text(host, "$FE_ON", ton, 16)) memcpy(ton, on, sizeof(on));
                if (!spec_text(host, "$FE_OFF", toff, 16)) memcpy(toff, off, sizeof(off));
                text_prim(d, PCM_FONT_SMALL, p->col1, ys[i], isize, 1.0f, v ? st->choice_on : st->choice, ton);
                text_prim(d, PCM_FONT_SMALL, p->col2, ys[i], isize, 1.0f, v ? st->choice : st->choice_on, toff);
            } else if (it->kind == IT_SLIDER && m_image[PCM_IMAGE_BULLET].pixels) {
                const int count = it->step > 0 ? (it->hi - it->lo) / it->step : 0;
                const int lit = it->step > 0 ? (m_slider[i] - it->lo) / it->step : 0;
                int b;
                for (b = 0; b < count && b < 40; b++) {
                    Prim *q = add(d, PR_IMAGE);
                    if (!q) break;
                    q->image = PCM_IMAGE_BULLET; q->x = p->list_x - 2 + b * 10.0f; q->y = ys[i] + 19;
                    q->w = 8; q->h = 32; q->colour = b < lit ? st->bullet_on : st->bullet;
                }
            }
        }
    }
    {   /* prompts */
        /* below the list: a list made longer in the file pushes the prompts down instead of running into them */
        float py = p->nitem && y + 1 > p->prompt_y ? y + 1 : p->prompt_y;
        const float px = p->prompt_x;
        if (loading) {
            if (spec_text(host, "$FE_LOADING", buf, PCM_TEXT_CAP)) text_prim(d, PCM_FONT_SMALL, px, py, st->text_size, 1.0f, st->prompt, buf);
        } else {
            if ((p->prompts & 1) && spec_text(host, "$FE_SELECT", buf, PCM_TEXT_CAP)) {
                if (!keyboard) {
                    prompt_icon(d, px - 18, py, 0xFFFFFFFFu, 0x25B2, 0);   /* the pad: one d-pad, in the A / B column */
                } else if (p->four_way) {               /* four small arrows round a point, as the keyboard prompt */
                    tri(d, px - 18, py - 8, px - 21.5f, py - 1, px - 14.5f, py - 1, st->prompt);
                    tri(d, px - 21.5f, py + 11, px - 14.5f, py + 11, px - 18, py + 18, st->prompt);
                    tri(d, px - 31, py + 5, px - 24, py + 1, px - 24, py + 9, st->prompt);
                    tri(d, px - 5, py + 5, px - 12, py + 1, px - 12, py + 9, st->prompt);
                } else {                                /* up and down, side by side */
                    tri(d, px - 30, py + 10, px - 19, py + 10, px - 24.5f, py, st->prompt);
                    tri(d, px - 18, py, px - 7, py, px - 12.5f, py + 10, st->prompt);
                }
                text_prim(d, PCM_FONT_SMALL, px, py, st->text_size, 1.0f, st->prompt, buf);
                py += p->prompt_step;
            }
            if ((p->prompts & 2) && spec_text(host, "$FE_CONFIRM", buf, PCM_TEXT_CAP)) {
                if (!keyboard) prompt_icon(d, px - 18, py, 0xFFFFFFFFu, 0x00B2, 0);
                text_prim(d, PCM_FONT_SMALL, px, py, st->text_size, 1.0f, st->prompt, buf);
                py += p->prompt_step;
            }
            if ((p->prompts & 4) && spec_text(host, "$FE_BACK", buf, PCM_TEXT_CAP)) {
                if (!keyboard) prompt_icon(d, px - 18, py, 0xFFFFFFFFu, 0x00B9, 0);
                text_prim(d, PCM_FONT_SMALL, px, py, st->text_size, 1.0f, st->prompt, buf);
            }
        }
    }
    (void)ready;
}

static int find_page(const char *name)
{
    int i;
    if (!m_doc || !name || !name[0]) return -1;
    for (i = 0; i < m_doc->npage; i++) if (!_stricmp(m_doc->page[i].name, name)) return i;
    return -1;
}

/* A page just shown starts with its `focus` item, or the next one that can take the focus. */
static void first_focus(const PcMenuHost *host, const Page *p, double t)
{
    int f;
    if (m_focus >= 0 || !p->nitem) return;
    f = p->focus;
    if (!item_focusable(host, &p->item[f])) f = step_focus(host, p, f, 1);
    m_focus = m_prev_focus = f;
    m_focus_t0 = t - 10;
}

static void deactivate(void)
{
    pc_photo_set_shown(0);
    pc_video_menu_hide_hint(0);
    AcquireSRWLockExclusive(&m_lock);
    if (m_display.n) { m_display.n = 0; m_generation++; }
    m_qhead = m_qtail = 0;
    ReleaseSRWLockExclusive(&m_lock);
    InterlockedExchange(&m_active, 0);
}

int pc_menu_tick(const char *page, int ready, const PcMenuHost *host)
{
    const double t = now_s();
    const Page *p;
    int idx, actions[32], n = 0, i;
    if (disabled() || !host) return 0;
    if (!page || !page[0] || !_stricmp(page, "Empty")) {
        if (m_page >= 0 || m_page_name[0]) vars_clear();
        m_page = -1; m_page_name[0] = 0; m_depth = 0;
        if (InterlockedCompareExchange(&m_active, 0, 0)) deactivate();
        return 0;
    }
    if (_stricmp(page, m_page_name)) {              /* a new page: the definition may have been edited since */
        load_doc(1);
        copy(m_page_name, PCM_NAME, page);
        idx = find_page(page);
        m_page = -1;
        m_depth = 0;                                /* subpages leave with their underlying game page */
        m_focus = m_prev_focus = -1;
        m_page_t0 = t;
        AcquireSRWLockExclusive(&m_lock);
        m_hit_n = 0; m_pointer_item = -1;
        ReleaseSRWLockExclusive(&m_lock);
        if (idx >= 0) {
            p = &m_doc->page[idx];
            m_page = idx;
            for (i = 0; i < p->nrequire; i++) if (!var_lookup(host, p->require[i])) { m_page = -1; break; }
        }
        if (m_page >= 0) {
            page_enter(host, m_page, t);
            p = &m_doc->page[m_page];
            fprintf(stderr, "[MENU] page %s: native (%d items)\n", page, p->nitem);
            {   /* lab: the game's values behind the page's settings, as the page opens */
                static int trace = -1;
                if (trace < 0) { const char *e = getenv("RECOMP_MENU_TRACE"); trace = e && *e && *e != '0'; }
                for (i = 0; trace && i < p->nitem; i++) {
                    int v = 0;
                    if (p->item[i].kind != IT_BUTTON && read_value(host, p->item[i].var, &v))
                        fprintf(stderr, "[MENU] trace value %s = %d\n", p->item[i].var, v);
                }
            }
        } else fprintf(stderr, "[MENU] page %s: the game's own\n", page);
    }
    if (m_page >= 0 && !m_overlay_assumed && (!m_overlay_seen || GetTickCount64() - m_overlay_seen > 1000)) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[MENU] no presenter overlay (RECOMP_FB_WINDOW unset?): the game's own menus\n"); }
        if (InterlockedCompareExchange(&m_active, 0, 0)) deactivate();
        return 0;
    }
    AcquireSRWLockExclusive(&m_lock);
    while (m_qhead != m_qtail && n < 32) { actions[n++] = m_queue[m_qhead]; m_qhead = (m_qhead + 1) % 32; }
    ReleaseSRWLockExclusive(&m_lock);
    if (InterlockedCompareExchange(&m_capture, 0, 0) && t - m_capture_t0 > PCM_CAPTURE_SECONDS) {
        InterlockedExchange(&m_capture, 0);
        if (m_page >= 0) play(host, "SNDBack");
    }
    if (ready) {
        for (i = 0; i < n; i++) {
            if (actions[i] == PCM_ACT_VIDEO) {
                /* The shortcut filter runs before ours. Revalidate its Options marker on the game thread. */
                if (!_stricmp(page, "OptionsMenuXbox") || !_stricmp(page, "PauseMenuOptionXbox")) {
                    if (!m_depth) open_page(host, "VideoSettings");
                }
                break;
            }
            if (m_page < 0) continue;
            p = &m_doc->page[m_page];
            first_focus(host, p, t);
            if (actions[i] >= PCM_ACT_HOVER && actions[i] < PCM_ACT_HOVER + PCM_ITEMS) {
                const int target = actions[i] - PCM_ACT_HOVER;
                if (!InterlockedCompareExchange(&m_capture, 0, 0) &&
                    target < p->nitem && target != m_focus && item_focusable(host, &p->item[target])) {
                    play(host, "SNDChange");
                    set_focus(host, p, target, t);
                }
                continue;
            }
            apply(host, p, actions[i], t);
            if ((actions[i] == PCM_ACT_CONFIRM || actions[i] == PCM_ACT_BACK || actions[i] == PCM_ACT_START) &&
                !InterlockedCompareExchange(&m_capture, 0, 0))
                break;                                                                  /* the page may be leaving */
        }
    }
    if (m_page < 0) {
        pc_photo_set_shown(0);
        if (InterlockedCompareExchange(&m_active, 0, 0)) deactivate();
        return 0;
    }
    p = &m_doc->page[m_page];                       /* an action may have opened or closed a page */
    pc_photo_set_shown(p->photo);
    if (pc_photo_shutter()) play(host, "SNDSelect");
    first_focus(host, p, t);
    build(&m_scratch, host, p, ready, t);
    AcquireSRWLockExclusive(&m_lock);
    if (m_display.n != m_scratch.n || m_display.slide != m_scratch.slide ||
        memcmp(m_display.prim, m_scratch.prim, sizeof(Prim) * (size_t)m_scratch.n)) {
        m_display.n = m_scratch.n;
        m_display.slide = m_scratch.slide;
        memcpy(m_display.prim, m_scratch.prim, sizeof(Prim) * (size_t)m_scratch.n);
        m_generation++;
    }
    memcpy(m_hit, m_hit_scratch, sizeof(m_hit));
    m_hit_n = m_hit_scratch_n;
    pointer_hit_locked();
    ReleaseSRWLockExclusive(&m_lock);
    m_active_tick = GetTickCount64();
    if (!InterlockedExchange(&m_active, 1)) pc_video_menu_hide_hint(1);
    return 1;
}

/* ---- input ------------------------------------------------------------------------------------------------------ */

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
#define KEY_DELETE 0x153u
#define KEY_W 0x011u
#define KEY_A 0x01Eu
#define KEY_S 0x01Fu
#define KEY_D 0x020u

static unsigned char m_key_swallowed[0x200];
static unsigned m_button_swallowed, m_pad_prev;
static uint32_t m_pad_controls_prev;
static int m_pad_hold;
static ULONGLONG m_pad_repeat[4];

int pc_menu_active(void)
{
    /* a native page that the game stopped updating (a crash in the page code, a lost focus of the loop) lets go */
    return InterlockedCompareExchange(&m_active, 0, 0) && GetTickCount64() - m_active_tick < 500;
}

static void queue(int action)
{
    AcquireSRWLockExclusive(&m_lock);
    /* Mouse motion can report the same row repeatedly before the game polls. Keep one hover so clicks
     * still have room, while a real new motion can re-focus a row after a page or keyboard change. */
    if (action >= PCM_ACT_HOVER && action < PCM_ACT_HOVER + PCM_ITEMS && m_qhead != m_qtail &&
        m_queue[(m_qtail + 31) % 32] == action) {
        ReleaseSRWLockExclusive(&m_lock);
        return;
    }
    if ((m_qtail + 1) % 32 != m_qhead) { m_queue[m_qtail] = action; m_qtail = (m_qtail + 1) % 32; }
    ReleaseSRWLockExclusive(&m_lock);
}

static void input_seen(void) { InterlockedExchange(&m_input_seen, 1); }

int pc_menu_take_input(void) { return InterlockedExchange(&m_input_seen, 0) != 0; }

void pc_menu_test_action(int action) { queue(action); }

void pc_menu_request_video(void) { input_seen(); queue(PCM_ACT_VIDEO); }

static int f_key(unsigned id, int down, int repeat)
{
    int action = 0, take = 0;
    if (id >= 0x200 || id == KEY_F11) return 0;
    if (!down) {
        AcquireSRWLockExclusive(&m_lock);
        take = m_key_swallowed[id];
        m_key_swallowed[id] = 0;
        ReleaseSRWLockExclusive(&m_lock);
        return take;
    }
    if (!pc_menu_active()) {
        /* a key held since before the page (ESC that opened the pause menu) keeps repeating: not ours */
        return 0;
    }
    if (InterlockedCompareExchange(&m_capture, 0, 0)) {
        /* every key goes to the capture; Escape cancels it */
        AcquireSRWLockExclusive(&m_lock);
        m_key_swallowed[id] = 1;
        ReleaseSRWLockExclusive(&m_lock);
        input_seen();
        if (!repeat && id == KEY_ESC) queue(ACT_CANCEL);
        else if (!repeat && InterlockedCompareExchange(&m_capture, 0, 0) == CAP_KEY) queue(ACT_KEY | (int)id);
        return 1;
    }
    switch (id) {
    case KEY_UP: case KEY_W: action = PCM_ACT_UP; break;
    case KEY_DOWN: case KEY_S: action = PCM_ACT_DOWN; break;
    /* the original repeats up and down while held, never left and right (a slider moves one step a press) */
    case KEY_LEFT: case KEY_A: if (!repeat) action = PCM_ACT_LEFT; break;
    case KEY_RIGHT: case KEY_D: if (!repeat) action = PCM_ACT_RIGHT; break;
    case KEY_ENTER: case KEY_NUMENTER: case KEY_SPACE: if (!repeat) action = PCM_ACT_CONFIRM; break;
    case KEY_ESC: case KEY_BACKSPACE: if (!repeat) action = PCM_ACT_BACK; break;
    case KEY_DELETE: if (!repeat) action = ACT_CLEAR; break;
    default: break;
    }
    AcquireSRWLockExclusive(&m_lock);
    if (!repeat) m_key_swallowed[id] = 1;
    take = m_key_swallowed[id];
    ReleaseSRWLockExclusive(&m_lock);
    if (take) input_seen();
    if (take && action) queue(action);
    return take;
}

static int f_button(int button, int down)
{
    int take = 0;
    if (button < 0 || button > 4) return 0;
    AcquireSRWLockExclusive(&m_lock);
    if (!down) {
        take = (m_button_swallowed >> button) & 1u;
        m_button_swallowed &= ~(1u << button);
    } else if (pc_menu_active()) {
        take = 1;
        m_button_swallowed |= 1u << button;
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (take && down) {
        const LONG cap = InterlockedCompareExchange(&m_capture, 0, 0);
        input_seen();
        if (cap) { if (cap == CAP_KEY) queue(ACT_KEY | (int)PC_SOURCE_MOUSE(button)); }
        else if (button == 0) {
            int item, kind = IT_BUTTON, act = PCM_ACT_CONFIRM;
            AcquireSRWLockExclusive(&m_lock);
            pointer_hit_locked();
            item = m_pointer_known ? m_pointer_item : -2;    /* -2: no pointer yet, the keyboard's confirm */
            if (item >= 0 && item < m_hit_n) {
                const Hit *h = &m_hit[item];
                kind = h->kind;
                if (kind == IT_TOGGLE && h->split > 0) act = m_pointer_mx >= h->split ? PCM_ACT_RIGHT : PCM_ACT_LEFT;
                else if (kind == IT_SLIDER) act = m_pointer_mx >= h->split ? PCM_ACT_RIGHT : PCM_ACT_LEFT;
            }
            ReleaseSRWLockExclusive(&m_lock);
            if (item >= 0) { queue(PCM_ACT_HOVER + item); queue(act); }
            else if (item == -2) queue(PCM_ACT_CONFIRM);
        } else if (button == 1) queue(PCM_ACT_BACK);
    }
    return take;
}

void pc_menu_pointer(int x, int y)
{
    int item;
    if (!pc_menu_active()) return;
    AcquireSRWLockExclusive(&m_lock);
    if (m_xf_s <= 0) { ReleaseSRWLockExclusive(&m_lock); return; }
    m_pointer_x = x; m_pointer_y = y;
    m_pointer_known = 1;
    pointer_hit_locked();
    item = m_pointer_item;
    ReleaseSRWLockExclusive(&m_lock);
    if (item >= 0) { input_seen(); queue(PCM_ACT_HOVER + item); }
}

static int f_wheel(int delta)
{
    LONG cap;
    if (!pc_menu_active()) return 0;
    input_seen();
    cap = InterlockedCompareExchange(&m_capture, 0, 0);
    if (cap) { if (cap == CAP_KEY) queue(ACT_KEY | (int)(delta > 0 ? PC_SOURCE_WHEEL_UP : PC_SOURCE_WHEEL_DOWN)); }
    else queue(delta > 0 ? PCM_ACT_UP : PCM_ACT_DOWN);
    return 1;
}

enum { P_UP = 1, P_DOWN = 2, P_LEFT = 4, P_RIGHT = 8, P_A = 16, P_B = 32, P_START = 64, P_X = 128, P_ALL = 255 };

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
    if (out[2] & 0x10) b |= P_START;
    if (out[6] > 100) b |= P_X;
    return b;
}

/* Activity includes controls the menu does not navigate with. They are swallowed too, so the
 * game's idle check cannot otherwise see a held trigger, stick, or other controller button. */
static int pad_activity(const uint8_t out[20])
{
    int i;
    if (out[2]) return 1;
    /* Retail idle activity is >0.1 after its default input normalization: face .05..95,
     * triggers .1..9, and stick deadzone .2 with maximum .9 (sub_000C5A80). */
    for (i = 4; i < 12; i++) if (out[i] >= (i < 10 ? 36 : 46)) return 1;
    for (i = 12; i < 20; i += 2) {
        const int axis = (int16_t)(out[i] | (out[i + 1] << 8));
        if (axis >= 8848 || axis <= -8848) return 1;
    }
    return 0;
}

static int f_report(uint8_t out[20], int pad_present)
{
    const unsigned held = pad_present ? pad_bits(out) : 0;
    const int activity = pad_present && pad_activity(out);
    const ULONGLONG now = GetTickCount64();
    const int active = pc_menu_active();
    const LONG cap = active ? InterlockedCompareExchange(&m_capture, 0, 0) : 0;
    const uint32_t controls = pad_present ? pc_controls_pad_held(out) : 0;
    unsigned edges;
    uint32_t control_edges;
    int actions[8], n = 0, i, take;
    AcquireSRWLockExclusive(&m_lock);
    edges = held & ~m_pad_prev;
    control_edges = controls & ~m_pad_controls_prev;
    m_pad_controls_prev = controls;
    if (cap) {
        /* a capture: the first control pressed now (START cancels a button capture), B cancels a key capture */
        if (cap == CAP_PAD) {
            if (control_edges & (1u << 12)) actions[n++] = ACT_CANCEL;
            else for (i = 0; i < PCC_CONTROLS; i++) if (control_edges & (1u << i)) { actions[n++] = ACT_PAD | i; break; }
        } else if (edges & P_B) actions[n++] = ACT_CANCEL;
    } else if (active) {
        static const unsigned dir_bit[4] = { P_UP, P_DOWN, P_LEFT, P_RIGHT };
        static const int dir_action[4] = { PCM_ACT_UP, PCM_ACT_DOWN, PCM_ACT_LEFT, PCM_ACT_RIGHT };
        for (i = 0; i < 4; i++) {
            if (!(held & dir_bit[i])) continue;
            if (edges & dir_bit[i]) { actions[n++] = dir_action[i]; m_pad_repeat[i] = now + 400; }
            else if (i < 2 && now >= m_pad_repeat[i]) { actions[n++] = dir_action[i]; m_pad_repeat[i] = now + 90; }
        }
        if (edges & P_A) actions[n++] = PCM_ACT_CONFIRM;
        if (edges & P_B) actions[n++] = PCM_ACT_BACK;
        if (edges & P_START) actions[n++] = PCM_ACT_START;
        if (edges & P_X) actions[n++] = ACT_CLEAR;
    }
    /* The buttons that left a page belong to it until they are let go: the game never sees that press. */
    if (active) {
        m_pad_hold = (held & (P_A | P_B | P_START)) != 0;
        if (activity || m_button_swallowed) input_seen();
    }
    m_pad_prev = held;
    take = active || m_pad_hold;
    if (!active && m_pad_hold && !(held & P_ALL)) m_pad_hold = 0;
    ReleaseSRWLockExclusive(&m_lock);
    for (i = 0; i < n; i++) queue(actions[i]);
    return take;
}

static const PcInputFilter k_filter = { f_key, f_button, f_wheel, f_report, NULL };

void pc_menu_install(void)
{
    pc_input_set_filter_slot(1, &k_filter);
    pc_video_menu_set_native_opener(disabled() ? NULL : pc_menu_request_video);
}

/* ---- drawing (presenter thread) ------------------------------------------------------------------------------- */

typedef struct Canvas { uint32_t *px; int w, h; float ox, oy, s; int cl, ct, cr, cb; } Canvas;

static inline void blend(uint32_t *d, uint32_t s)
{
    const unsigned sa = s >> 24;
    uint32_t v = *d;
    unsigned inv;
    if (!sa) return;
    if (sa == 255) { *d = s; return; }
    inv = 255 - sa;
    {
        const unsigned rb = ((v & 0x00FF00FFu) * inv + 0x00800080u), ag = (((v >> 8) & 0x00FF00FFu) * inv + 0x00800080u);
        const uint32_t rb2 = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        const uint32_t ag2 = ((ag + ((ag >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        *d = s + (rb2 | (ag2 << 8));
    }
}

static void draw_rect(const Canvas *c, const Prim *p, float slide)
{
    float x0 = c->ox + (p->x + slide) * c->s, y0 = c->oy + p->y * c->s, x1 = x0 + p->w * c->s, y1 = y0 + p->h * c->s;
    int ix0, iy0, ix1, iy1, x, y;
    const uint32_t pm = premultiply(p->colour);
    if (p->flags & PF_EXT_LEFT) x0 = (float)c->cl;
    if (p->flags & PF_EXT_RIGHT) x1 = (float)c->cr;
    if (p->flags & PF_EXT_UP) y0 = (float)c->ct;
    if (p->flags & PF_EXT_DOWN) y1 = (float)c->cb;
    ix0 = (int)floorf(x0 + 0.5f); iy0 = (int)floorf(y0 + 0.5f); ix1 = (int)floorf(x1 + 0.5f); iy1 = (int)floorf(y1 + 0.5f);
    if (ix1 <= ix0 && p->w > 0) ix1 = ix0 + 1;                     /* a line never disappears */
    if (iy1 <= iy0 && p->h > 0) iy1 = iy0 + 1;
    if (ix0 < c->cl) ix0 = c->cl;
    if (iy0 < c->ct) iy0 = c->ct;
    if (ix1 > c->cr) ix1 = c->cr;
    if (iy1 > c->cb) iy1 = c->cb;
    for (y = iy0; y < iy1; y++) {
        uint32_t *row = c->px + (size_t)y * c->w;
        if (p->flags & PF_STORE) for (x = ix0; x < ix1; x++) row[x] = pm;
        else for (x = ix0; x < ix1; x++) blend(row + x, pm);
    }
}

/* One axis of a bilinear lookup for each destination pixel: the first texel, its weight out of 256 and whether
 * each of the two texels is inside the cell (outside the cell is empty, so neighbouring glyphs never bleed in). */
typedef struct Tap { int t; unsigned w0, w1; } Tap;

static int taps(Tap *out, int d0, int d1, float dst, float dsz, float src, float ssz, int limit)
{
    const int lo = (int)ceilf(src - 0.01f), hi = (int)ceilf(src + ssz - 0.01f) - 1;   /* texels of the cell */
    const float step = ssz / dsz;
    int i;
    for (i = d0; i < d1; i++) {
        const float t = src + ((float)i + 0.5f - dst) * step - 0.5f;
        const int it = (int)floorf(t);
        const unsigned f = (unsigned)((t - (float)it) * 256.0f + 0.5f);
        Tap *k = &out[i - d0];
        k->t = it;
        k->w0 = (it >= lo && it <= hi && it >= 0 && it < limit) ? 256u - (f > 256u ? 256u : f) : 0u;
        k->w1 = (it + 1 >= lo && it + 1 <= hi && it + 1 >= 0 && it + 1 < limit) ? (f > 256u ? 256u : f) : 0u;
        if (k->t < 0) k->t = 0;                       /* a zero weight never reads it */
        if (k->t > limit - 2) k->t = limit - 2 < 0 ? 0 : limit - 2;
        if (it != k->t) {                            /* clamped at the image's edge: keep the weights on the right texels */
            if (it < k->t) { k->w0 = k->w1; k->w1 = 0; }   /* texel it + 1 is now the first */
            else { k->w1 = k->w0; k->w0 = 0; }             /* texel it is now the second */
        }
    }
    return 1;
}

/* Draws the texel cell (u, v, cw, ch) of img into the window rectangle (dx, dy, dw, dh), tinted by colour.
 * `crisp` > 1 steepens the edges of magnified alpha (glyphs are tiny bitmaps drawn several times larger).
 * Bilinear, in fixed point: the taps of every column and row are worked out once. */
static void draw_cell(const Canvas *c, const uint32_t *img, unsigned iw, unsigned ih, float u, float v, float cw, float ch,
                      float dx, float dy, float dw, float dh, uint32_t colour, float crisp)
{
    static Tap *xt, *yt;
    static int cap;
    const unsigned cr = (colour >> 16) & 255u, cg = (colour >> 8) & 255u, cb = colour & 255u, ca = colour >> 24;
    unsigned lut[256];                                  /* alpha -> sharpened alpha (0..255) */
    unsigned gain[256];                                 /* alpha -> colour gain keeping premultiplied colour <= alpha, 8.8 */
    int x0 = (int)floorf(dx), y0 = (int)floorf(dy), x1 = (int)ceilf(dx + dw), y1 = (int)ceilf(dy + dh), x, y, a;
    if (dw <= 0 || dh <= 0 || iw < 1 || ih < 1) return;
    if (x0 < c->cl) x0 = c->cl;
    if (y0 < c->ct) y0 = c->ct;
    if (x1 > c->cr) x1 = c->cr;
    if (y1 > c->cb) y1 = c->cb;
    if (x1 <= x0 || y1 <= y0) return;
    if (x1 - x0 > cap || y1 - y0 > cap) {               /* presenter thread only: one pair of buffers */
        const int n = (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) + 64;
        Tap *nx = (Tap *)realloc(xt, sizeof(Tap) * (size_t)n), *ny;
        if (!nx) return;
        xt = nx;
        ny = (Tap *)realloc(yt, sizeof(Tap) * (size_t)n);
        if (!ny) return;
        yt = ny; cap = n;
    }
    taps(xt, x0, x1, dx, dw, u, cw, (int)iw);
    taps(yt, y0, y1, dy, dh, v, ch, (int)ih);
    for (a = 0; a < 256; a++) {
        float na = (float)a / 255.0f;
        if (crisp > 1.0f) { na = 0.5f + (na - 0.5f) * crisp; if (na < 0) na = 0; if (na > 1) na = 1; }
        lut[a] = (unsigned)(na * 255.0f + 0.5f);
        gain[a] = a ? (lut[a] * 256u + (unsigned)a / 2u) / (unsigned)a : 0u;
    }
    for (y = y0; y < y1; y++) {
        const Tap *ty = &yt[y - y0];
        const uint32_t *r0 = img + (size_t)ty->t * iw, *r1 = ty->t + 1 < (int)ih ? r0 + iw : r0;
        uint32_t *row = c->px + (size_t)y * c->w;
        if (!ty->w0 && !ty->w1) continue;
        for (x = x0; x < x1; x++) {
            const Tap *tx = &xt[x - x0];
            const unsigned w00 = tx->w0 * ty->w0, w10 = tx->w1 * ty->w0, w01 = tx->w0 * ty->w1, w11 = tx->w1 * ty->w1;
            const int t1 = tx->t + 1 < (int)iw ? tx->t + 1 : tx->t;
            const uint32_t p00 = r0[tx->t], p10 = r0[t1], p01 = r1[tx->t], p11 = r1[t1];
            unsigned sa, sr, sg, sb, al, k;
            sa = ((p00 >> 24) * w00 + (p10 >> 24) * w10 + (p01 >> 24) * w01 + (p11 >> 24) * w11 + 32768u) >> 16;
            if (!sa) continue;
            sr = (((p00 >> 16) & 255u) * w00 + ((p10 >> 16) & 255u) * w10 + ((p01 >> 16) & 255u) * w01 + ((p11 >> 16) & 255u) * w11 + 32768u) >> 16;
            sg = (((p00 >> 8) & 255u) * w00 + ((p10 >> 8) & 255u) * w10 + ((p01 >> 8) & 255u) * w01 + ((p11 >> 8) & 255u) * w11 + 32768u) >> 16;
            sb = ((p00 & 255u) * w00 + (p10 & 255u) * w10 + (p01 & 255u) * w01 + (p11 & 255u) * w11 + 32768u) >> 16;
            if (sa > 255u) sa = 255u;
            k = gain[sa];
            al = (lut[sa] * ca + 127u) / 255u;
            if (!al) continue;
            sr = ((sr * k) >> 8) * cr / 255u * ca / 255u;
            sg = ((sg * k) >> 8) * cg / 255u * ca / 255u;
            sb = ((sb * k) >> 8) * cb / 255u * ca / 255u;
            if (sr > al) sr = al;
            if (sg > al) sg = al;
            if (sb > al) sb = al;
            blend(row + x, (al << 24) | (sr << 16) | (sg << 8) | sb);
        }
    }
}

static void draw_text(const Canvas *c, const Prim *p, float slide)
{
    const Font *f = &m_font[p->font < PCM_FONT_COUNT ? p->font : 0];
    const float k = c->s * p->size / f->nominal * p->scale;     /* window pixels per texel */
    float pen = c->ox + (p->x + slide) * c->s;
    const float top = c->oy + p->y * c->s;
    const float crisp = k > 1.25f ? (k < 4.0f ? k * 0.75f : 3.0f) : 1.0f;
    const uint16_t *t;
    if (!f->pixels) return;
    for (t = p->text; *t; t++) {
        const PcMenuGlyph *g = glyph(f, *t);
        if (!g) continue;
        if (g->w > 0 && g->h > 0)
            draw_cell(c, f->pixels, f->w, f->h, g->u, g->v, g->w, g->h, pen + g->ox * k, top + (g->oy - f->cap_oy) * k,
                      g->w * k, g->h * k, p->colour, crisp);
        pen += g->advance * k;
    }
}

static void draw_image(const Canvas *c, const Prim *p, float slide)
{
    const Image *im = &m_image[p->image < PCM_IMAGE_COUNT ? p->image : 0];
    if (!im->pixels) return;
    draw_cell(c, im->pixels, im->w, im->h, 0, 0, (float)im->w, (float)im->h, c->ox + (p->x + slide) * c->s, c->oy + p->y * c->s,
              p->w * c->s, p->h * c->s, p->colour, 1.0f);
}

/* A filled triangle with its edges antialiased (4 x 4 samples per pixel). */
static void draw_tri(const Canvas *c, const Prim *p, float slide)
{
    const float X[3] = { c->ox + (p->x + slide) * c->s, c->ox + (p->w + slide) * c->s, c->ox + (p->size + slide) * c->s };
    const float Y[3] = { c->oy + p->y * c->s, c->oy + p->h * c->s, c->oy + p->scale * c->s };
    const float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    const unsigned ca = p->colour >> 24;
    int x0 = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), x1 = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
    int y0 = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), y1 = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
    int x, y, sx, sy;
    if (fabsf(area) < 1e-3f) return;
    if (x0 < c->cl) x0 = c->cl;
    if (y0 < c->ct) y0 = c->ct;
    if (x1 > c->cr) x1 = c->cr;
    if (y1 > c->cb) y1 = c->cb;
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) {
        int hits = 0;
        for (sy = 0; sy < 4; sy++) for (sx = 0; sx < 4; sx++) {
            const float qx = (float)x + (sx + 0.5f) / 4.0f, qy = (float)y + (sy + 0.5f) / 4.0f;
            const float e0 = (X[1] - X[0]) * (qy - Y[0]) - (Y[1] - Y[0]) * (qx - X[0]);
            const float e1 = (X[2] - X[1]) * (qy - Y[1]) - (Y[2] - Y[1]) * (qx - X[1]);
            const float e2 = (X[0] - X[2]) * (qy - Y[2]) - (Y[0] - Y[2]) * (qx - X[2]);
            if (area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0)) hits++;
        }
        if (hits) {
            const unsigned a = ca * (unsigned)hits / 16u;
            const uint32_t s = premultiply((p->colour & 0x00FFFFFFu) | (a << 24));
            blend(c->px + (size_t)y * c->w + x, s);
        }
    }
}

/* The 640 x 480 menu space, centred in the picture at the picture's height (or its width, when narrower). */
static void canvas_for(Canvas *c, uint32_t *px, unsigned w, unsigned h, int px0, int py0, unsigned pw, unsigned ph)
{
    c->px = px; c->w = (int)w; c->h = (int)h;
    c->cl = px0 < 0 ? 0 : px0; c->ct = py0 < 0 ? 0 : py0;
    c->cr = px0 + (int)pw > (int)w ? (int)w : px0 + (int)pw; c->cb = py0 + (int)ph > (int)h ? (int)h : py0 + (int)ph;
    if ((float)pw * 3.0f >= (float)ph * 4.0f) {
        c->s = (float)ph / 480.0f;
        c->ox = (float)px0 + ((float)pw - 640.0f * c->s) * 0.5f; c->oy = (float)py0;
    } else {
        c->s = (float)pw / 640.0f;
        c->ox = (float)px0; c->oy = (float)py0 + ((float)ph - 480.0f * c->s) * 0.5f;
    }
}

/* the shade and the rails stay put; the page's contents slide in */
static float prim_slide(const Display *d, const Prim *p)
{
    return (p->type == PR_RECT && (p->flags & (PF_EXT_UP | PF_EXT_DOWN))) ? 0.0f : d->slide;
}

typedef struct Box { int x0, y0, x1, y1; } Box;

/* The window pixels a primitive can touch, within the picture. 0 when none. */
static int prim_box(const Canvas *c, const Prim *p, float slide, Box *b)
{
    float x0, y0, x1, y1;
    int pad = 2;
    if (p->type == PR_RECT) {
        x0 = c->ox + (p->x + slide) * c->s; y0 = c->oy + p->y * c->s; x1 = x0 + p->w * c->s; y1 = y0 + p->h * c->s;
        if (p->flags & PF_EXT_LEFT) x0 = (float)c->cl;
        if (p->flags & PF_EXT_RIGHT) x1 = (float)c->cr;
        if (p->flags & PF_EXT_UP) y0 = (float)c->ct;
        if (p->flags & PF_EXT_DOWN) y1 = (float)c->cb;
    } else if (p->type == PR_TEXT) {
        const Font *f = &m_font[p->font < PCM_FONT_COUNT ? p->font : 0];
        const float k = f->pixels ? c->s * p->size / f->nominal * p->scale : 0.0f;
        float pen = c->ox + (p->x + slide) * c->s;
        const float top = c->oy + p->y * c->s;
        const uint16_t *t;
        x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        for (t = p->text; *t && k > 0; t++) {
            const PcMenuGlyph *g = glyph(f, *t);
            if (!g) continue;
            if (g->w > 0 && g->h > 0) {
                const float gx = pen + g->ox * k, gy = top + (g->oy - f->cap_oy) * k;
                x0 = fminf(x0, gx); y0 = fminf(y0, gy); x1 = fmaxf(x1, gx + g->w * k); y1 = fmaxf(y1, gy + g->h * k);
            }
            pen += g->advance * k;
        }
        if (x1 < x0) return 0;
    } else if (p->type == PR_TRI) {
        x0 = c->ox + (fminf(p->x, fminf(p->w, p->size)) + slide) * c->s; x1 = c->ox + (fmaxf(p->x, fmaxf(p->w, p->size)) + slide) * c->s;
        y0 = c->oy + fminf(p->y, fminf(p->h, p->scale)) * c->s; y1 = c->oy + fmaxf(p->y, fmaxf(p->h, p->scale)) * c->s;
    } else {
        x0 = c->ox + (p->x + slide) * c->s; y0 = c->oy + p->y * c->s; x1 = x0 + p->w * c->s; y1 = y0 + p->h * c->s;
    }
    b->x0 = (int)floorf(x0) - pad; b->y0 = (int)floorf(y0) - pad; b->x1 = (int)ceilf(x1) + pad; b->y1 = (int)ceilf(y1) + pad;
    if (b->x0 < c->cl) b->x0 = c->cl;
    if (b->y0 < c->ct) b->y0 = c->ct;
    if (b->x1 > c->cr) b->x1 = c->cr;
    if (b->y1 > c->cb) b->y1 = c->cb;
    return b->x1 > b->x0 && b->y1 > b->y0;
}

/* Clears region r of the buffer and draws every primitive that reaches into it, clipped to it. */
static void render_region(const Display *d, const Canvas *full, Box r)
{
    Canvas c = *full;
    int i, y;
    if (r.x0 > c.cl) c.cl = r.x0;
    if (r.y0 > c.ct) c.ct = r.y0;
    if (r.x1 < c.cr) c.cr = r.x1;
    if (r.y1 < c.cb) c.cb = r.y1;
    if (c.cr <= c.cl || c.cb <= c.ct) return;
    for (y = c.ct; y < c.cb; y++) memset(c.px + (size_t)y * c.w + c.cl, 0, (size_t)(c.cr - c.cl) * 4);
    for (i = 0; i < d->n; i++) {
        const Prim *p = &d->prim[i];
        const float slide = prim_slide(d, p);
        Box b;
        if (!prim_box(full, p, slide, &b) || b.x1 <= c.cl || b.x0 >= c.cr || b.y1 <= c.ct || b.y0 >= c.cb) continue;
        if (p->type == PR_RECT) draw_rect(&c, p, slide);
        else if (p->type == PR_TEXT) draw_text(&c, p, slide);
        else if (p->type == PR_TRI) draw_tri(&c, p, slide);
        else draw_image(&c, p, slide);
    }
}

static void render(const Display *d, uint32_t *px, unsigned w, unsigned h, int px0, int py0, unsigned pw, unsigned ph)
{
    Canvas c;
    Box all;
    memset(px, 0, (size_t)w * h * 4);
    canvas_for(&c, px, w, h, px0, py0, pw, ph);
    all.x0 = c.cl; all.y0 = c.ct; all.x1 = c.cr; all.y1 = c.cb;
    render_region(d, &c, all);
}

#define PCM_BOXES 64

static void box_add(Box *boxes, int *n, Box b, int *overflow)
{
    if (*n >= PCM_BOXES) { *overflow = 1; return; }
    boxes[(*n)++] = b;
}

/* What changed between two display lists, as a few merged boxes. Returns -1 when it is simpler to redraw everything. */
static int diff_boxes(const Canvas *c, const Display *a, const Display *b, Box *out)
{
    int i, n = 0, overflow = 0, merged;
    const int count = a->n > b->n ? a->n : b->n;
    long long area = 0, full = (long long)(c->cr - c->cl) * (c->cb - c->ct);
    for (i = 0; i < count && !overflow; i++) {
        const Prim *pa = i < a->n ? &a->prim[i] : NULL, *pb = i < b->n ? &b->prim[i] : NULL;
        Box bx;
        /* while the page slides in, everything but the shade and the rails moves */
        if (pa && pb && !memcmp(pa, pb, sizeof(Prim)) && prim_slide(a, pa) == prim_slide(b, pb)) continue;
        if (pa && prim_box(c, pa, prim_slide(a, pa), &bx)) box_add(out, &n, bx, &overflow);
        if (pb && prim_box(c, pb, prim_slide(b, pb), &bx)) box_add(out, &n, bx, &overflow);
    }
    if (overflow) return -1;
    do {   /* merge boxes that touch or nearly touch: fewer, larger regions redraw faster than many slivers */
        int j;
        merged = 0;
        for (i = 0; i < n && !merged; i++) for (j = i + 1; j < n && !merged; j++) {
            const Box p = out[i], q = out[j];
            if (p.x0 - 8 <= q.x1 && q.x0 - 8 <= p.x1 && p.y0 - 8 <= q.y1 && q.y0 - 8 <= p.y1) {
                out[i].x0 = p.x0 < q.x0 ? p.x0 : q.x0; out[i].y0 = p.y0 < q.y0 ? p.y0 : q.y0;
                out[i].x1 = p.x1 > q.x1 ? p.x1 : q.x1; out[i].y1 = p.y1 > q.y1 ? p.y1 : q.y1;
                out[j] = out[--n];
                merged = 1;
            }
        }
    } while (merged);
    for (i = 0; i < n; i++) area += (long long)(out[i].x1 - out[i].x0) * (out[i].y1 - out[i].y0);
    return area * 10 > full * 6 ? -1 : n;
}

/* ---- the overlay stack ------------------------------------------------------------------------------------------- */

static Display m_draw, m_next;                /* the presenter's copies: what is in m_buf, and the newest */
static uint32_t *m_buf, *m_comp;
static unsigned m_buf_w, m_buf_h, m_drawn_gen = (unsigned)-1;
static int m_drawn_rect[4], m_last_visible;
static Box m_dirty;                           /* what the last changed overlay changed (x1 = 0: everything) */

int pc_menu_overlay_dirty(unsigned box[4])
{
    if (m_dirty.x1 <= m_dirty.x0 || m_dirty.y1 <= m_dirty.y0) return 0;
    box[0] = (unsigned)m_dirty.x0; box[1] = (unsigned)m_dirty.y0;
    box[2] = (unsigned)(m_dirty.x1 - m_dirty.x0); box[3] = (unsigned)(m_dirty.y1 - m_dirty.y0);
    return 1;
}

static int page_overlay(unsigned w, unsigned h, int px, int py, unsigned pw, unsigned ph, const uint32_t **pixels, int *changed)
{
    const uint32_t *video = NULL;
    int video_changed = 0, video_on, menu_on, menu_changed = 0, nboxes = -1, i;
    unsigned gen;
    Box boxes[PCM_BOXES];
    m_overlay_seen = GetTickCount64();
    m_dirty.x0 = m_dirty.y0 = m_dirty.x1 = m_dirty.y1 = 0;
    if (pw && ph) {
        Canvas xf;
        canvas_for(&xf, NULL, w, h, px, py, pw, ph);
        AcquireSRWLockExclusive(&m_lock);
        m_xf_ox = xf.ox; m_xf_oy = xf.oy; m_xf_s = xf.s;
        pointer_hit_locked();
        ReleaseSRWLockExclusive(&m_lock);
    }
    video_on = pc_video_menu_overlay(w, h, &video, &video_changed);
    menu_on = pc_menu_active();
    if (!menu_on) {
        *changed = video_changed || m_last_visible;
        if (m_last_visible && !video_on) { m_last_visible = 0; return 0; }
        m_last_visible = 0;
        m_drawn_gen = (unsigned)-1;
        if (video_on) *pixels = video;
        return video_on;
    }
    if (!w || !h) return 0;
    if (w != m_buf_w || h != m_buf_h || !m_buf) {
        free(m_buf); free(m_comp);
        m_buf = (uint32_t *)malloc((size_t)w * h * 4);
        m_comp = (uint32_t *)malloc((size_t)w * h * 4);
        if (!m_buf || !m_comp) { free(m_buf); free(m_comp); m_buf = m_comp = NULL; m_buf_w = m_buf_h = 0; return 0; }
        m_buf_w = w; m_buf_h = h; m_drawn_gen = (unsigned)-1;
    }
    AcquireSRWLockShared(&m_lock);
    gen = m_generation;
    if (gen != m_drawn_gen || m_drawn_rect[0] != px || m_drawn_rect[1] != py || m_drawn_rect[2] != (int)pw || m_drawn_rect[3] != (int)ph) {
        m_next.n = m_display.n;
        m_next.slide = m_display.slide;
        memcpy(m_next.prim, m_display.prim, sizeof(Prim) * (size_t)m_display.n);
        menu_changed = 1;
    }
    ReleaseSRWLockShared(&m_lock);
    if (menu_changed) {
        static int trace = -1;
        static unsigned traced;
        const double t0 = now_s();
        Canvas c;
        canvas_for(&c, m_buf, w, h, px, py, pw, ph);
        /* Only what changed is drawn again (a focus move touches a few rows); a new page or picture is drawn whole. */
        if (m_drawn_gen != (unsigned)-1 && m_drawn_rect[0] == px && m_drawn_rect[1] == py && m_drawn_rect[2] == (int)pw &&
            m_drawn_rect[3] == (int)ph && m_last_visible)
            nboxes = diff_boxes(&c, &m_draw, &m_next, boxes);
        if (nboxes < 0) {
            render(&m_next, m_buf, w, h, px, py, pw, ph);
            m_dirty.x0 = 0; m_dirty.y0 = 0; m_dirty.x1 = (int)w; m_dirty.y1 = (int)h;
        } else {
            for (i = 0; i < nboxes; i++) {
                render_region(&m_next, &c, boxes[i]);
                if (!i) m_dirty = boxes[0];
                else {
                    if (boxes[i].x0 < m_dirty.x0) m_dirty.x0 = boxes[i].x0;
                    if (boxes[i].y0 < m_dirty.y0) m_dirty.y0 = boxes[i].y0;
                    if (boxes[i].x1 > m_dirty.x1) m_dirty.x1 = boxes[i].x1;
                    if (boxes[i].y1 > m_dirty.y1) m_dirty.y1 = boxes[i].y1;
                }
            }
        }
        m_draw.n = m_next.n;
        m_draw.slide = m_next.slide;
        memcpy(m_draw.prim, m_next.prim, sizeof(Prim) * (size_t)m_next.n);
        if (trace < 0) { const char *e = getenv("RECOMP_MENU_TRACE"); trace = e && *e && *e != '0'; }
        if (trace && traced++ < 400)          /* lab: what a redraw costs the presenter */
            fprintf(stderr, "[MENU] overlay %ux%u: %s in %.2f ms (%d primitives, changed %dx%d)\n", w, h,
                    nboxes < 0 ? "drawn whole" : "redrawn in part", (now_s() - t0) * 1000.0, m_draw.n,
                    m_dirty.x1 - m_dirty.x0, m_dirty.y1 - m_dirty.y0);
        m_drawn_gen = gen;
        m_drawn_rect[0] = px; m_drawn_rect[1] = py; m_drawn_rect[2] = (int)pw; m_drawn_rect[3] = (int)ph;
    }
    if (video_on && video) {
        if (video_changed || m_last_visible != 2) {
            const size_t n = (size_t)w * h;
            size_t k;
            memcpy(m_comp, m_buf, n * 4);
            for (k = 0; k < n; k++) blend(m_comp + k, video[k]);
            m_dirty.x0 = 0; m_dirty.y0 = 0; m_dirty.x1 = (int)w; m_dirty.y1 = (int)h;
        } else if (menu_changed) {
            int y, x;
            for (y = m_dirty.y0; y < m_dirty.y1; y++) {
                const size_t row = (size_t)y * w;
                memcpy(m_comp + row + m_dirty.x0, m_buf + row + m_dirty.x0, (size_t)(m_dirty.x1 - m_dirty.x0) * 4);
                for (x = m_dirty.x0; x < m_dirty.x1; x++) blend(m_comp + row + x, video[row + x]);
            }
        }
        *pixels = m_comp;
        *changed = menu_changed || video_changed || m_last_visible != 2;
        if (m_last_visible != 2) { m_dirty.x0 = 0; m_dirty.y0 = 0; m_dirty.x1 = (int)w; m_dirty.y1 = (int)h; }
        m_last_visible = 2;
        return 1;
    }
    *pixels = m_buf;
    *changed = menu_changed || m_last_visible != 1;
    if (m_last_visible != 1) { m_dirty.x0 = 0; m_dirty.y0 = 0; m_dirty.x1 = (int)w; m_dirty.y1 = (int)h; }
    m_last_visible = 1;
    return 1;
}

/* The hint is a separate layer: its pixels never enter the cached page pixels, so hiding it
 * restores exactly the page underneath. A change to this layer invalidates the whole upload;
 * ordinary pages retain their existing small dirty regions while the hint is hidden. */
#define PCM_CAPTION_ROWS 5

/* Wrap at the last fitting space; split an overlong word without losing characters. */
static unsigned caption_rows(const uint16_t *text, uint16_t rows[PCM_CAPTION_ROWS][PCM_TEXT_CAP], float limit)
{
    unsigned count = 0, at = 0;
    memset(rows, 0, sizeof(uint16_t) * PCM_CAPTION_ROWS * PCM_TEXT_CAP);
    while (text[at] && count < PCM_CAPTION_ROWS) {
        unsigned start, n = 0, space = 0;
        while (text[at] == ' ' || text[at] == '\t') ++at;
        if (!text[at]) break;
        start = at;
        while (text[at] && text[at] != '\n' && text[at] != '\r' && n + 1 < PCM_TEXT_CAP) {
            rows[count][n++] = text[at];
            rows[count][n] = 0;
            if (n > 1 && text_width(PCM_FONT_SMALL, rows[count], 17.0f) > limit) {
                rows[count][--n] = 0;
                if (space) { n = space; at = start + space + 1; }
                break;
            }
            if (text[at] == ' ' || text[at] == '\t') space = n - 1;
            ++at;
        }
        while (n && (rows[count][n - 1] == ' ' || rows[count][n - 1] == '\t')) --n;
        rows[count][n] = 0;
        if (text[at] == '\r') ++at;
        if (text[at] == '\n') ++at;
        ++count;
    }
    return count;
}

int pc_menu_overlay(unsigned w, unsigned h, int px, int py, unsigned pw, unsigned ph, const uint32_t **pixels, int *changed)
{
    static uint32_t *hint, *composed;
    static uint16_t previous_caption[PC_SPEECH_SUBTITLE_CHARS];
    static int caption_rect[4];
    static unsigned hw, hh;
    static int overlay_was_on, hint_was_on, base_was_on, caption_was_on;
    const uint32_t *base = NULL;
    uint16_t caption[PC_SPEECH_SUBTITLE_CHARS], speech_caption[PC_SPEECH_SUBTITLE_CHARS];
    int base_changed = 0, overlay_changed = 0, caption_changed;
    const int base_on = page_overlay(w, h, px, py, pw, ph, &base, &base_changed);
    int hint_on = InterlockedCompareExchange(&m_movie_skip_prompt, 0, 0) && w && h && pw && ph &&
                  m_font[PCM_FONT_SMALL].pixels;
    int subtitle_on, movie_on;
    memset(caption, 0, sizeof(caption));
    /* Poll even when fonts/picture are not ready, so pause timing stays current.
     * A cinematic owns the screen; its subtitles take priority over speech. */
    subtitle_on = pc_speech_subtitle(speech_caption, PC_SPEECH_SUBTITLE_CHARS,
                                     base_on || pc_movie_skip_active());
    movie_on = pc_movie_skip_subtitle(caption, PC_SPEECH_SUBTITLE_CHARS);
    if (movie_on) subtitle_on = 1;
    else memcpy(caption, speech_caption, sizeof(caption));
    subtitle_on = subtitle_on && pc_subtitles_enabled() && w && h && pw && ph && m_font[PCM_FONT_SMALL].pixels;
    caption_changed = subtitle_on != caption_was_on || memcmp(caption, previous_caption, sizeof(caption)) != 0;
    if (subtitle_on && (caption_rect[0] != px || caption_rect[1] != py ||
                        caption_rect[2] != (int)pw || caption_rect[3] != (int)ph)) {
        overlay_changed = 1;
        caption_rect[0] = px; caption_rect[1] = py; caption_rect[2] = (int)pw; caption_rect[3] = (int)ph;
    }
    if (caption_changed) memcpy(previous_caption, caption, sizeof(caption));
    caption_was_on = subtitle_on;
    if ((hint_on || subtitle_on) && (hw != w || hh != h || !hint || !composed)) {
        uint32_t *next_hint = (uint32_t *)malloc((size_t)w * h * 4);
        uint32_t *next_composed = (uint32_t *)malloc((size_t)w * h * 4);
        if (!next_hint || !next_composed) { free(next_hint); free(next_composed); hint_on = subtitle_on = 0; }
        else {
            free(hint); free(composed);
            hint = next_hint; composed = next_composed; hw = w; hh = h;
            overlay_changed = 1;
        }
    }
    if (!hint_on && !subtitle_on) {
        *changed = base_changed || overlay_was_on;
        if (overlay_was_on && base_on) { m_dirty.x0 = m_dirty.y0 = 0; m_dirty.x1 = (int)w; m_dirty.y1 = (int)h; }
        overlay_was_on = hint_was_on = 0; base_was_on = base_on;
        if (base_on) *pixels = base;
        return base_on;
    }
    overlay_changed |= !overlay_was_on || caption_changed || hint_on != hint_was_on;
    if (overlay_changed) {
        static Display d;
        static const uint16_t english_skip_text[] = { 'P','r','e','s','s',' ','a','g','a','i','n',' ','t','o',' ','s','k','i','p',0 };
        const char *language = getenv("RECOMP_LANGUAGE");
        const char *skip_text = "Press again to skip";
        uint16_t localized_skip_text[64];
        const uint16_t *text = english_skip_text;
        if (language && !_stricmp(language, "pt-BR")) skip_text = "PRESSIONE NOVAMENTE PARA PULAR";
        else if (language && !_stricmp(language, "es")) skip_text = "PULSA DE NUEVO PARA OMITIR";
        else if (language && !_stricmp(language, "fr")) skip_text = "APPUYEZ À NOUVEAU POUR PASSER";
        else if (language && !_stricmp(language, "de")) skip_text = "ZUM ÜBERSPRINGEN ERNEUT DRÜCKEN";
        else if (language && !_stricmp(language, "it")) skip_text = "PREMI DI NUOVO PER SALTARE";
        if (language && (_stricmp(language, "pt-BR") == 0 || _stricmp(language, "es") == 0 ||
                         _stricmp(language, "fr") == 0 || _stricmp(language, "de") == 0 || _stricmp(language, "it") == 0)) {
            if (MultiByteToWideChar(CP_UTF8, 0, skip_text, -1, (WCHAR *)localized_skip_text, 64) > 0)
                text = localized_skip_text;
        }
        Canvas c;
        float right, bottom, tw;
        d.n = 0; d.slide = 0;
        if (hint_on) {
            /* The skip prompt belongs to the client area, including letterbox bars. */
            canvas_for(&c, hint, w, h, 0, 0, w, h);
            right = ((float)w - c.ox) / c.s - 20.0f;
            bottom = ((float)h - c.oy) / c.s - 20.0f;
            tw = text_width(PCM_FONT_SMALL, text, 16.0f);
            rect(&d, right - tw - 20.0f, bottom - 30.0f, tw + 20.0f, 30.0f, 0xCC101010u, 0);
            text_prim(&d, PCM_FONT_SMALL, right - tw - 10.0f, bottom - 23.0f, 16.0f, 1.0f, 0xFFE5E5E5u, text);
        }
        render(&d, hint, w, h, 0, 0, w, h);
        if (subtitle_on) {
            uint16_t rows[PCM_CAPTION_ROWS][PCM_TEXT_CAP];
            float widths[PCM_CAPTION_ROWS], box_w = 0, box_y, centre, limit, caption_bottom;
            float caption_size = movie_on ? 17.0f : 13.0f, line_step, padding;
            unsigned count, i;
            /* Captions belong to the displayed picture; the hint keeps its client anchor. */
            canvas_for(&c, hint, w, h, px, py, pw, ph);
            limit = ((float)(c.cr - c.cl) / c.s) - 68.0f;
            if (limit < 1.0f) limit = 1.0f;
            count = caption_rows(caption, rows, movie_on ? limit : 1000000.0f);
            centre = (((float)c.cl + (float)c.cr) * 0.5f - c.ox) / c.s;
            for (i = 0; i < count; ++i) {
                widths[i] = text_width(PCM_FONT_SMALL, rows[i], caption_size);
                box_w = fmaxf(box_w, widths[i]);
            }
            if (box_w > limit) {
                const float fit = limit / box_w;
                caption_size *= fit;
                for (i = 0; i < count; ++i) widths[i] *= fit;
                box_w = limit;
            }
            line_step = movie_on ? 22.0f : caption_size + 4.0f;
            padding = movie_on ? 14.0f : 10.0f;
            box_w += movie_on ? 28.0f : 20.0f;
            caption_bottom = (float)c.cb - 14.0f * c.s;
            if (hint_on) {
                Canvas client;
                canvas_for(&client, hint, w, h, 0, 0, w, h);
                caption_bottom = fminf(caption_bottom, (float)h - 50.0f * client.s - 8.0f * c.s);
            }
            box_y = (caption_bottom - c.oy) / c.s - (line_step * count + padding);
            d.n = 0;
            rect(&d, centre - box_w * 0.5f, box_y, box_w, line_step * count + padding, 0xB8000000u, 0);
            for (i = 0; i < count; ++i)
                text_prim(&d, PCM_FONT_SMALL, centre - widths[i] * 0.5f, box_y + padding * 0.5f + line_step * i,
                          caption_size, 1.0f, 0xFFFFFFFFu, rows[i]);
            /* Draw onto the hint layer without render_region's clearing pass. */
            for (i = 0; i < (unsigned)d.n; ++i) {
                const Prim *p = &d.prim[i];
                if (p->type == PR_RECT) draw_rect(&c, p, 0);
                else draw_text(&c, p, 0);
            }
        }
    }
    *changed = base_changed || overlay_changed || base_on != base_was_on;
    if (base_on) {
        if (*changed) {
            const size_t n = (size_t)w * h;
            size_t i;
            memcpy(composed, base, n * 4);
            for (i = 0; i < n; i++) blend(composed + i, hint[i]);
        }
        *pixels = composed;
    } else *pixels = hint;
    if (*changed) { m_dirty.x0 = m_dirty.y0 = 0; m_dirty.x1 = (int)w; m_dirty.y1 = (int)h; }
    overlay_was_on = 1; hint_was_on = hint_on; base_was_on = base_on;
    return 1;
}

/* ---- panels ------------------------------------------------------------------------------------------------------- */

int pc_menu_panel(const char *title, const char *const *labels, const char *const *values, int n, float scale,
                  const uint32_t **pixels, unsigned *w, unsigned *h)
{
    static Display d;
    static uint32_t *buf;
    static size_t cap;
    const float pad = 9.0f, title_size = 17.0f, size = 11.0f, step = 14.0f, gap = 14.0f;
    const int title_font = m_font[PCM_FONT_BIG].pixels ? PCM_FONT_BIG : PCM_FONT_SMALL;
    uint16_t text[PCM_TEXT_CAP];
    float label_w = 0.0f, value_w = 0.0f, pw, ph, y;
    unsigned bw, bh;
    int i;
    if (!m_font[PCM_FONT_SMALL].pixels || scale <= 0.0f || n < 0) return 0;
    for (i = 0; i < n; i++) {
        utf8_to_utf16(labels[i], text, PCM_TEXT_CAP);
        label_w = fmaxf(label_w, text_width(PCM_FONT_SMALL, text, size));
        utf8_to_utf16(values[i], text, PCM_TEXT_CAP);
        value_w = fmaxf(value_w, text_width(PCM_FONT_SMALL, text, size));
    }
    utf8_to_utf16(title, text, PCM_TEXT_CAP);
    pw = fmaxf(pad + 4.0f + label_w + gap + value_w + pad, pad + 4.0f + text_width(title_font, text, title_size) + pad);
    pw = fmaxf(pw, 180.0f);
    ph = pad + title_size + 8.0f + step * (float)n + pad - 3.0f;
    bw = (unsigned)ceilf(pw * scale); bh = (unsigned)ceilf(ph * scale);
    if ((size_t)bw * bh > cap) {
        uint32_t *next = (uint32_t *)realloc(buf, (size_t)bw * bh * 4);
        if (!next) return 0;
        buf = next; cap = (size_t)bw * bh;
    }
    /* the pause menu's look over the game: its dark bar, the rail at the left, its grey title, green labels */
    d.n = 0; d.slide = 0;
    rect(&d, 0, 0, pw, ph, 0xD0090B0Au, 0);
    rect(&d, 0, 0, 2.0f, ph, 0xFF5A6057u, 0);
    text_prim(&d, title_font, pad + 4.0f, pad, title_size, 1.0f, 0xFFB4B4B4u, text);
    rect(&d, pad + 4.0f, pad + title_size + 3.0f, pw - 2.0f * pad - 4.0f, 1.0f, 0xFF5A6057u, 0);
    for (i = 0, y = pad + title_size + 9.0f; i < n; i++, y += step) {
        utf8_to_utf16(labels[i], text, PCM_TEXT_CAP);
        text_prim(&d, PCM_FONT_SMALL, pad + 4.0f, y, size, 1.0f, 0xFF6F917Cu, text);
        utf8_to_utf16(values[i], text, PCM_TEXT_CAP);
        text_prim(&d, PCM_FONT_SMALL, pad + 4.0f + label_w + gap, y, size, 1.0f, 0xFFE0E0E0u, text);
    }
    render(&d, buf, bw, bh, 0, 0, (unsigned)(640.0f * scale + 0.5f), (unsigned)(480.0f * scale + 0.5f));
    *pixels = buf; *w = bw; *h = bh;
    return 1;
}

/* ---- tests -------------------------------------------------------------------------------------------------------- */

void pc_menu_test_reset(const char *definition)
{
    Doc *d = (Doc *)malloc(sizeof(Doc));
    char log[2048];
    if (!d) return;
    parse_doc(definition, d, log, sizeof(log));
    if (log[0]) fprintf(stderr, "%s", log);
    AcquireSRWLockExclusive(&m_lock);
    free(m_doc);
    m_doc = d;
    m_loaded = 1;
    m_mtime.dwLowDateTime = m_mtime.dwHighDateTime = 0;
    m_qhead = m_qtail = 0;
    m_display.n = 0;
    m_generation++;
    ReleaseSRWLockExclusive(&m_lock);
    m_page = -1; m_page_name[0] = 0; m_focus = m_prev_focus = -1; m_depth = 0;
    m_hit_n = m_hit_scratch_n = 0; m_pointer_item = -1; m_pointer_known = 0; m_xf_s = 0;
    m_button_swallowed = 0;
    m_disabled = 0;
    InterlockedExchange(&m_input_seen, 0);
    pc_menu_set_movie_skip_prompt(0);
    m_overlay_assumed = 1;
    strcpy(m_path, "<test>");
    vars_clear();
    InterlockedExchange(&m_active, 0);
}

int pc_menu_test_focus(void) { return m_focus; }

int pc_menu_test_load_file(const char *path)
{
    snprintf(m_path, sizeof(m_path), "%s", path);
    m_loaded = 0;
    load_doc(0);
    return m_doc ? m_doc->npage : 0;
}

int pc_menu_test_render(uint32_t *pixels, unsigned w, unsigned h)
{
    Display *d = (Display *)malloc(sizeof(Display));
    if (!d) return 0;
    AcquireSRWLockShared(&m_lock);
    *d = m_display;
    ReleaseSRWLockShared(&m_lock);
    d->slide = 0;
    render(d, pixels, w, h, 0, 0, w, h);
    free(d);
    return 1;
}

void pc_menu_test_pointer_space(unsigned w, unsigned h)
{
    Canvas xf;
    canvas_for(&xf, NULL, w, h, 0, 0, w, h);
    AcquireSRWLockExclusive(&m_lock);
    m_xf_ox = xf.ox; m_xf_oy = xf.oy; m_xf_s = xf.s;
    pointer_hit_locked();
    ReleaseSRWLockExclusive(&m_lock);
}

void pc_menu_test_button(int button, int down) { f_button(button, down); }
int pc_menu_test_pointer_item(void) { return m_pointer_item; }

size_t pc_menu_test_dump(char *out, size_t cap)
{
    size_t len = 0;
    int i;
    if (!cap) return 0;
    out[0] = 0;
    AcquireSRWLockShared(&m_lock);
    for (i = 0; i < m_display.n && len + 1 < cap; i++) {
        const Prim *p = &m_display.prim[i];
        char t[PCM_TEXT_CAP];
        int j, n;
        for (j = 0; p->text[j] && j + 1 < PCM_TEXT_CAP; j++) t[j] = p->text[j] < 128 ? (char)p->text[j] : '?';
        t[j] = 0;
        n = snprintf(out + len, cap - len, "%s %.1f %.1f %.1f %.1f %08X %s\n",
                     p->type == PR_RECT ? "rect" : p->type == PR_TEXT ? "text" : "image", p->x, p->y,
                     p->type == PR_TEXT ? p->size : p->w, p->type == PR_TEXT ? p->scale : p->h, p->colour, t);
        if (n < 0) break;
        len += (size_t)n < cap - len ? (size_t)n : cap - len - 1;
    }
    ReleaseSRWLockShared(&m_lock);
    return len;
}

#else  /* !_WIN32 */
int pc_menu_set_font(int slot, const PcMenuFont *font) { (void)slot; (void)font; return 0; }
int pc_menu_has_font(int slot) { (void)slot; return 0; }
int pc_menu_set_image(int slot, const uint32_t *p, unsigned w, unsigned h) { (void)slot; (void)p; (void)w; (void)h; return 0; }
void pc_menu_set_default_definition(const char *text) { (void)text; }
void pc_menu_set_replaced_defaults(const uint64_t *h, int n) { (void)h; (void)n; }
void pc_menu_set_movie_skip_prompt(int visible) { (void)visible; }
int pc_menu_take_input(void) { return 0; }
int pc_menu_tick(const char *page, int ready, const PcMenuHost *host) { (void)page; (void)ready; (void)host; return 0; }
void pc_menu_install(void) {}
int pc_menu_active(void) { return 0; }
int pc_menu_overlay(unsigned w, unsigned h, int px, int py, unsigned pw, unsigned ph, const uint32_t **pixels, int *changed)
{ (void)px; (void)py; (void)pw; (void)ph; return pc_video_menu_overlay(w, h, pixels, changed); }
int pc_menu_panel(const char *title, const char *const *labels, const char *const *values, int n, float scale,
                  const uint32_t **pixels, unsigned *w, unsigned *h)
{ (void)title; (void)labels; (void)values; (void)n; (void)scale; (void)pixels; (void)w; (void)h; return 0; }
int pc_menu_check_definition(const char *text, char *log, size_t cap) { (void)text; (void)log; (void)cap; return 0; }
const char *pc_menu_definition_path(void) { return ""; }
void pc_menu_test_reset(const char *d) { (void)d; }
void pc_menu_test_action(int a) { (void)a; }
void pc_menu_request_video(void) {}
int pc_menu_test_focus(void) { return -1; }
int pc_menu_test_load_file(const char *path) { (void)path; return 0; }
int pc_menu_test_render(uint32_t *p, unsigned w, unsigned h) { (void)p; (void)w; (void)h; return 0; }
void pc_menu_pointer(int x, int y) { (void)x; (void)y; }
void pc_menu_test_pointer_space(unsigned w, unsigned h) { (void)w; (void)h; }
void pc_menu_test_button(int button, int down) { (void)button; (void)down; }
int pc_menu_test_pointer_item(void) { return -1; }
size_t pc_menu_test_dump(char *out, size_t cap) { (void)out; (void)cap; return 0; }
#endif
