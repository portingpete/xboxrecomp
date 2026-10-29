#ifndef PC_SPEECH_SUBTITLE_H
#define PC_SPEECH_SUBTITLE_H
#include <stdint.h>

#define PC_SPEECH_SUBTITLE_CHARS 512
#define PC_SUBTITLE_COLUMNS 42
#define PC_SUBTITLE_ROWS 2
/* Complete words, at most 42 characters per line. Longer text is paged over
 * its audio/cue duration. A final page remains until playback ends. */
int pc_subtitle_page(const uint16_t *text, uint64_t elapsed, unsigned duration,
                     unsigned rows, uint16_t *out, unsigned capacity);
/* Only actual playback enters here. Loading/prefetching a bank is not a cue. */
void pc_speech_subtitle_start(uint32_t owner, unsigned level, unsigned sector,
                              unsigned sectors, unsigned rate);
void pc_speech_subtitle_stop(uint32_t owner);
int pc_subtitles_enabled(void);
int pc_speech_subtitle(uint16_t *out, unsigned capacity, int paused);
#ifdef PC_SPEECH_SUBTITLE_TEST
void pc_speech_subtitle_test_time(uint64_t now);
#endif
#endif
