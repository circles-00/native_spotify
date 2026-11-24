#ifndef NATIVE_SCENE_H
#define NATIVE_SCENE_H

#include "app.h"
#include "canvas.h"
#include "scroll.h"

#define ROW_SLOTS (HIT_ROW_LAST - HIT_ROW_FIRST + 1)
#define HOME_SHELF_COLUMNS 7
#define LIBRARY_ROW_HEIGHT 53.33f
#define QUEUE_ROW_HEIGHT 42.67f
#define QUEUE_LIST_TOP 198.0f
#define QUEUE_LIST_BOTTOM 8.0f
#define QUEUE_VISIBLE_SLOTS 24
#define SCENE_HISTORY_LIMIT 24
#define HISTORY_ROW_HEIGHT 50.0f
#define DESKTOP_SEARCH_HISTORY_LIMIT 5

typedef enum {
    HIT_NONE,
    HIT_HOME,
    HIT_TOP_HOME,
    HIT_BACK,
    HIT_FORWARD,
    HIT_SEARCH_NAV,
    HIT_SEARCH,
    HIT_SEARCH_CLEAR,
    HIT_ACCOUNT,
    HIT_TRACKS,
    HIT_ALBUMS,
    HIT_PLAYLISTS,
    HIT_ARTISTS,
    HIT_QUEUE,
    HIT_DEVICES,
    HIT_REFRESH,
    HIT_PREVIOUS,
    HIT_PLAY,
    HIT_NEXT,
    HIT_SEEK,
    HIT_VOLUME,
    HIT_SHUFFLE,
    HIT_REPEAT,
    HIT_MUTE,
    HIT_FRIENDS,
    HIT_LYRICS,
    HIT_PANEL_CLOSE,
    HIT_PANEL_QUEUE,
    HIT_PANEL_RECENT_TAB,
    HIT_PANEL_QUEUE_TAB,
    HIT_PANEL_RECENT_FIRST,
    HIT_PANEL_RECENT_LAST = HIT_PANEL_RECENT_FIRST + 7,
    HIT_PANEL_PREVIOUS,
    HIT_PANEL_PLAY,
    HIT_PANEL_NEXT,
    HIT_PLAYING_ON,
    HIT_SCROLL_MAIN,
    HIT_SCROLL_LIBRARY,
    HIT_SCROLL_PANEL,
    HIT_DETAIL_PLAY,
    HIT_DETAIL_SAVE,
    HIT_DETAIL_SHUFFLE,
    HIT_DISC_ALL,
    HIT_DISC_ALBUMS,
    HIT_DISC_SINGLES,
    HIT_DETAIL_ARTIST,
    HIT_DETAIL_LINK,
    HIT_DETAIL_ALL,
    HIT_PROFILE,
    HIT_PROFILE_FOLLOWING,
    HIT_POPULAR_FIRST,
    HIT_POPULAR_LAST = HIT_POPULAR_FIRST + 9,
    HIT_PROFILE_ARTIST_FIRST,
    HIT_PROFILE_ARTIST_LAST = HIT_PROFILE_ARTIST_FIRST + 4,
    HIT_CTX_FIRST,
    HIT_CTX_LAST = HIT_CTX_FIRST + 5,
    HIT_OPEN,
    HIT_LINK,
    HIT_TRANSFER,
    HIT_SETUP_KEY,
    HIT_SETUP_PASTE,
    HIT_SETUP_FORGET,
    HIT_CLOSE_SETUP,
    HIT_SEARCH_ALL,
    HIT_SEARCH_SONGS,
    HIT_SEARCH_ARTISTS,
    HIT_SEARCH_ALBUMS,
    HIT_SEARCH_PLAYLISTS,
    HIT_SEARCH_TOP,
    HIT_SEARCH_ARTIST_FIRST,
    HIT_SEARCH_ARTIST_LAST = HIT_SEARCH_ARTIST_FIRST + 4,
    HIT_SEARCH_ALBUM_FIRST,
    HIT_SEARCH_ALBUM_LAST = HIT_SEARCH_ALBUM_FIRST + 4,
    HIT_SEARCH_PLAYLIST_FIRST,
    HIT_SEARCH_PLAYLIST_LAST = HIT_SEARCH_PLAYLIST_FIRST + 4,
    HIT_HOME_QUICK_FIRST,
    HIT_HOME_QUICK_LAST = HIT_HOME_QUICK_FIRST + 7,
    HIT_HOME_HISTORY,
    HIT_HOME_ALBUMS,
    HIT_HOME_PLAYLISTS,
    HIT_CARD_FIRST,
    HIT_CARD_LAST = HIT_CARD_FIRST + 3 * HOME_SHELF_COLUMNS - 1,
    HIT_LIBRARY_FIRST,
    HIT_LIBRARY_LAST = HIT_LIBRARY_FIRST + 23,
    HIT_ROW_FIRST,
    HIT_ROW_LAST = HIT_ROW_FIRST + 29,
    HIT_DEVICE_FIRST,
    HIT_DEVICE_LAST = HIT_DEVICE_FIRST + DEVICE_LIMIT - 1,
    HIT_PLAYER_LIKE,
    HIT_PLAYER_SONG,
    HIT_PLAYER_ARTIST,
    HIT_ARTIST_LIKES,
    HIT_TOP_LIKE,
    HIT_ROW_LIKE_FIRST,
    HIT_ROW_LIKE_LAST = HIT_ROW_LIKE_FIRST + ROW_SLOTS - 1,
    HIT_POPULAR_LIKE_FIRST,
    HIT_POPULAR_LIKE_LAST = HIT_POPULAR_LIKE_FIRST + 9,
    HIT_QUEUE_ROW_FIRST,
    HIT_QUEUE_ROW_LAST = HIT_QUEUE_ROW_FIRST + QUEUE_VISIBLE_SLOTS - 1,
    HIT_QUEUE_LIKE_FIRST,
    HIT_QUEUE_LIKE_LAST = HIT_QUEUE_LIKE_FIRST + QUEUE_VISIBLE_SLOTS - 1,
    HIT_LYRICS_FULLSCREEN,
    HIT_LYRICS_FOLLOW,
    HIT_LYRIC_FIRST,
    HIT_LYRIC_LAST = HIT_LYRIC_FIRST + LYRICS_LINE_LIMIT - 1,
    HIT_SETTING_SEARCH_RADIO,
    HIT_LIST_FILTER,
    HIT_LIST_FILTER_CLEAR,
    HIT_SORT_INDEX,
    HIT_SORT_TITLE,
    HIT_SORT_ALBUM,
    HIT_SORT_DURATION,
    HIT_QUEUE_CLEAR,
    HIT_HISTORY_FIRST,
    HIT_HISTORY_LAST = HIT_HISTORY_FIRST + SEARCH_HISTORY_LIMIT - 1,
    HIT_HISTORY_LIKE_FIRST,
    HIT_HISTORY_LIKE_LAST = HIT_HISTORY_LIKE_FIRST + SEARCH_HISTORY_LIMIT - 1,
    HIT_HISTORY_REMOVE_FIRST,
    HIT_HISTORY_REMOVE_LAST = HIT_HISTORY_REMOVE_FIRST + SEARCH_HISTORY_LIMIT - 1,
    HIT_COUNT
} HitId;

