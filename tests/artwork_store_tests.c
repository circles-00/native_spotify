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
    CHECK(artwork_store_bytes(&store) == 0);

    /* Byte-budget eviction removes stale unpinned artwork while keeping active frame entries. */
    const unsigned tile_side = ARTWORK_SIDE_LIMIT;
    const size_t tile_bytes = (size_t)tile_side * tile_side * sizeof(uint32_t);
    store.frame = 1;

    for (unsigned i = 0; i < 26; ++i) {
        if (i == 10) {
            store.frame = 2;
        }

        char url[128];
        snprintf(url, sizeof(url), "https://i.scdn.co/image/budget%u", i);
        ArtworkEntry *item = artwork_store_request(&store, url, 0);
        CHECK(item != NULL);
        CHECK(artwork_store_evict_budget(&store, tile_bytes));
        item->state = ART_READY;
        item->image = (Image){tile_side, tile_side, malloc(tile_bytes)};
        CHECK(item->image.pixels != NULL);
        CHECK(artwork_store_bytes(&store) <= ARTWORK_BYTES_LIMIT);
    }

    CHECK(artwork_store_evict_budget(&store, tile_bytes));
    CHECK(artwork_store_bytes(&store) + tile_bytes <= ARTWORK_BYTES_LIMIT);
    artwork_store_clear(&store);
    CHECK(artwork_store_bytes(&store) == 0);

    /* Stale off-screen artwork is pruned down to the idle budget while recent pins remain ready. */
    store.frame = 1;

    for (unsigned i = 0; i < 18; ++i) {
        if (i == 14) {
            store.frame = 5;
        }

        char url[128];
        snprintf(url, sizeof(url), "https://i.scdn.co/image/prune%u", i);
        ArtworkEntry *item = artwork_store_request(&store, url, 0);
        CHECK(item != NULL);
        item->state = ART_READY;
        item->image = (Image){tile_side, tile_side, malloc(tile_bytes)};
        CHECK(item->image.pixels != NULL);
    }

    CHECK(artwork_store_bytes(&store) > ARTWORK_IDLE_BYTES_LIMIT);
    CHECK(artwork_store_prune_stale(&store, 1, ARTWORK_IDLE_BYTES_LIMIT));
    CHECK(artwork_store_bytes(&store) <= ARTWORK_IDLE_BYTES_LIMIT);

    for (unsigned i = 14; i < 18; ++i) {
        char url[128];
        snprintf(url, sizeof(url), "https://i.scdn.co/image/prune%u", i);
        ArtworkEntry *item = artwork_store_request(&store, url, 0);
        CHECK(item && item->state == ART_READY && item->image.pixels != NULL);
    }

    artwork_store_clear(&store);
    CHECK(artwork_store_bytes(&store) == 0);

    /* When active-frame pinned covers already fill the hard budget, new pixel storage is rejected. */
    store.frame = 3;

    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        char url[128];
        snprintf(url, sizeof(url), "https://i.scdn.co/image/hard%u", i);
        ArtworkEntry *item = artwork_store_request(&store, url, 0);

        if (!item) {
            break;
        }

        if (!artwork_store_evict_budget(&store, tile_bytes)) {
            break;
        }

        item->state = ART_READY;
        item->image = (Image){tile_side, tile_side, malloc(tile_bytes)};
        CHECK(item->image.pixels != NULL);
    }

    CHECK(artwork_store_bytes(&store) <= ARTWORK_HARD_BYTES_LIMIT);
    CHECK(!artwork_store_evict_budget(&store, ARTWORK_HARD_BYTES_LIMIT));
    artwork_store_clear(&store);
    CHECK(artwork_store_bytes(&store) == 0);
    printf("PASS: %u artwork store checks (visible pins, scroll churn, priority, byte budgets and retries)\n",
           checks);
    return 0;
}
