#ifndef NATIVE_APP_INTERNAL_H
#define NATIVE_APP_INTERNAL_H

#include "app.h"
#include "api.h"
#include "browse_cache.h"
#include "platform.h"

/*
 * Content, playback control and status reads use independent lanes.
 * Each lane has its own request state; OAuth tokens are shared under platform_token_lock().
 */
typedef enum { LANE_PLAYER, LANE_CONTENT, LANE_STATE, LANE_COUNT } Lane;

typedef struct {
    Action action;
    BrowseRequest browse;
    CatalogItem item;
    Device device;
    Playback target;
    unsigned value;
    unsigned session;
    unsigned seq;
    unsigned browse_seq;
} Job;

#define LANE_CAPACITY 16

typedef struct {
    Job jobs[LANE_CAPACITY];
    unsigned head;
    unsigned count;
    bool running;
    bool active;
    Action action;
    Spotify spotify;
} Worker;

typedef struct {
    CollectionInfo info;
    uint64_t fetched_ms;
} DetailCacheEntry;

typedef struct {
    View view;
    Worker workers[LANE_COUNT];
    Tokens tokens;
    BrowseCache browse_cache;
    PlayerSync sync;
    PlayerSeek seek;
    uint64_t next_paint_ms;
    uint64_t next_devices_ms;
    unsigned command_seq;
    unsigned browse_seq;
    bool shutting_down;
    Job deferred;
    bool has_deferred;
    CatalogItem *collection;
    unsigned collection_capacity;
    unsigned viewport_first;
    CatalogItem *playlists;
    unsigned playlist_capacity;
    unsigned library_first;
    bool queue_visible;
    char radio_guarded_uri[128];
    DetailCacheEntry detail_cache[8];
    AppWake wake;
} AppState;

extern AppState app_state;

void app_wake(void);
void app_optimistic(const Job *job);
void app_reset_session(void);
bool app_selected_item(CatalogItem *item);

bool app_collection_store(const BrowseRequest *request, const BrowsePage *page, bool append);
void app_store_library(const BrowseRequest *request, const BrowsePage *page);
bool app_fetch_page(Spotify *spotify, const BrowseRequest *request, BrowsePage *page, bool fresh);
bool app_begin_browse(Job job);

void app_load_library(Spotify *spotify, unsigned session);
void app_run_content(Spotify *spotify, const Job *job);
void app_run_player(Spotify *spotify, const Job *job);

Lane app_lane_for(Action action);
bool app_player_command(Action action);
bool app_submit(Job job);
bool app_command_pending(void);
void app_replace_tokens(const Tokens *replacement);
bool app_signed_in(void);
void app_set_status(unsigned session, const char *message);
void app_workers_configure(void);
bool app_workers_save_key(const char *key, size_t size);
void app_workers_forget_key(void);

#endif
