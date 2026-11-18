#include "browse_cache.h"

#include <string.h>

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
    if (request->kind == BROWSE_QUEUE || page->count > BROWSE_PAGE_SIZE || page->offset != request->offset) {
        return;
    }

    unsigned slot = 0;

    for (unsigned i = 0; i < BROWSE_CACHE_LIMIT; ++i) {
        if (cache->entries[i].valid && same_request(&cache->entries[i].request, request)) {
            slot = i;
            break;
        }

        if (!cache->entries[i].valid || cache->entries[i].used < cache->entries[slot].used) {
            slot = i;
        }
    }

    cache->entries[slot] = (BrowseCacheEntry){*request, *page, now_ms, ++cache->clock, true};
}
