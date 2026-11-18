#ifndef NATIVE_BROWSE_CACHE_H
#define NATIVE_BROWSE_CACHE_H

#include "browse.h"

#define BROWSE_CACHE_LIMIT 16
#define BROWSE_CACHE_TTL_MS 300000

typedef struct {
    BrowseRequest request;
    BrowsePage page;
    uint64_t fetched_ms;
    uint64_t used;
    bool valid;
} BrowseCacheEntry;

typedef struct {
    BrowseCacheEntry entries[BROWSE_CACHE_LIMIT];
    uint64_t clock;
} BrowseCache;

bool browse_cache_get(BrowseCache *cache, const BrowseRequest *request, uint64_t now_ms, BrowsePage *page);
void browse_cache_store(BrowseCache *cache, const BrowseRequest *request, const BrowsePage *page,
                        uint64_t now_ms);

#endif
