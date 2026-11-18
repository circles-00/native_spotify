#ifndef NATIVE_SCENE_H
#define NATIVE_SCENE_H

#include "app.h"
#include "canvas.h"
#include "scroll.h"

#define ROW_SLOTS (HIT_ROW_LAST - HIT_ROW_FIRST + 1)

typedef enum {
    HIT_NONE,
    HIT_ALL,
    HIT_MUSIC,
    HIT_HOME,
    HIT_SEARCH,
    HIT_ACCOUNT,
    HIT_TRACKS,
    HIT_ALBUMS,
    HIT_PLAYLISTS,
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
    HIT_NOW_PLAYING,
    HIT_FRIENDS,
    HIT_LYRICS,
    HIT_PANEL_CLOSE,
    HIT_PANEL_QUEUE,
    HIT_PANEL_ARTIST,
    HIT_PANEL_PLAY,
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
    HIT_CLOSE_DEVICES,
    HIT_SETUP_KEY,
    HIT_SETUP_PASTE,
    HIT_SETUP_FORGET,
    HIT_CLOSE_SETUP,
    HIT_SEARCH_ALL,
    HIT_SEARCH_SONGS,
    HIT_SEARCH_ARTISTS,
    HIT_SEARCH_ALBUMS,
    HIT_SEARCH_TOP,
    HIT_SEARCH_ARTIST_FIRST,
    HIT_SEARCH_ARTIST_LAST = HIT_SEARCH_ARTIST_FIRST + 4,
    HIT_SEARCH_ALBUM_FIRST,
    HIT_SEARCH_ALBUM_LAST = HIT_SEARCH_ALBUM_FIRST + 4,
    HIT_HOME_QUICK_FIRST,
    HIT_HOME_QUICK_LAST = HIT_HOME_QUICK_FIRST + 7,
    HIT_HOME_HISTORY,
    HIT_HOME_ALBUMS,
    HIT_HOME_PLAYLISTS,
    HIT_CARD_FIRST,
    HIT_CARD_LAST = HIT_CARD_FIRST + 9,
    HIT_LIBRARY_FIRST,
    HIT_LIBRARY_LAST = HIT_LIBRARY_FIRST + 23,
    HIT_ROW_FIRST,
    HIT_ROW_LAST = HIT_ROW_FIRST + 29,
    HIT_DEVICE_FIRST,
    HIT_DEVICE_LAST = HIT_DEVICE_FIRST + DEVICE_LIMIT - 1,
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
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_ESCAPE,
    KEY_SPACE
} SceneKey;

typedef enum { PANEL_NONE, PANEL_NOW_PLAYING, PANEL_QUEUE, PANEL_FRIENDS, PANEL_LYRICS } SidePanel;

typedef enum {
    CTX_NONE,
    CTX_PLAY,
    CTX_RADIO,
    CTX_ALBUM,
    CTX_ARTIST,
    CTX_QUEUE,
    CTX_LINK,
    CTX_PROFILE,
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
    float width, height;
    Rect sidebar, content, search, player, device_panel;
    Rect side_panel;
    SidePanel panel;
    unsigned panel_first;
    bool over_panel;
    Hit hits[HIT_COUNT];
    float hover[HIT_COUNT];
    HitId hot, focus, dragging, pressed;
    bool home, device_popup, setup_popup, pending_browse, select_all, animating, login_screen;
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
    uint64_t search_due;
    char query[SEARCH_CAPACITY];
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
    unsigned device_revision;
    unsigned device_visible;
    float entrance;
    bool retain_content;
    uint64_t last_ms;
    uint64_t click_ms;
    HitId last_click;
    bool double_click;
    PlayerDraft draft;
    ContextMenu menu;
} Scene;

void scene_init(Scene *scene);
unsigned scene_columns(float width);
void scene_layout(Scene *scene, const View *view, float width, float height);
void scene_draw(Scene *scene, const View *view, Canvas *canvas);
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
bool scene_input(Scene *scene, const char *text, size_t size);
void scene_query_changed(Scene *scene);
void scene_search(Scene *scene);
void scene_scroll(Scene *scene, const View *view, int direction);
const CatalogItem *scene_home_item(const View *view, unsigned index);

#endif
