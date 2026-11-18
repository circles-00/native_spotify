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

size_t artwork_store_bytes(const ArtworkStore *store) {
    size_t total = 0;

    for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
        const ArtworkEntry *entry = &store->entries[i];

        if (entry->image.pixels) {
            total += (size_t)entry->image.width * entry->image.height * sizeof(uint32_t);
        }
    }

    return total;
}

bool artwork_store_evict_budget(ArtworkStore *store, size_t reserve_bytes) {
    if (reserve_bytes > ARTWORK_HARD_BYTES_LIMIT) {
        return false;
    }

    size_t total = artwork_store_bytes(store);

    while (total + reserve_bytes > ARTWORK_BYTES_LIMIT) {
        ArtworkEntry *victim = NULL;

        for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
            ArtworkEntry *entry = &store->entries[i];

            if (!entry->image.pixels || entry->state == ART_LOADING || entry->frame == store->frame) {
                continue;
            }

            if (!victim || entry->used < victim->used) {
                victim = entry;
            }
        }

        if (!victim) {
            break;
        }

        size_t freed = (size_t)victim->image.width * victim->image.height * sizeof(uint32_t);
        free(victim->image.pixels);
        *victim = (ArtworkEntry){0};
        total -= freed;
    }

    return total + reserve_bytes <= ARTWORK_HARD_BYTES_LIMIT;
}

bool artwork_store_prune_stale(ArtworkStore *store, uint64_t keep_frames, size_t target_bytes) {
    size_t total = artwork_store_bytes(store);
    bool pruned = false;

    while (total > target_bytes) {
        ArtworkEntry *victim = NULL;

        for (unsigned i = 0; i < ARTWORK_CACHE_LIMIT; ++i) {
            ArtworkEntry *entry = &store->entries[i];

            if (!entry->image.pixels || entry->state == ART_LOADING || entry->frame > store->frame ||
                store->frame - entry->frame <= keep_frames) {
                continue;
            }

            if (!victim || entry->used < victim->used) {
                victim = entry;
            }
        }

        if (!victim) {
            break;
        }

        size_t freed = (size_t)victim->image.width * victim->image.height * sizeof(uint32_t);
        free(victim->image.pixels);
        *victim = (ArtworkEntry){0};
        total -= freed;
        pruned = true;
    }

    return pruned;
}
