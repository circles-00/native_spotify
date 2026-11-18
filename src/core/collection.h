#ifndef NATIVE_COLLECTION_H
#define NATIVE_COLLECTION_H

#include "browse.h"

typedef struct {
    CatalogItem item;
    CatalogItem artist;
    CatalogItem popular[10];
    unsigned popular_count;
    bool popular_loaded;
    unsigned total_tracks;
    uint64_t followers;
    bool followers_known;
    char release_precision[8];
    char label[256];
    char copyrights[1024];
    uint64_t duration_ms;
    bool duration_complete;
    bool loaded;
    bool saved;
    bool saved_known;
    bool saving;
    bool permission_required;
} CollectionInfo;

typedef struct {
    char id[128];
    char name[256];
    char artwork[ARTWORK_URL_CAPACITY];
    uint64_t followers;
    bool followers_known;
    CatalogItem following[10];
    unsigned following_count;
    unsigned following_total;
    bool following_known;
    bool permission_required;
    bool loaded;
} ProfileInfo;

bool collection_parse(const char *json, ItemKind kind, CollectionInfo *info);
bool collection_popular_parse(const char *json, CollectionInfo *info);
void collection_release_label(const CollectionInfo *info, char *out, size_t capacity);
void collection_duration_label(uint64_t duration_ms, char *out, size_t capacity);
bool collection_page(BrowseKind kind);
bool profile_parse(const char *json, ProfileInfo *profile);

#endif
