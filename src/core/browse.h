#ifndef NATIVE_BROWSE_H
#define NATIVE_BROWSE_H

#include <stdbool.h>
#include <stddef.h>
#include "artwork.h"

#define BROWSE_PAGE_SIZE 10
#define SEARCH_CAPACITY 256
#define SONG_RADIO_LIMIT 50
#define TRACKS_PAGE_SIZE 50
#define SEARCH_RADIO_LIMIT 20

typedef enum {
    BROWSE_SEARCH,
    BROWSE_SAVED_TRACKS,
    BROWSE_SAVED_ALBUMS,
    BROWSE_PLAYLISTS,
    BROWSE_ALBUM_TRACKS,
    BROWSE_PLAYLIST_TRACKS,
    BROWSE_QUEUE,
    BROWSE_ARTIST_ALBUMS,
    BROWSE_PROFILE_PLAYLISTS,
    BROWSE_FOLLOWED_ARTISTS,
    BROWSE_ARTIST_LIKES,
    BROWSE_SONG_RADIO,
    BROWSE_SETTINGS
} BrowseKind;

typedef enum { ITEM_TRACK, ITEM_ALBUM, ITEM_PLAYLIST, ITEM_EPISODE, ITEM_ARTIST } ItemKind;
typedef enum { SEARCH_ALL, SEARCH_SONGS, SEARCH_ARTISTS, SEARCH_ALBUMS, SEARCH_PLAYLISTS } SearchFilter;

typedef struct {
    ItemKind kind;
    char id[23];
    char title[256];
    char detail[256];
    char guest_artists[128];
    char artwork[ARTWORK_URL_CAPACITY];
    bool playable;
    unsigned duration_ms;
    bool duration_known;
    bool explicit_content;
    unsigned char popularity;
    unsigned track_number;
    unsigned disc_number;
    char artist_id[23];
    char guest_artist_ids[8][23];
    bool liked;
    char album_id[23];
    char album_title[256];
    char release_date[11];
    char album_type[16];
    char owner_id[64];
    bool public_playlist;
} CatalogItem;

typedef struct {
    BrowseKind kind;
    SearchFilter filter;
    char query[SEARCH_CAPACITY];
    char id[23];
    unsigned offset;
    char title[256];
    char artwork[ARTWORK_URL_CAPACITY];
    char owner_id[64];
    char after[23];
} BrowseRequest;

typedef struct {
    CatalogItem items[BROWSE_PAGE_SIZE];
    unsigned count;
    unsigned offset;
    unsigned limit;
    unsigned total;
    bool has_next;
    CatalogItem artists[5];
    CatalogItem albums[5];
    CatalogItem playlists[5];
    unsigned artist_count;
    unsigned album_count;
    unsigned playlist_count;
    char next_after[23];
} BrowsePage;

typedef struct {
    BrowseRequest request;
    BrowsePage page;
    int selected;
    bool loaded;
} Browser;

/* Playlist and Liked Songs pages are fetched 50 at a time into heap memory, separately from BrowsePage. */
typedef struct {
    CatalogItem items[TRACKS_PAGE_SIZE];
    unsigned count;
    unsigned scanned;
    unsigned total;
    bool has_next;
} TrackPage;

bool spotify_id_valid(const char *id);
bool browse_tracks_url(const BrowseRequest *request, char *out, size_t capacity);
bool browse_parse_tracks(const char *json, const BrowseRequest *request, TrackPage *page);
bool browse_url(const BrowseRequest *request, char *out, size_t capacity);
bool browse_parse(const char *json, const BrowseRequest *request, BrowsePage *page);
const CatalogItem *browse_search_top(const char *query, const CatalogItem *tracks, unsigned track_count,
                                     const CatalogItem *artists, unsigned artist_count,
                                     const CatalogItem *albums, unsigned album_count,
                                     const CatalogItem *playlists, unsigned playlist_count);
const char *browse_label(BrowseKind kind);
bool item_link(const CatalogItem *item, char *out, size_t capacity);
bool item_play_body(const CatalogItem *item, char *out, size_t capacity);
bool browse_user_id_valid(const char *id);
bool item_context_body(const CatalogItem *item, const BrowseRequest *context, char *out, size_t capacity);
bool item_can_play(const CatalogItem *item);
bool item_browse_request(const CatalogItem *item, BrowseRequest *request);

void browser_reset(Browser *browser);
void browser_commit(Browser *browser, const BrowseRequest *request, const BrowsePage *page);
bool browser_select(Browser *browser, int index);
const CatalogItem *browser_selected(const Browser *browser);
bool browser_page_request(const Browser *browser, int direction, BrowseRequest *request);
bool browser_child_request(const Browser *browser, BrowseRequest *request);

bool search_append(char query[SEARCH_CAPACITY], const char *utf8, size_t size);
void search_backspace(char query[SEARCH_CAPACITY]);

#endif
