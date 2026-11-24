#ifndef NATIVE_APP_INTERNAL_H
#define NATIVE_APP_INTERNAL_H

#include "app.h"
#include "api.h"
#include "browse_cache.h"
#include "platform.h"
#include "yyjson.h"

/*
 * Content, playback control and status reads use independent lanes.
 * Each lane has its own request state; OAuth tokens are shared under platform_token_lock().
 */
typedef enum { LANE_PLAYER, LANE_CONTENT, LANE_STATE, LANE_LYRICS, LANE_SYNC, LANE_RADIO, LANE_COUNT } Lane;

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
    uint64_t like_ticket;
    char track_ids[SONG_RADIO_LIMIT][23];
    unsigned track_count;
    bool local_seek;
} Job;

#define LANE_CAPACITY 16

typedef struct {
    char id[23];
    uint64_t ticket;
    bool liked;
} PendingLike;

/* Like state for one song, read directly from Spotify while the full library scan is incomplete. */
typedef struct {
    char id[23];
    bool liked;
    bool known;
} LikeStatus;

#define LIKE_STATUS_CAPACITY 256
#define LIKES_LIMIT 500
#define PLAYLISTS_LIMIT 250
#define PLAY_ORDER_LIMIT 128
#define UP_NEXT_LIMIT 50
#define TRACK_CACHE_SLOTS 1
#define TRACK_CACHE_ITEM_LIMIT 150
#define DETAIL_CACHE_LIMIT 2

/* A fully loaded playlist, reused while Spotify reports the same snapshot. */
typedef struct {
    char id[23];
    char snapshot[128];
    CatalogItem *items;
    unsigned count;
    uint64_t used;
} TrackCacheEntry;
#define UP_NEXT_STALE_LIMIT 4

/* A song Spotify already holds in its queue that the listener no longer wants. */
typedef struct {
    char id[23];
    uint64_t until_ms;
} StaleHandoff;

typedef struct {
    Job *jobs;
    unsigned capacity;
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
    char id[23];
    uint64_t last_played;
} PlaylistUsage;

typedef struct {
    View view;
    Worker workers[LANE_COUNT];
    Job worker_jobs[LANE_SYNC][LANE_CAPACITY];
    Job radio_job;
    Job sync_job;
    Tokens tokens;
    BrowseCache browse_cache;
    PlayerSync sync;
    PlayerSeek seek;
    Job delayed_seek;
    uint64_t seek_due_ms;
    uint64_t seek_sent_ms;
    uint64_t next_paint_ms;
    uint64_t next_devices_ms;
    unsigned command_seq;
    unsigned browse_seq;
    bool shutting_down;
    bool startup_home_done;
    bool startup_library_done;
    bool restored_pending;
    bool restored_controls_synced;
    uint64_t playback_saved_ms;
    Job deferred;
    bool has_deferred;
    /* Every loaded row in Spotify's order; `order` maps displayed rows when sorted or filtered. */
    CatalogItem *collection;
    unsigned collection_capacity;
    unsigned collection_count;
    unsigned *order;
    unsigned order_capacity;
    CatalogItem radio_seed;
    CatalogItem *likes;
    unsigned likes_count, likes_capacity, likes_offset;
    bool likes_ready;
    bool likes_scanning;
    PendingLike pending_likes[LANE_CAPACITY + 1];
    uint64_t like_ticket;
    /* What follows the song the listener started, in the order Spotify will play it. */
    CatalogItem *play_order;
    unsigned play_order_count;
    unsigned play_order_capacity;
    unsigned play_order_pos;
    unsigned play_order_followed;
    char play_order_context[128];
    BrowseRequest play_order_source;
    bool play_order_follow;
    unsigned queue_first;
    TrackCacheEntry track_cache[TRACK_CACHE_SLOTS];
    uint64_t track_cache_clock;
    char loading_snapshot[128];
    CatalogItem up_next[UP_NEXT_LIMIT];
    unsigned up_next_count;
    unsigned up_next_revision;
    unsigned up_next_generation;
    char handoff_id[23];
    bool handoff_inflight;
    StaleHandoff stale_handoffs[UP_NEXT_STALE_LIMIT];
    LikeStatus like_status[LIKE_STATUS_CAPACITY];
    unsigned like_status_next;
    bool like_status_inflight;
    uint64_t like_status_retry_ms;
    unsigned viewport_first;
    CatalogItem *playlists;
    unsigned playlist_capacity;
    PlaylistUsage *playlist_usage;
    unsigned playlist_usage_count;
    unsigned playlist_usage_capacity;
    uint64_t playlist_usage_clock;
    char last_played_playlist_id[23];
    CatalogItem search_radio_seed;
    unsigned library_first;
    bool queue_visible;
    bool devices_visible;
    bool lyrics_visible;
    uint64_t lyrics_next_request_ms;
    char radio_guarded_uri[128];
    DetailCacheEntry detail_cache[DETAIL_CACHE_LIMIT];
    AppWake wake;
} AppState;

extern AppState app_state;

void app_wake(void);
void app_optimistic(const Job *job);
void app_reset_session(void);
bool app_selected_item(CatalogItem *item);

bool app_collection_store(const BrowseRequest *request, const BrowsePage *page, bool append);
CatalogItem *app_list_item(unsigned index);
void app_arrange_list_locked(void);
bool app_collection_store_radio(const BrowseRequest *request, const SongRadio *radio);
void app_store_library(const BrowseRequest *request, const BrowsePage *page);
bool app_begin_browse(Job job);
bool app_song_liked(const char *id);
void app_remember_like_locked(const char *id, bool liked);
void app_request_like_status(const View *view);
void app_finish_like_status(const Job *job, const bool *liked);
void app_finish_song_like(const Job *job, const char *error);
void app_scan_likes(void);
void app_record_playlist_used_locked(const char *id);
void app_sort_playlists_locked(CatalogItem *items, unsigned count);
void app_save_playback_locked(bool force);
void app_sync_startup_intro_locked(void);

void app_replace_up_next_locked(void);
void app_explicit_play_locked(const char *id);
void app_append_up_next_locked(const CatalogItem *items, unsigned count);
void app_compose_queue_locked(const Queue *spotify, Queue *out, unsigned first);
unsigned app_queue_total_locked(const Queue *spotify);
const CatalogItem *app_queue_at_locked(const Queue *spotify, unsigned index);
const CatalogItem *app_queue_next_locked(void);
void app_capture_play_order_locked(const Job *job);
void app_follow_play_order_locked(bool append);
bool app_up_next_skip(Spotify *spotify, const Playback *target, unsigned session);
bool app_skip_command(Spotify *spotify, const Playback *target, const char *command, unsigned session);
bool app_enqueue_command(Spotify *spotify, const Playback *target, const CatalogItem *item, unsigned session);
void app_up_next_sync(Spotify *spotify, const Playback *playback, const char *previous_uri, unsigned session);
void app_up_next_save_locked(yyjson_mut_doc *doc, yyjson_mut_val *root);
void app_up_next_restore_locked(yyjson_val *root);

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
