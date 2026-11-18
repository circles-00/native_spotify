#include "lyrics.h"
#include "platform.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lyrics_track(const Playback *track) {
    return !strncmp(track->uri, "spotify:track:", 14) && track->title[0] && track->artist[0] &&
           track->duration_ms > 0;
}

bool lyrics_match(const Lyrics *lyrics, const Playback *track) {
    return lyrics_track(track) && !strcmp(lyrics->uri, track->uri);
}

int lyrics_active_line(const Lyrics *lyrics, unsigned position_ms) {
    unsigned lo = 0, hi = lyrics->state == LYRICS_SYNCED ? lyrics->count : 0;

    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;

        if (lyrics->lines[mid].time_ms <= position_ms) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return (int)lo - 1;
}

static bool digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static const char *timestamp(const char *p, const char *end, unsigned *ms) {
    if (p == end || *p++ != '[') {
        return NULL;
    }

    unsigned minutes = 0, digits = 0;

    while (p < end && digit(*p) && digits < 4) {
        minutes = minutes * 10 + (unsigned)(*p++ - '0');
        ++digits;
    }

    if (!digits || end - p < 4 || p[0] != ':' || !digit(p[1]) || !digit(p[2]) || p[1] > '5') {
        return NULL;
    }

    unsigned time = minutes * 60000 + (unsigned)((p[1] - '0') * 10 + p[2] - '0') * 1000;
    p += 3;

    if (*p == '.') {
        if (end - p < 2 || !digit(p[1])) {
            return NULL;
        }

        unsigned scale = 100;

        while (++p < end && digit(*p) && scale) {
            time += (unsigned)(*p - '0') * scale;
            scale /= 10;
        }
    }

    if (p == end || *p != ']') {
        return NULL;
    }

    *ms = time;
    return p + 1;
}

static int by_time(const void *a, const void *b) {
    const LyricLine *left = a, *right = b;

    if (left->time_ms != right->time_ms) {
        return left->time_ms < right->time_ms ? -1 : 1;
    }

    return (left->text > right->text) - (left->text < right->text);
}

static bool parse_lines(Lyrics *lyrics, const char *source, bool synced, unsigned duration_ms) {
    unsigned used = 0;
    bool has_text = false;
    long offset = 0;
    lyrics->count = 0;

    for (const char *p = source; p && *p && lyrics->count < LYRICS_LINE_LIMIT;) {
        const char *line = p;
        const char *end = strchr(p, '\n');
        p = end ? end + 1 : NULL;
        end = end ? end : line + strlen(line);

        if (end - line >= 3 && !memcmp(line, "\xef\xbb\xbf", 3)) {
            line += 3;
        }

        unsigned first = lyrics->count;

        if (synced) {
            if (end - line > 8 && !memcmp(line, "[offset:", 8)) {
                char *tail;
                long value = strtol(line + 8, &tail, 10);

                if (tail > line + 8 && tail < end && *tail == ']' && value >= -600000 && value <= 600000) {
                    offset = value;
                }
            }

            unsigned ms;
            const char *tag;

            while (lyrics->count < LYRICS_LINE_LIMIT && (tag = timestamp(line, end, &ms))) {
                lyrics->lines[lyrics->count++] = (LyricLine){ms, used};
                line = tag;
            }

            if (lyrics->count == first) {
                continue;
            }
        } else {
            lyrics->lines[lyrics->count++] = (LyricLine){0, used};
        }

        while (line < end && (*line == ' ' || *line == '\t')) {
            ++line;
        }

        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }

        size_t length = (size_t)(end - line);

        if (length >= sizeof(lyrics->text) - used) {
            lyrics->count = first;
            break;
        }

        memcpy(lyrics->text + used, line, length);
        lyrics->text[used + length] = 0;
        used += (unsigned)length + 1;
    }

    unsigned kept = 0;

    for (unsigned i = 0; i < lyrics->count; ++i) {
        LyricLine line = lyrics->lines[i];
        int64_t ms = (int64_t)line.time_ms - offset;

        if (synced && ms > duration_ms) {
            continue;
        }

        line.time_ms = ms < 0 ? 0 : (unsigned)ms;
        has_text |= lyrics->text[line.text] != 0;
        lyrics->lines[kept++] = line;
    }

    lyrics->count = kept;

    if (synced) {
        qsort(lyrics->lines, lyrics->count, sizeof(*lyrics->lines), by_time);
    }

    return has_text;
}

static LyricsState parse_record(yyjson_val *record, const Playback *track, Lyrics *lyrics) {
    const char *timed = yyjson_get_str(yyjson_obj_get(record, "syncedLyrics"));
    const char *plain = yyjson_get_str(yyjson_obj_get(record, "plainLyrics"));

    if (parse_lines(lyrics, timed, true, track->duration_ms)) {
        return LYRICS_SYNCED;
    }

    if (parse_lines(lyrics, plain, false, track->duration_ms)) {
        return LYRICS_PLAIN;
    }

    lyrics->count = 0;
    return yyjson_is_true(yyjson_obj_get(record, "instrumental")) ? LYRICS_INSTRUMENTAL : LYRICS_MISSING;
}