typedef struct {
    Rect rect;
    bool enabled;
} Hit;

typedef enum {
    KEY_TAB,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_ESCAPE,
    KEY_SPACE
} SceneKey;

typedef struct {
    unsigned cursor;
    unsigned anchor;
    float scroll_x;
} InputState;

typedef enum { PANEL_NONE, PANEL_DEVICES, PANEL_QUEUE, PANEL_FRIENDS, PANEL_LYRICS } SidePanel;

typedef enum {
    CTX_NONE,
    CTX_PLAY,
    CTX_RADIO,
    CTX_ALBUM,
    CTX_ARTIST,
    CTX_QUEUE,
    CTX_DEQUEUE,
    CTX_LINK,
    CTX_PROFILE,
    CTX_SETTINGS,
    CTX_LOGOUT
} ContextAction;

typedef struct {
    bool open;
    Rect rect;
    CatalogItem item;
    ContextAction actions[6];
    char labels[6][64];
    unsigned count;
} ContextMenu;

typedef struct {
    bool home;
    BrowseKind kind;
    SearchFilter filter;
    char query[SEARCH_CAPACITY];
    CatalogItem item;
} SceneRoute;

typedef struct {
    float width, height;
    float caption_width;
    Rect header;
    Rect sidebar, library_rows, content, search, player;
    Rect side_panel;
    SidePanel panel;
    unsigned panel_first;
    bool over_panel;
    bool panel_recent;
    bool lyrics_fullscreen;
    bool lyrics_follow;
    int lyric_active;
    LyricsState lyric_state;
    char lyric_uri[128];
    Rect lyric_list;
    float lyric_layout_width;
    float lyric_y[LYRICS_LINE_LIMIT + 1];
    Rect search_history_panel;
    Scroll search_history_scroll;
    bool search_history_open, over_search_history;
    Hit hits[HIT_COUNT];
    float hover[HIT_COUNT];
    HitId hot, focus, dragging, pressed;
    bool home, setup_popup, pending_browse, select_all, animating, intro_screen, login_screen;
    BrowseKind section;
    SearchFilter search_filter;
    float search_scroll;
    float home_scroll;
    Scroll main_scroll;
    Scroll library_scroll;
    Scroll panel_scroll;
    float main_origin;
    float main_row_height;
    unsigned main_columns;
    float scroll_grab;
    unsigned popular_selected;
    bool popular_expanded;
    float home_extent;
    float home_headings[3];
    uint64_t search_due;
    char query[SEARCH_CAPACITY];
    char list_filter[SEARCH_CAPACITY];
    InputState search_input, filter_input;
    float input_click_x;
    bool input_click_pending;
    Rect login_panel;
    Rect setup_panel;
    unsigned setup_revision;
    char collection[256];
    char collection_artwork[ARTWORK_URL_CAPACITY];
    unsigned first_row;
    unsigned library_first;
    bool over_library;
    int last_selected;
    unsigned session_revision;
    unsigned device_first;
    unsigned device_visible;
    unsigned unmute_volume;
    float entrance;
    bool retain_content;
    uint64_t last_ms;
    uint64_t click_ms;
    HitId last_click;
    bool double_click;
    bool pressed_song_art;
    PlayerDraft draft;
    ContextMenu menu;
    SceneRoute history[SCENE_HISTORY_LIMIT];
    unsigned history_index, history_count;
    bool restoring_route;
    float player_text_w;
    float player_title_w;
    float player_artist_w;
    bool player_text_measured;
    char player_measured_title[512];
    char player_measured_artist[512];
} Scene;

