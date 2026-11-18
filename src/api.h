#ifndef NATIVE_API_H
#define NATIVE_API_H
#include "core.h"
#include "browse.h"
#include "player.h"
#include "collection.h"

typedef struct {
    char client_id[33];
    Tokens tokens;
    uint64_t retry_at;
    char message[256];
    void (*notify)(const char *message);
    int last_status;
} Spotify;

bool spotify_login(Spotify *spotify);
bool spotify_restore(Spotify *spotify);
bool spotify_ensure_token(Spotify *spotify);
bool spotify_disconnect(Spotify *spotify);
bool spotify_playback(Spotify *spotify, Playback *playback);
bool spotify_command(Spotify *spotify, const Playback *target, const char *command);
bool spotify_browse(Spotify *spotify, const BrowseRequest *request, BrowsePage *page);
bool spotify_play_item(Spotify *spotify, const Playback *target, const CatalogItem *item,
                       const BrowseRequest *context);
bool spotify_devices(Spotify *spotify, Devices *devices);
bool spotify_queue(Spotify *spotify, Queue *queue);
bool spotify_transfer(Spotify *spotify, const Device *device, bool playing);
bool spotify_seek(Spotify *spotify, const Playback *playback, unsigned position);
bool spotify_volume(Spotify *spotify, const Playback *playback, unsigned percent);
bool spotify_options(Spotify *spotify, const Playback *target, bool shuffle, unsigned value);
bool spotify_recent_albums(Spotify *spotify, BrowsePage *page);
bool spotify_collection(Spotify *spotify, ItemKind kind, const char *id, CollectionInfo *info);
bool spotify_popular(Spotify *spotify, const char *id, CollectionInfo *info);
bool spotify_collection_saved(Spotify *spotify, const CatalogItem *item, bool *saved);
bool spotify_save_collection(Spotify *spotify, const CatalogItem *item, bool saved);
bool spotify_profile(Spotify *spotify, ProfileInfo *profile);
bool spotify_enqueue(Spotify *spotify, const Playback *target, const CatalogItem *item);
bool spotify_play_radio(Spotify *spotify, const Playback *target, const CatalogItem *seed, bool play_first);
#endif
