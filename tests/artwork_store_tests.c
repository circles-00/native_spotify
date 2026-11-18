#include "ui/artwork_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression)                                                                                    \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(expression)) {                                                                                 \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression);                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

int main(void) {
    ArtworkStore store = {.frame = 1};

    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        char url[128];
        snprintf(url, sizeof(url), "https://i.scdn.co/image/cover%u", i);
        ArtworkEntry *entry = artwork_store_request(&store, url, 0);
        CHECK(entry && !strcmp(entry->url, url));
        entry->state = ART_READY;
        entry->image = (Image){1, 1, malloc(sizeof(uint32_t))};
        CHECK(entry->image.pixels);
        entry->image.pixels[0] = i;
    }

    // Every cover visible in this frame survives, including covers beyond the old 24-slot limit.
    CHECK(!artwork_store_request(&store, "https://i.scdn.co/image/overflow", 0));

    for (unsigned pass = 0; pass < 3; ++pass) {
        ++store.frame;

        for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
            char url[128];
            snprintf(url, sizeof(url), "https://i.scdn.co/image/cover%u", i);
            artwork_store_pin(&store, url);
        }

        CHECK(!artwork_store_request(&store, "https://i.scdn.co/image/overflow", 0));

        for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
            char url[128];
            snprintf(url, sizeof(url), "https://i.scdn.co/image/cover%u", i);
            ArtworkEntry *entry = artwork_store_request(&store, url, 0);
            CHECK(entry && entry->state == ART_READY && entry->image.pixels[0] == i);
        }
    }

    ++store.frame;
    artwork_store_pin(&store, "https://i.scdn.co/image/cover0");
    ArtworkEntry *entry = artwork_store_request(&store, "https://i.scdn.co/image/new", 0);
    CHECK(entry && entry->state == ART_PENDING);
    CHECK(artwork_store_request(&store, "https://i.scdn.co/image/cover0", 0)->state == ART_READY);

    entry->state = ART_FAILED;
    entry->retry_ms = 30000;
    CHECK(artwork_store_request(&store, entry->url, 29999)->state == ART_FAILED);
    CHECK(artwork_store_request(&store, entry->url, 30000)->state == ART_PENDING);

    unsigned loading = artwork_store_next(&store);
    CHECK(loading < ARTWORK_CACHE_LIMIT);
    store.entries[loading].state = ART_LOADING;

    // Fast scrolling replaces stale queued work, not a download that is currently writing its slot.
    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT * 3; ++i) {
        ++store.frame;
        char url[128];
        snprintf(url, sizeof(url), "https://i.scdn.co/image/scroll%u", i);
        CHECK(artwork_store_request(&store, url, 0));
        CHECK(store.entries[loading].state == ART_LOADING);
        CHECK(!strcmp(store.entries[artwork_store_next(&store)].url, url));
    }

    CHECK(!artwork_store_request(&store, "https://invalid.test/image/cover", 0));
    artwork_store_clear(&store);
    CHECK(artwork_store_next(&store) == ARTWORK_CACHE_LIMIT);
    printf("PASS: %u artwork store checks (visible pins, scroll churn, priority and retries)\n", checks);
    return 0;
}
