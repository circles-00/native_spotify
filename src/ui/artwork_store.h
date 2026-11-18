#ifndef NATIVE_ARTWORK_STORE_H
#define NATIVE_ARTWORK_STORE_H

#include "artwork.h"

typedef enum { ART_EMPTY, ART_PENDING, ART_LOADING, ART_READY, ART_FAILED } ArtworkState;

typedef struct {
    char url[ARTWORK_URL_CAPACITY];
    Image image;
    ArtworkState state;
    uint64_t used;
    uint64_t frame;
    uint64_t retry_ms;
} ArtworkEntry;

typedef struct {
    ArtworkEntry entries[ARTWORK_CACHE_LIMIT];
    uint64_t clock;
    uint64_t frame;
} ArtworkStore;

void artwork_store_clear(ArtworkStore *store);
void artwork_store_pin(ArtworkStore *store, const char *url);
ArtworkEntry *artwork_store_request(ArtworkStore *store, const char *url, uint64_t now);
unsigned artwork_store_next(const ArtworkStore *store);

#endif
