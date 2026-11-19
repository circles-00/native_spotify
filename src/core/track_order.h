#ifndef NATIVE_TRACK_ORDER_H
#define NATIVE_TRACK_ORDER_H

#include <stdbool.h>
#include <stddef.h>

static inline unsigned track_fold(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static inline int track_compare_text(const char *left, const char *right) {
    while (*left && track_fold((unsigned char)*left) == track_fold((unsigned char)*right)) {
        ++left;
        ++right;
    }

    unsigned a = track_fold((unsigned char)*left);
    unsigned b = track_fold((unsigned char)*right);
    return (a > b) - (a < b);
}

static inline bool track_contains_word(const char *text, const char *word, size_t length) {
    for (; *text; ++text) {
        size_t i = 0;

        while (i < length && track_fold((unsigned char)text[i]) == track_fold((unsigned char)word[i])) {
            ++i;
        }

        if (i == length) {
            return true;
        }
    }

    return false;
}

static inline bool track_filter_matches(const char *title, const char *artist, const char *album, const char *filter) {
    while (*filter) {
        if (*filter == ' ' || *filter == '\t' || *filter == '\r' || *filter == '\n') {
            ++filter;
            continue;
        }

        const char *word = filter;
        while (*filter && *filter != ' ' && *filter != '\t' && *filter != '\r' && *filter != '\n') {
            ++filter;
        }

        size_t length = (size_t)(filter - word);
        if (!track_contains_word(title, word, length) && !track_contains_word(artist, word, length) &&
            !track_contains_word(album, word, length)) {
            return false;
        }
    }

    return true;
}

#endif
