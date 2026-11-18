#include "browse_cache.h"

#include <string.h>

/* Keep the library roots separate from the rotating set of recently opened collections. */
static int first_slot(const BrowseRequest *request) {
    if (request->offset || request->after[0]) {
        return -1;
    }

    switch (request->kind) {
    case BROWSE_SAVED_TRACKS:
        return 0;
    case BROWSE_SAVED_ALBUMS:
        return 1;
    case BROWSE_PLAYLISTS:
        return 2;
    case BROWSE_FOLLOWED_ARTISTS:
        return 3;
    case BROWSE_PROFILE_PLAYLISTS:
        return 4;
    case BROWSE_ALBUM_TRACKS:
    case BROWSE_PLAYLIST_TRACKS:
    case BROWSE_ARTIST_ALBUMS:
        return BROWSE_CACHE_LIBRARY_SLOTS;
    default:
        return -1;
    }
}

static bool same_request(const BrowseRequest *left, const BrowseRequest *right) {
    return left->kind == right->kind && left->filter == right->filter && left->offset == right->offset &&
           !strcmp(left->id, right->id) && !strcmp(left->query, right->query) &&
           !strcmp(left->owner_id, right->owner_id) && !strcmp(left->after, right->after);
}

bool browse_cache_get(BrowseCache *cache, const BrowseRequest *request, uint64_t now_ms, BrowsePage *page) {
    for (unsigned i = 0; i < BROWSE_CACHE_LIMIT; ++i) {
        BrowseCacheEntry *entry = &cache->entries[i];

        if (!entry->valid || !same_request(&entry->request, request) || now_ms < entry->fetched_ms ||
            now_ms - entry->fetched_ms >= BROWSE_CACHE_TTL_MS) {
            continue;
        }

        entry->used = ++cache->clock;
        *page = entry->page;
        return true;
    }

    return false;
}

void browse_cache_store(BrowseCache *cache, const BrowseRequest *request, const BrowsePage *page,
                        uint64_t now_ms) {
    int first = first_slot(request);

    if (first < 0 || page->count > BROWSE_PAGE_SIZE || page->offset != request->offset) {
        return;
    }

    unsigned slot = (unsigned)first;

    if (slot == BROWSE_CACHE_LIBRARY_SLOTS) {
        for (unsigned i = slot; i < BROWSE_CACHE_LIMIT; ++i) {
            BrowseCacheEntry *entry = &cache->entries[i];

            if (entry->valid && same_request(&entry->request, request)) {
                slot = i;
                break;
            }

            if (!entry->valid || (cache->entries[slot].valid && entry->used < cache->entries[slot].used)) {
                slot = i;
            }
        }
    }

    cache->entries[slot] = (BrowseCacheEntry){*request, *page, now_ms, ++cache->clock, true};
}

void browse_cache_invalidate(BrowseCache *cache, const BrowseRequest *request) {
    for (unsigned i = 0; i < BROWSE_CACHE_LIMIT; ++i) {
        if (cache->entries[i].valid && same_request(&cache->entries[i].request, request)) {
            cache->entries[i].valid = false;
        }
    }
}
