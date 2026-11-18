#include "artwork_store.h"

#include <stdlib.h>
#include <string.h>

void artwork_store_clear(ArtworkStore *store) {
    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        free(store->entries[i].image.pixels);
    }

    memset(store, 0, sizeof(*store));
}

void artwork_store_pin(ArtworkStore *store, const char *url) {
    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        if (store->entries[i].state != ART_EMPTY && !strcmp(store->entries[i].url, url)) {
            store->entries[i].frame = store->frame;
            return;
        }
    }
}

ArtworkEntry *artwork_store_request(ArtworkStore *store, const char *url, uint64_t now) {
    if (!artwork_url_valid(url)) {
        return NULL;
    }

    ArtworkEntry *victim = NULL;

    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        ArtworkEntry *entry = &store->entries[i];

        if (entry->state != ART_EMPTY && !strcmp(entry->url, url)) {
            entry->used = ++store->clock;
            entry->frame = store->frame;

            if (entry->state == ART_FAILED && now >= entry->retry_ms) {
                entry->state = ART_PENDING;
            }

            return entry;
        }

        if (entry->state == ART_EMPTY) {
            victim = entry;
            continue;
        }

        if (entry->state == ART_LOADING || entry->frame == store->frame) {
            continue;
        }

        if (!victim || (victim->state != ART_EMPTY && entry->used < victim->used)) {
            victim = entry;
        }
    }

    if (!victim) {
        return NULL;
    }

    free(victim->image.pixels);
    *victim = (ArtworkEntry){.state = ART_PENDING, .used = ++store->clock, .frame = store->frame};
    memcpy(victim->url, url, strlen(url) + 1);
    return victim;
}

unsigned artwork_store_next(const ArtworkStore *store) {
    unsigned slot = ARTWORK_CACHE_LIMIT;

    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        const ArtworkEntry *entry = &store->entries[i];

        if (entry->state != ART_PENDING) {
            continue;
        }

        if (slot == ARTWORK_CACHE_LIMIT || entry->used > store->entries[slot].used) {
            slot = i;
        }
    }

    return slot;
}