Rect scene_song_art(const Scene *scene, HitId id);
bool scene_queued(const View *view, const char *id);

void scene_init(Scene *scene);
float scene_view_scale(float width, float height, float minimum);
unsigned scene_columns(float width);
void scene_layout(Scene *scene, const View *view, float width, float height);
void scene_draw(Scene *scene, const View *view, Canvas *canvas);
bool scene_contains(Rect rect, float x, float y);
HitId scene_hit(const Scene *scene, float x, float y);
void scene_hover(Scene *scene, HitId hit, uint64_t now);
bool scene_tick(Scene *scene, uint64_t now);
void scene_activate(Scene *scene, const View *view, HitId hit);
void scene_press(Scene *scene, const View *view, float x, float y, uint64_t now);
void scene_context_press(Scene *scene, const View *view, float x, float y);
void scene_motion(Scene *scene, float x);
void scene_pointer_motion(Scene *scene, float x, float y);
void scene_scrollbars(Scene *scene, Canvas *canvas);
void scene_release(Scene *scene, const View *view, float x, float y);
bool scene_key(Scene *scene, const View *view, SceneKey key, bool shift);
bool scene_key_mods(Scene *scene, const View *view, SceneKey key, bool shift, bool ctrl);
bool scene_input(Scene *scene, const char *text, size_t size);
void scene_select_input_all(Scene *scene);
bool scene_copy(Scene *scene, char *out, size_t capacity, bool cut);
void scene_query_changed(Scene *scene);
void scene_search(Scene *scene);
void scene_scroll(Scene *scene, const View *view, int direction);
void scene_wheel(Scene *scene, const View *view, float notches, unsigned lines, bool page);
const CatalogItem *scene_like_item(const Scene *scene, const View *view, HitId id);
const CatalogItem *scene_home_item(const View *view, unsigned index);
const CatalogItem *scene_home_quick_item(const View *view, unsigned index);
bool scene_home_made_for_you(const View *view);

#endif
