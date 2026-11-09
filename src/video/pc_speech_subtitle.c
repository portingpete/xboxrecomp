#include "pc_speech_subtitle.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct SpeechCue {
    unsigned level, sector, sectors, rate, duration;
    uint16_t text[512];
} SpeechCue;
typedef struct PlayingCue {
    const SpeechCue *cue;
    uint32_t owner;
    uint64_t started;
} PlayingCue;
static INIT_ONCE m_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK m_lock = SRWLOCK_INIT;
static SpeechCue *m_cues;
static unsigned m_count;
/* Two simultaneously audible speakers; a third replaces the oldest caption. */
static PlayingCue m_playing[2];
static uint64_t m_polled;
static int m_paused;

int pc_subtitles_enabled(void)
{
    const char *enabled = getenv("RECOMP_SUBTITLES");
    return !enabled || strcmp(enabled, "0") != 0;
}
#ifdef PC_SPEECH_SUBTITLE_TEST
static uint64_t m_test_now;
void pc_speech_subtitle_test_time(uint64_t now) { m_test_now = now; }
static uint64_t now_ms(void) { return m_test_now; }
#else
static uint64_t now_ms(void) { return GetTickCount64(); }
#endif

static int number(char **at, char separator, unsigned max, unsigned *out)
{
    char *end;
    unsigned long value;
    const int quoted = separator != '\t' && **at == '"';
    if (quoted) ++*at;
    if (**at < '0' || **at > '9') return 0;
    errno = 0;
    value = strtoul(*at, &end, 10);
    if (quoted) { if (*end != '"') return 0; ++end; }
    if (errno || *end != separator || value > max) return 0;
    *out = (unsigned)value;
    *at = end + 1;
    return 1;
}

/* Numeric fields precede the single text field. CSV quoting only applies to
 * that last field; doubled quotes preserve dialogue quotes and punctuation. */
static int caption_text(char *text, char separator)
{
    char *src = text, *dst = text;
    text[strcspn(text, "\r\n")] = 0;
    if (separator == '\t') return *text && !strchr(text, '\t');
    if (*src != '"') return *text && !strchr(text, separator);
    ++src;
    while (*src) {
        if (*src == '"') {
            if (src[1] == '"') { *dst++ = '"'; src += 2; continue; }
            ++src;
            while (*src == ' ' || *src == '\t') ++src;
            if (*src) return 0;
            *dst = 0;
            return dst != text;
        }
        *dst++ = *src++;
    }
    return 0;
}

static FILE *open_track(const char *track, char path[96])
{
    FILE *file;
    snprintf(path, 96, "subtitles/%s/gameplay.csv", track);
    file = fopen(path, "rb");
    if (file) return file;
    snprintf(path, 96, "subtitles/%s/gameplay.tsv", track);
    return fopen(path, "rb");
}

static int binds_next_word(const uint16_t *word, unsigned length)
{
    /* Function words belong with their complement, across line AND page
     * boundaries. Bare words only: sentence punctuation permits a break. */
    static const WCHAR *const words[] = {
        L"a", L"o", L"as", L"os", L"um", L"uma", L"uns", L"umas",
        L"de", L"do", L"da", L"dos", L"das", L"em", L"no", L"na", L"nos", L"nas",
        L"ao", L"aos", L"\xE0", L"\xE0s", L"com", L"por", L"para", L"pelo", L"pela",
        L"sem", L"sob", L"sobre", L"entre", L"que", L"se", L"e", L"ou", L"mas",
        L"an", L"the", L"of", L"to", L"for", L"in", L"on", L"at", L"with", L"from", L"and", L"or",
        L"il", L"lo", L"la", L"i", L"gli", L"le", L"un", L"uno", L"una", L"di", L"del", L"della", L"al", L"alla", L"per", L"con",
        L"les", L"une", L"des", L"du", L"au", L"aux", L"dans", L"en", L"et",
        L"el", L"los", L"las", L"unos", L"unas", L"y",
        L"der", L"die", L"das", L"ein", L"eine", L"einen", L"einem", L"eines", L"dem", L"den", L"zu", L"von", L"mit", L"f\xFCr", L"auf", L"und", L"oder"
    };
    unsigned i;
    if (length > 5) return 0;
    for (i = 0; i < sizeof(words) / sizeof(*words); ++i)
        if (wcslen(words[i]) == length && CompareStringOrdinal((WCHAR *)word, length, words[i], length, TRUE) == CSTR_EQUAL)
            return 1;
    return 0;
}

