#ifndef NATIVE_LYRICS_H
#define NATIVE_LYRICS_H

#include "core.h"

#define LYRICS_LINE_LIMIT 512
#define LYRICS_TEXT_LIMIT 16384

typedef enum {
    LYRICS_IDLE,
    LYRICS_LOADING,
    LYRICS_SYNCED,
    LYRICS_PLAIN,
    LYRICS_INSTRUMENTAL,
    LYRICS_MISSING,
    LYRICS_ERROR
} LyricsState;

typedef struct {
    unsigned time_ms;
    unsigned text;
} LyricLine;

typedef struct {
    LyricsState state;
    unsigned count;
    char uri[128];
    LyricLine lines[LYRICS_LINE_LIMIT];
    char text[LYRICS_TEXT_LIMIT];
} Lyrics;

bool lyrics_track(const Playback *track);
bool lyrics_match(const Lyrics *lyrics, const Playback *track);
int lyrics_active_line(const Lyrics *lyrics, unsigned position_ms);
bool lyrics_parse(const char *json, size_t size, const Playback *track, bool search, Lyrics *lyrics);
void lyrics_fetch(const Playback *track, Lyrics *lyrics, uint64_t *next_request_ms);

#endif
