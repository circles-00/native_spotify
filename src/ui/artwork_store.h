#ifndef NATIVE_ARTWORK_STORE_H
#define NATIVE_ARTWORK_STORE_H

#include "artwork.h"

#define ARTWORK_CACHE_LIMIT 112
#define ARTWORK_BYTES_LIMIT (6 * 1024 * 1024)
#define ARTWORK_HARD_BYTES_LIMIT (10 * 1024 * 1024)
#define ARTWORK_PRIORITY_BYTES_LIMIT (1024 * 1024) /* Room for now playing and overlay covers. */
#define ARTWORK_IDLE_BYTES_LIMIT (3 * 1024 * 1024)

typedef enum { ART_EMPTY, ART_PENDING, ART_LOADING, ART_READY, ART_FAILED } ArtworkState;

typedef struct {
    char url[ARTWORK_URL_CAPACITY];
    Image image;
    ArtworkState state;
    uint64_t used;
    uint64_t frame;
    uint64_t priority_frame;
    uint64_t retry_ms;
    bool budget_failed;
} ArtworkEntry;

typedef struct {
    ArtworkEntry entries[ARTWORK_CACHE_LIMIT];
    uint64_t clock;
    uint64_t frame;
} ArtworkStore;

void artwork_store_clear(ArtworkStore *store);
void artwork_store_pin(ArtworkStore *store, const char *url);
ArtworkEntry *artwork_store_request(ArtworkStore *store, const char *url, uint64_t now);
void artwork_store_request_priority(ArtworkStore *store, const char *url, uint64_t now);
unsigned artwork_store_next(const ArtworkStore *store);
size_t artwork_store_bytes(const ArtworkStore *store);
bool artwork_store_evict_budget(ArtworkStore *store, size_t reserve_bytes, bool priority);
bool artwork_store_prune_stale(ArtworkStore *store, uint64_t keep_frames, size_t target_bytes);

#endif