int pc_subtitle_page(const uint16_t *text, uint64_t elapsed, unsigned duration,
                     unsigned rows, uint16_t *out, unsigned capacity)
{
    uint16_t lines[32][PC_SUBTITLE_COLUMNS + 1] = {{0}};
    unsigned lengths[32] = {0}, count = 0, at = 0, total = 0, i, first, used = 0;
    uint64_t position;
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!text || !rows || rows > PC_SUBTITLE_ROWS) return 0;
    while (text[at]) {
        unsigned start, word, group, look, last, last_length;
        while (text[at] && text[at] <= ' ') ++at;
        if (!text[at]) break;
        start = at;
        while (text[at] > ' ') ++at;
        word = at - start;
        if (word > PC_SUBTITLE_COLUMNS) return 0; /* Catalog validation rejects this. */
        group = word; look = at; last = start; last_length = word;
        while (binds_next_word(text + last, last_length)) {
            while (text[look] && text[look] <= ' ') ++look;
            if (!text[look]) break;
            last = look;
            while (text[look] > ' ') ++look;
            last_length = look - last;
            group += 1 + last_length;
            if (group > PC_SUBTITLE_COLUMNS) { group = word; break; }
        }
        if (lengths[count] && lengths[count] + 1 + group > PC_SUBTITLE_COLUMNS) {
            if (++count == 32) return 0;
        }
        if (lengths[count]) lines[count][lengths[count]++] = ' ';
        memcpy(lines[count] + lengths[count], text + start, word * sizeof(*text));
        lengths[count] += word;
    }
    if (!lengths[0]) return 0;
    ++count;
    for (i = 0; i < count; ++i) total += lengths[i] + 1;
    if (!duration) duration = 1;
    if (elapsed >= duration) elapsed = duration - 1;
    position = elapsed * total / duration;
    for (i = 0; i + 1 < count && position >= lengths[i] + 1; ++i) position -= lengths[i] + 1;
    /* Each line has its own time slice. With two speakers use one line each;
     * changing the row budget still includes the line whose slice is current. */
    first = i / rows * rows;
    for (i = first; i < count && i < first + rows; ++i) used += lengths[i] + (i != first);
    if (used + 1 > capacity) return 0; /* Never return a silently truncated word. */
    used = 0;
    for (i = first; i < count && i < first + rows; ++i) {
        if (i != first) out[used++] = '\n';
        memcpy(out + used, lines[i], lengths[i] * sizeof(*out));
        used += lengths[i];
    }
    out[used] = 0;
    return used != 0;
}

static BOOL CALLBACK load_cues(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    FILE *file;
    char line[2048];
    const char *language = getenv("RECOMP_LANGUAGE");
    const char *enabled = getenv("RECOMP_GAMEPLAY_SUBTITLES");
    static const char *const tracks[] = { "pt-BR", "es", "fr", "de", "it" };
    const char *track = "en";
    char path[96];
    unsigned i, rejected = 0;
    (void)once; (void)parameter; (void)context;
    if (!pc_subtitles_enabled() || (enabled && !strcmp(enabled, "0"))) return TRUE;
    for (i = 0; language && i < sizeof(tracks) / sizeof(*tracks); ++i)
        if (!_stricmp(language, tracks[i])) { track = tracks[i]; break; }
    file = open_track(track, path);
    if (!file && strcmp(track, "en")) file = open_track("en", path);
    if (!file) return TRUE;
    while (fgets(line, sizeof(line), file)) {
        SpeechCue cue = {0}, *next;
        char *text = line;
        char *separator;
        size_t length = strlen(line);
        if (length >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) text += 3;
        /* Blank lines, comments and the CSV column header are not entries. */
        if ((*text < '0' || *text > '9') && !(*text == '"' && text[1] >= '0' && text[1] <= '9')) continue;
        if (line[length - 1] != '\n' && !feof(file)) {
            int ch;
            while ((ch = fgetc(file)) != EOF && ch != '\n') {}
            ++rejected;
            continue;
        }
        separator = strpbrk(text, "\t,;");
        if (!separator || !number(&text, *separator, 99, &cue.level) ||
            !number(&text, *separator, 65535, &cue.sector) ||
            !number(&text, *separator, 65535, &cue.sectors) || !number(&text, *separator, 192000, &cue.rate) ||
            !number(&text, *separator, 120000, &cue.duration) || !cue.sectors || cue.rate < 8000 || !cue.duration ||
            !caption_text(text, *separator) ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, (WCHAR *)cue.text, 512)) {
            ++rejected;
            continue;
        }
        {
            uint16_t page[86];
            if (!pc_subtitle_page(cue.text, 0, cue.duration, 2, page, 86)) { ++rejected; continue; }
        }
        next = (SpeechCue *)realloc(m_cues, (m_count + 1) * sizeof(*m_cues));
        if (!next) break;
        m_cues = next;
        m_cues[m_count++] = cue;
    }
    fclose(file);
    fprintf(stderr, "[SPEECH SUB] loaded %u gameplay captions from %s (%u rejected rows)\n", m_count, path, rejected);
    return TRUE;
}

