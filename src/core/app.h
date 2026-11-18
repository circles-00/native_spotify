#ifndef NATIVE_APP_H
#define NATIVE_APP_H

#include "core.h"
#include "browse.h"
#include "engine.h"
#include "player.h"
#include "lyrics.h"
#include "collection.h"

typedef enum {
    ACTION_RESTORE,
    ACTION_CONNECT,
    ACTION_REFRESH,
    ACTION_PREVIOUS,
    ACTION_PLAY,
    ACTION_PAUSE,
    ACTION_NEXT,
    ACTION_DISCONNECT,
    ACTION_BROWSE,
    ACTION_MORE,
    ACTION_PLAY_ITEM,
    ACTION_OPEN_LINK,
    ACTION_DEVICES,
    ACTION_QUEUE,
    ACTION_TRANSFER,
    ACTION_SEEK,
    ACTION_VOLUME,
    ACTION_SHUFFLE,
    ACTION_REPEAT,
    ACTION_SYNC,
    ACTION_LIBRARY,
    ACTION_HOME,
    ACTION_QUEUE_PANEL,
    ACTION_DETAILS,
    ACTION_SAVE_COLLECTION,
    ACTION_ENQUEUE,
    ACTION_RADIO,
    ACTION_LIKES,
    ACTION_TOGGLE_LIKE,
    ACTION_LIKE_STATUS,
    ACTION_TOGGLE_PLAYBACK,
    ACTION_LYRICS
} Action;

/* Rows around the scroll position; the full collection stays in the app so snapshots stay small. */
#define LIST_WINDOW 64
#define LIST_LIMIT 1500
#define SEARCH_HISTORY_LIMIT 20

typedef enum { LIST_SORT_NONE, LIST_SORT_TITLE, LIST_SORT_ALBUM, LIST_SORT_DURATION } ListSort;

typedef struct {
    BrowseRequest request;
    CatalogItem rows[LIST_WINDOW];
    ListSort sort;
    bool sort_descending;
    char filter[SEARCH_CAPACITY];
    unsigned first;
    unsigned rows_count;
    unsigned count;
    unsigned total;
    unsigned next_offset;
    char next_after[23];
    uint64_t duration_ms;
    bool duration_known;
    unsigned max_disc;
    int selected;
    bool loaded;
    bool more;
    bool loading;
    unsigned revision;
} ListView;

const CatalogItem *list_row(const ListView *list, unsigned index);

typedef struct {
    bool search_radio;
} AppSettings;

typedef struct {
    Playback playback;
    Lyrics lyrics;
    CollectionInfo details;
    ProfileInfo profile;
    ListView list;
    ListView playlists;
    CatalogItem search_artists[5];
    CatalogItem search_albums[5];
    CatalogItem search_playlists[5];
    unsigned search_artist_count;
    unsigned search_album_count;
    unsigned search_playlist_count;
    Devices devices;
    Queue queue;
    char status[256];
    bool connecting;
    bool loading_intro;
    unsigned session_revision;
    unsigned playback_revision;
    unsigned devices_revision;
    bool authenticated;
    uint64_t playback_received_ms;
    BrowsePage library[3];
    bool library_loaded[3];
    CatalogItem recent[8];
    unsigned recent_count;
    bool recent_loaded;
    bool recent_permission_required;
    CatalogItem search_history[SEARCH_HISTORY_LIMIT];
    unsigned search_history_count;
    unsigned artist_liked_count;
    bool artist_likes_ready;
    bool artist_likes_failed;
    bool playback_liked;
    EngineState engine;
    char engine_message[256];
    char engine_device_id[DEVICE_ID_CAPACITY];
    bool remote_selected;
    unsigned setup_revision;
    AppSettings settings;
} View;

typedef void (*AppWake)(void);

void app_initialize(void);
void app_set_wake(AppWake wake);
void app_shutdown(void);
void app_action(Action action);
bool app_connect(void);
bool app_open_item(const CatalogItem *item);
bool app_play_collection(void);
bool app_play_popular(unsigned index);
bool app_toggle_collection_saved(void);
bool app_toggle_song_like(const CatalogItem *item);
bool app_browse_artist_likes(const CatalogItem *artist);
void app_search_history_add(const CatalogItem *item);
void app_search_history_remove(const char *id);
bool app_play_search_history(const CatalogItem *item);
bool app_filter_discography(const char *group);
bool app_open_collection_link(void);
bool app_open_item_link(const CatalogItem *item);
bool app_enqueue_item(const CatalogItem *item);
bool app_start_radio(const CatalogItem *item);
bool app_browse_song_radio(const CatalogItem *item);
bool app_play_song_radio(void);
bool app_dequeue(const char *id);
bool app_clear_queue(void);
bool app_play_queued(const char *id);
void app_queue_viewport(unsigned first);
bool app_play_upcoming(const CatalogItem *item);
bool app_play_list(void);
bool app_list_arrangeable(BrowseKind kind);
bool app_list_sort(ListSort sort, bool descending);
bool app_list_filter(const char *text);
int app_tick(bool visible);
bool app_browse(BrowseKind kind, const char *query);
bool app_search(const char *query, SearchFilter filter);
bool app_load_more(void);
void app_viewport(unsigned first);
void app_library_viewport(unsigned first);
void app_queue_visible(bool visible);
void app_devices_visible(bool visible);
bool app_transfer_to(int index);
void app_lyrics_visible(bool visible);
bool app_select(int index);
bool app_activate(void);
bool app_open_link(void);
bool app_select_device(int index);
bool app_transfer(void);
bool app_seek(unsigned fraction);
bool app_seek_lyric(const char *uri, unsigned position_ms);
bool app_volume(unsigned percent);
bool app_engine_key(const char *text, size_t size);
void app_engine_dashboard(void);
void app_engine_forget(void);
bool app_set_search_radio(bool enabled);
void app_snapshot_into(View *out);
View app_snapshot(void);

#endif