static bool same_name(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }

    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;

        if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y)) {
            return false;
        }
    }

    return *a == *b;
}

bool lyrics_parse(const char *json, size_t size, const Playback *track, bool search, Lyrics *lyrics) {
    yyjson_doc *doc = yyjson_read(json, size, 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = search ? yyjson_is_arr(root) : yyjson_is_obj(root);

    if (ok && !search) {
        lyrics->state = parse_record(root, track, lyrics);
    }

    if (ok && search) {
        size_t i, count;
        yyjson_val *record;
        double best_score = 0;
        Lyrics candidate;

        yyjson_arr_foreach(root, i, count, record) {
            if (!same_name(track->title, yyjson_get_str(yyjson_obj_get(record, "trackName"))) ||
                !same_name(track->artist, yyjson_get_str(yyjson_obj_get(record, "artistName")))) {
                continue;
            }

            double duration = yyjson_get_num(yyjson_obj_get(record, "duration"));
            double delta = duration - track->duration_ms / 1000.0;

            if (duration <= 0 || delta < -2 || delta > 2) {
                continue;
            }

            candidate = (Lyrics){0};
            candidate.state = parse_record(record, track, &candidate);

            if (candidate.state != LYRICS_SYNCED &&
                (candidate.state != LYRICS_PLAIN || (lyrics->state == LYRICS_PLAIN && best_score == 0))) {
                continue;
            }

            bool album = same_name(track->album, yyjson_get_str(yyjson_obj_get(record, "albumName")));
            double score = (candidate.state == LYRICS_SYNCED ? 16 : 8) + (album ? 4 : 0) + 3 -
                           (delta < 0 ? -delta : delta);

            if (score > best_score) {
                best_score = score;
                *lyrics = candidate;
            }
        }
    }

    copy_utf8(lyrics->uri, sizeof(lyrics->uri), track->uri);
    yyjson_doc_free(doc);
    return ok;
}

static int request(const char *url, HttpResponse *response, uint64_t *next_request_ms) {
    uint64_t now = platform_milliseconds();

    if (*next_request_ms > now) {
        platform_sleep((unsigned)(*next_request_ms - now));
    }

    bool ok = platform_http("GET", url, NULL, NULL, false, response);
    now = platform_milliseconds();
    *next_request_ms = now + 250;

    if (ok && response->status == 429) {
        unsigned char jitter = 0;
        platform_random(&jitter, sizeof(jitter));
        *next_request_ms = now + (response->retry_after ? response->retry_after : 60) * 1000ULL + jitter * 4;
    }

    return ok ? response->status : 0;
}

void lyrics_fetch(const Playback *track, Lyrics *lyrics, uint64_t *next_request_ms) {
    char title[sizeof(track->title) * 3], artist[sizeof(track->artist) * 3], album[sizeof(track->album) * 3];
    char url[sizeof(title) + sizeof(artist) + sizeof(album) + 128];
    *lyrics = (Lyrics){.state = LYRICS_ERROR};
    copy_utf8(lyrics->uri, sizeof(lyrics->uri), track->uri);

    if (!url_encode(track->title, title, sizeof(title)) ||
        !url_encode(track->artist, artist, sizeof(artist)) ||
        !url_encode(track->album, album, sizeof(album))) {
        return;
    }

    int size =
        snprintf(url, sizeof(url), "https://lrclib.net/api/get?track_name=%s&artist_name=%s&album_name=%s",
                 title, artist, album);
    unsigned duration = (unsigned)(((uint64_t)track->duration_ms + 500) / 1000);

    if (duration && duration <= 3600) {
        snprintf(url + size, sizeof(url) - (size_t)size, "&duration=%u", duration);
    }

    HttpResponse response = {0};
    int status = request(url, &response, next_request_ms);

    if (status == 200) {
        lyrics_parse(response.body, response.size, track, false, lyrics);
    }

    if (status == 404) {
        lyrics->state = LYRICS_MISSING;
    }

    free(response.body);

    if (lyrics->state != LYRICS_MISSING && lyrics->state != LYRICS_PLAIN) {
        return;
    }

    snprintf(url, sizeof(url), "https://lrclib.net/api/search?track_name=%s&artist_name=%s", title, artist);
    response = (HttpResponse){0};
    status = request(url, &response, next_request_ms);

    if ((status != 200 || !lyrics_parse(response.body, response.size, track, true, lyrics)) &&
        lyrics->state == LYRICS_MISSING) {
        lyrics->state = LYRICS_ERROR;
    }

    free(response.body);
}