void pc_speech_subtitle_start(uint32_t owner, unsigned level, unsigned sector,
                              unsigned sectors, unsigned rate)
{
    unsigned i;
    const SpeechCue *cue = NULL;
    uint64_t now = now_ms();
    if (!owner) return;
    InitOnceExecuteOnce(&m_once, load_cues, NULL, NULL);
    for (i = 0; i < m_count; ++i) {
        const SpeechCue *c = &m_cues[i];
        if (c->level == level && c->sector == sector && c->sectors == sectors && c->rate == rate) {
            cue = c;
            break;
        }
    }
    AcquireSRWLockExclusive(&m_lock);
    for (i = 0; i < 2; ++i)
        if (m_playing[i].owner == owner) memset(&m_playing[i], 0, sizeof(m_playing[i]));
    if (cue) {
        if (m_playing[1].cue) m_playing[0] = m_playing[1];
        m_playing[1].cue = cue;
        m_playing[1].owner = owner;
        m_playing[1].started = now;
        fprintf(stderr, "[SPEECH SUB] play level=%02u sector=%u duration=%ums owner=%08X\n", level, sector, cue->duration, owner);
    }
    ReleaseSRWLockExclusive(&m_lock);
}

void pc_speech_subtitle_stop(uint32_t owner)
{
    unsigned i;
    AcquireSRWLockExclusive(&m_lock);
    for (i = 0; i < 2; ++i)
        if (m_playing[i].owner == owner) memset(&m_playing[i], 0, sizeof(m_playing[i]));
    ReleaseSRWLockExclusive(&m_lock);
}

int pc_speech_subtitle(uint16_t *out, unsigned capacity, int paused)
{
    uint64_t now = now_ms();
    unsigned i, used = 0, speakers = 0;
    if (!out || !capacity) return 0;
    memset(out, 0, capacity * sizeof(*out));
    AcquireSRWLockExclusive(&m_lock);
    for (i = 0; i < 2; ++i) if (m_playing[i].cue) ++speakers;
    for (i = 0; i < 2; ++i) {
        PlayingCue *p = &m_playing[i];
        if (!p->cue) continue;
        if (m_paused && now >= m_polled) p->started += now - m_polled;
        if (!paused) {
            uint16_t page[PC_SUBTITLE_ROWS * (PC_SUBTITLE_COLUMNS + 1)];
            unsigned n = 0;
            const uint64_t elapsed = now >= p->started ? now - p->started : 0;
            if (!pc_subtitle_page(p->cue->text, elapsed, p->cue->duration,
                                  speakers == 2 ? 1 : 2, page, sizeof(page) / sizeof(*page))) continue;
            while (page[n]) ++n;
            if (used + (used != 0) + n + 1 > capacity) { used = 0; break; }
            if (used) out[used++] = '\n';
            memcpy(out + used, page, n * sizeof(*out));
            used += n;
        }
    }
    m_polled = now;
    m_paused = paused;
    out[used] = 0;
    ReleaseSRWLockExclusive(&m_lock);
    return used != 0;
}
