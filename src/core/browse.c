#include "browse.h"
#include "catalog.h"
#include "core.h"
#include "yyjson.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

bool spotify_id_valid(const char *id) {
    if (!id || strlen(id) != 22) {
        return false;
    }

    for (size_t i = 0; i < 22; ++i) {
        unsigned char c = (unsigned char)id[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }

    return true;
}

static unsigned maximum_offset(BrowseKind kind) {
    return kind == BROWSE_SEARCH ? 1000 : 100000;
}

bool browse_url(const BrowseRequest *request, char *out, size_t capacity) {
    if (request->offset > maximum_offset(request->kind)) {
        return false;
    }

    char path[1024];

    switch (request->kind) {
    case BROWSE_SEARCH: {
        char query[SEARCH_CAPACITY * 3];

        if (!request->query[0] || !url_encode(request->query, query, sizeof(query))) {
            return false;
        }

        const char *type = request->filter == SEARCH_ALL         ? "track,artist,album,playlist"
                           : request->filter == SEARCH_SONGS     ? "track"
                           : request->filter == SEARCH_ARTISTS   ? "artist"
                           : request->filter == SEARCH_ALBUMS    ? "album"
                           : request->filter == SEARCH_PLAYLISTS ? "playlist"
                                                                 : NULL;

        if (!type) {
            return false;
        }

        snprintf(path, sizeof(path), "search?type=%s&q=%s&", type, query);
        break;
    }

    case BROWSE_SAVED_TRACKS:
        snprintf(path, sizeof(path), "me/tracks?");
        break;

    case BROWSE_SAVED_ALBUMS:
        snprintf(path, sizeof(path), "me/albums?");
        break;

    case BROWSE_PLAYLISTS:
    case BROWSE_PROFILE_PLAYLISTS:
        snprintf(path, sizeof(path), "me/playlists?");
        break;

    case BROWSE_FOLLOWED_ARTISTS: {
        if (request->after[0] && !spotify_id_valid(request->after)) {
            return false;
        }

        int size = snprintf(out, capacity, "https://api.spotify.com/v1/me/following?type=artist&limit=%d%s%s",
                            BROWSE_PAGE_SIZE, request->after[0] ? "&after=" : "", request->after);
        return size > 0 && (size_t)size < capacity;
    }

    case BROWSE_ALBUM_TRACKS:
    case BROWSE_PLAYLIST_TRACKS:
        if (!spotify_id_valid(request->id)) {
            return false;
        }

        snprintf(path, sizeof(path), "%s/%s/%s?",
                 request->kind == BROWSE_ALBUM_TRACKS ? "albums" : "playlists", request->id,
                 request->kind == BROWSE_ALBUM_TRACKS ? "tracks" : "items");
        break;

    case BROWSE_ARTIST_ALBUMS:
        if (!spotify_id_valid(request->id)) {
            return false;
        }

        {
            const char *group = !request->query[0]                  ? "album,single"
                                : !strcmp(request->query, "album")  ? "album"
                                : !strcmp(request->query, "single") ? "single"
                                                                    : NULL;

            if (!group) {
                return false;
            }

            snprintf(path, sizeof(path), "artists/%s/albums?include_groups=%s&", request->id, group);
        }
        break;

    default:
        return false;
    }

    int size = snprintf(out, capacity, "https://api.spotify.com/v1/%slimit=%d&offset=%u", path,
                        BROWSE_PAGE_SIZE, request->offset);

    return size > 0 && (size_t)size < capacity;
}

const char *browse_label(BrowseKind kind) {
    switch (kind) {
    case BROWSE_SEARCH:
        return "Search";
    case BROWSE_SAVED_TRACKS:
        return "Saved tracks";
    case BROWSE_ARTIST_LIKES:
        return "Liked songs";
    case BROWSE_SONG_RADIO:
        return "Song Radio";
    case BROWSE_SETTINGS:
        return "Settings";
    case BROWSE_SAVED_ALBUMS:
        return "Saved albums";
    case BROWSE_PLAYLISTS:
        return "Your playlists";
    case BROWSE_ALBUM_TRACKS:
        return "Album tracks";
    case BROWSE_PLAYLIST_TRACKS:
        return "Playlist tracks";
    case BROWSE_QUEUE:
        return "Queue";
    case BROWSE_ARTIST_ALBUMS:
        return "Artist albums";
    case BROWSE_PROFILE_PLAYLISTS:
        return "Profile";
    case BROWSE_FOLLOWED_ARTISTS:
        return "Following";
    }

    return "Browse";
}

static const char *item_type(ItemKind kind) {
    switch (kind) {
    case ITEM_TRACK:
        return "track";
    case ITEM_ALBUM:
        return "album";
    case ITEM_PLAYLIST:
        return "playlist";
    case ITEM_EPISODE:
        return "episode";
    case ITEM_ARTIST:
        return "artist";
    }

    return NULL;
}

bool item_link(const CatalogItem *item, char *out, size_t capacity) {
    const char *type = item_type(item->kind);

    if (!type || !spotify_id_valid(item->id)) {
        return false;
    }

    int size = snprintf(out, capacity, "https://open.spotify.com/%s/%s", type, item->id);

    return size > 0 && (size_t)size < capacity;
}

bool item_can_play(const CatalogItem *item) {
    return (item->kind == ITEM_TRACK || item->kind == ITEM_EPISODE) && item->playable &&
           spotify_id_valid(item->id);
}

bool item_play_body(const CatalogItem *item, char *out, size_t capacity) {
    if (!item_can_play(item)) {
        return false;
    }

    int size = snprintf(out, capacity, "{\"uris\":[\"spotify:%s:%s\"]}", item_type(item->kind), item->id);

    return size > 0 && (size_t)size < capacity;
}

/* Spotify user ids are plain account names; anything else is refused before it reaches a JSON body. */
bool browse_user_id_valid(const char *id) {
    size_t length = strlen(id);

    if (!length || length >= 64) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        char c = id[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-')) {
            return false;
        }
    }

    return true;
}

bool item_context_body(const CatalogItem *item, const BrowseRequest *context, char *out, size_t capacity) {
    if ((item->kind == ITEM_ARTIST || item->kind == ITEM_ALBUM || item->kind == ITEM_PLAYLIST) &&
        spotify_id_valid(item->id)) {
        int size =
            snprintf(out, capacity, "{\"context_uri\":\"spotify:%s:%s\"}", item_type(item->kind), item->id);
        return size > 0 && (size_t)size < capacity;
    }

    if (context->kind == BROWSE_SAVED_TRACKS && item_can_play(item) && browse_user_id_valid(context->owner_id)) {
        int size = snprintf(out, capacity,
                            "{\"context_uri\":\"spotify:user:%s:collection\",\"offset\":{\"uri\":\"spotify:%s:%s\"}}",
                            context->owner_id, item_type(item->kind), item->id);
        return size > 0 && (size_t)size < capacity;
    }

    if (context->kind != BROWSE_ALBUM_TRACKS && context->kind != BROWSE_PLAYLIST_TRACKS) {
        return item_play_body(item, out, capacity);
    }

    if (!item_can_play(item) || !spotify_id_valid(context->id)) {
        return false;
    }

    int size =
        snprintf(out, capacity, "{\"context_uri\":\"spotify:%s:%s\",\"offset\":{\"uri\":\"spotify:%s:%s\"}}",
                 context->kind == BROWSE_ALBUM_TRACKS ? "album" : "playlist", context->id,
                 item_type(item->kind), item->id);
    return size > 0 && (size_t)size < capacity;
}

static bool parse_item(yyjson_val *value, BrowseKind kind, CatalogItem *item) {
    if (kind == BROWSE_SAVED_TRACKS) {
        value = yyjson_obj_get(value, "track");
    }
    if (kind == BROWSE_SAVED_ALBUMS) {
        value = yyjson_obj_get(value, "album");
    }

    if (kind == BROWSE_PLAYLIST_TRACKS) {
        yyjson_val *current = yyjson_obj_get(value, "item");
        value = current ? current : yyjson_obj_get(value, "track");
    }

    ItemKind expected = kind == BROWSE_SAVED_ALBUMS || kind == BROWSE_ARTIST_ALBUMS ? ITEM_ALBUM : ITEM_TRACK;

    if (kind == BROWSE_PLAYLISTS || kind == BROWSE_PROFILE_PLAYLISTS) {
        expected = ITEM_PLAYLIST;
    }

    if (kind == BROWSE_FOLLOWED_ARTISTS) {
        expected = ITEM_ARTIST;
    }

    return catalog_item_parse(value, expected, item);
}

bool browse_tracks_url(const BrowseRequest *request, char *out, size_t capacity) {
    if (request->kind == BROWSE_SAVED_TRACKS) {
        int size = snprintf(out, capacity, "https://api.spotify.com/v1/me/tracks?limit=%d&offset=%u",
                            TRACKS_PAGE_SIZE, request->offset);
        return size > 0 && (size_t)size < capacity;
    }

    if (request->kind != BROWSE_PLAYLIST_TRACKS || !spotify_id_valid(request->id)) {
        return false;
    }

    int size = snprintf(out, capacity, "https://api.spotify.com/v1/playlists/%s/items?limit=%d&offset=%u",
                        request->id, TRACKS_PAGE_SIZE, request->offset);
    return size > 0 && (size_t)size < capacity;
}

bool browse_parse_tracks(const char *json, const BrowseRequest *request, TrackPage *page) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *items = yyjson_obj_get(root, "items");
    yyjson_val *total = yyjson_obj_get(root, "total");
    yyjson_val *next = yyjson_obj_get(root, "next");
    yyjson_val *offset = yyjson_obj_get(root, "offset");
    bool valid = yyjson_is_arr(items) && yyjson_arr_size(items) <= TRACKS_PAGE_SIZE && yyjson_is_uint(total) &&
                 yyjson_get_uint(total) <= UINT_MAX && yyjson_is_uint(offset) &&
                 yyjson_get_uint(offset) == request->offset && request->offset <= UINT_MAX - TRACKS_PAGE_SIZE &&
                 (yyjson_is_null(next) || yyjson_is_str(next));

    page->count = 0;
    page->scanned = valid ? (unsigned)yyjson_arr_size(items) : 0;
    page->total = valid ? (unsigned)yyjson_get_uint(total) : 0;
    page->has_next = valid && yyjson_is_str(next) && yyjson_get_len(next) > 0;
    valid = valid && (!page->has_next || page->scanned > 0);

    // Local files and removed songs fail to parse; they still count towards the next offset.
    for (unsigned i = 0; valid && i < page->scanned; ++i) {
        if (parse_item(yyjson_arr_get(items, i), request->kind, &page->items[page->count])) {
            ++page->count;
        }
    }

    yyjson_doc_free(doc);
    return valid;
}

static bool read_paging(yyjson_val *root, const BrowseRequest *request, BrowsePage *page) {
    if (request->kind < BROWSE_SEARCH ||
        (request->kind > BROWSE_PLAYLIST_TRACKS && request->kind != BROWSE_ARTIST_ALBUMS &&
         request->kind != BROWSE_PROFILE_PLAYLISTS && request->kind != BROWSE_FOLLOWED_ARTISTS) ||
        request->offset > maximum_offset(request->kind)) {
        return false;
    }

    yyjson_val *items = yyjson_obj_get(root, "items");
    yyjson_val *offset = yyjson_obj_get(root, "offset");
    yyjson_val *limit = yyjson_obj_get(root, "limit");
    yyjson_val *total = yyjson_obj_get(root, "total");
    yyjson_val *next = yyjson_obj_get(root, "next");

    if (!yyjson_is_arr(items) || yyjson_arr_size(items) > BROWSE_PAGE_SIZE) {
        return false;
    }

    if (request->kind != BROWSE_FOLLOWED_ARTISTS &&
        (!yyjson_is_uint(offset) || yyjson_get_uint(offset) != request->offset)) {
        return false;
    }

    if (!yyjson_is_uint(limit) || !yyjson_get_uint(limit) || yyjson_get_uint(limit) > BROWSE_PAGE_SIZE ||
        yyjson_arr_size(items) > yyjson_get_uint(limit)) {
        return false;
    }

    bool total_optional = request->kind == BROWSE_FOLLOWED_ARTISTS && (!total || yyjson_is_null(total));

    if ((!total_optional && (!yyjson_is_uint(total) || yyjson_get_uint(total) > UINT_MAX)) ||
        (!yyjson_is_null(next) && !yyjson_is_str(next))) {
        return false;
    }

    page->offset = request->offset;
    page->limit = (unsigned)yyjson_get_uint(limit);
    page->total = total_optional ? (unsigned)yyjson_arr_size(items) : (unsigned)yyjson_get_uint(total);
    page->has_next = yyjson_is_str(next) && yyjson_get_len(next) > 0 &&
                     page->offset <= maximum_offset(request->kind) - page->limit;

    if (request->kind == BROWSE_FOLLOWED_ARTISTS && page->has_next) {
        const char *after = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root, "cursors"), "after"));

        if (!spotify_id_valid(after) && yyjson_arr_size(items) > 0) {
            after = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(items, yyjson_arr_size(items) - 1), "id"));
        }

        if (!spotify_id_valid(after) || !strcmp(after, request->after)) {
            page->has_next = false;
        } else {
            snprintf(page->next_after, sizeof(page->next_after), "%s", after);
        }
    }

    return true;
}

bool browse_parse(const char *json, const BrowseRequest *request, BrowsePage *page) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *response = yyjson_doc_get_root(doc);
    yyjson_val *root = response;

    if (request->kind == BROWSE_SEARCH) {
        root = yyjson_obj_get(root, request->filter == SEARCH_ARTISTS     ? "artists"
                                    : request->filter == SEARCH_ALBUMS    ? "albums"
                                    : request->filter == SEARCH_PLAYLISTS ? "playlists"
                                                                          : "tracks");
    }

    if (request->kind == BROWSE_FOLLOWED_ARTISTS) {
        root = yyjson_obj_get(root, "artists");
    }

    BrowsePage parsed = {0};
    bool valid = read_paging(root, request, &parsed);

    if (valid) {
        yyjson_val *items = yyjson_obj_get(root, "items");

        for (size_t i = 0; i < yyjson_arr_size(items); ++i) {
            CatalogItem item = {0};

            bool catalog = request->kind == BROWSE_SEARCH &&
                           (request->filter == SEARCH_ARTISTS || request->filter == SEARCH_ALBUMS ||
                            request->filter == SEARCH_PLAYLISTS);
            ItemKind catalog_kind = request->filter == SEARCH_ARTISTS  ? ITEM_ARTIST
                                    : request->filter == SEARCH_ALBUMS ? ITEM_ALBUM
                                                                       : ITEM_PLAYLIST;
            bool ok = catalog ? catalog_item_parse(yyjson_arr_get(items, i), catalog_kind, &item)
                              : parse_item(yyjson_arr_get(items, i), request->kind, &item);

            if (ok &&
                (request->kind != BROWSE_PROFILE_PLAYLISTS || (request->owner_id[0] && item.public_playlist &&
                                                               !strcmp(item.owner_id, request->owner_id)))) {
                if (request->kind == BROWSE_ALBUM_TRACKS && !item.album_id[0] &&
                    spotify_id_valid(request->id)) {
                    snprintf(item.album_id, sizeof(item.album_id), "%s", request->id);
                    copy_utf8(item.album_title, sizeof(item.album_title), request->title);
                }
                parsed.items[parsed.count++] = item;
            }
        }

        if (request->kind == BROWSE_SEARCH && request->filter == SEARCH_ALL && !request->offset) {
            yyjson_val *artists = yyjson_obj_get(yyjson_obj_get(response, "artists"), "items");
            yyjson_val *albums = yyjson_obj_get(yyjson_obj_get(response, "albums"), "items");
            yyjson_val *playlists = yyjson_obj_get(yyjson_obj_get(response, "playlists"), "items");

            for (size_t i = 0; i < yyjson_arr_size(artists) && parsed.artist_count < 5; ++i) {
                if (catalog_item_parse(yyjson_arr_get(artists, i), ITEM_ARTIST,
                                       &parsed.artists[parsed.artist_count])) {
                    ++parsed.artist_count;
                }
            }

            for (size_t i = 0; i < yyjson_arr_size(albums) && parsed.album_count < 5; ++i) {
                if (catalog_item_parse(yyjson_arr_get(albums, i), ITEM_ALBUM,
                                       &parsed.albums[parsed.album_count])) {
                    ++parsed.album_count;
                }
            }

            for (size_t i = 0; i < yyjson_arr_size(playlists) && parsed.playlist_count < 5; ++i) {
                if (catalog_item_parse(yyjson_arr_get(playlists, i), ITEM_PLAYLIST,
                                       &parsed.playlists[parsed.playlist_count])) {
                    ++parsed.playlist_count;
                }
            }
        }

        *page = parsed;
    }

    yyjson_doc_free(doc);

    return valid;
}

void browser_reset(Browser *browser) {
    memset(browser, 0, sizeof(*browser));
    browser->selected = -1;
}

void browser_commit(Browser *browser, const BrowseRequest *request, const BrowsePage *page) {
    browser->request = *request;
    browser->page = *page;
    browser->selected = -1;
    browser->loaded = true;
}

bool browser_select(Browser *browser, int index) {
    if (index < -1 || (index >= 0 && (unsigned)index >= browser->page.count)) {
        return false;
    }

    browser->selected = index;

    return true;
}

const CatalogItem *browser_selected(const Browser *browser) {
    if (browser->selected < 0 || (unsigned)browser->selected >= browser->page.count) {
        return NULL;
    }

    return &browser->page.items[browser->selected];
}

bool browser_page_request(const Browser *browser, int direction, BrowseRequest *request) {
    if (!browser->loaded || !browser->page.limit || browser->page.limit > BROWSE_PAGE_SIZE ||
        (direction != -1 && direction != 1)) {
        return false;
    }

    if (direction == 1 &&
        (!browser->page.has_next ||
         browser->request.offset > maximum_offset(browser->request.kind) - browser->page.limit)) {
        return false;
    }

    if (direction == -1 && !browser->request.offset) {
        return false;
    }

    *request = browser->request;

    if (direction == 1) {
        request->offset += browser->page.limit;
    }
    if (direction == -1) {
        request->offset = request->offset > browser->page.limit ? request->offset - browser->page.limit : 0;
    }

    return true;
}

bool browser_child_request(const Browser *browser, BrowseRequest *request) {
    const CatalogItem *item = browser_selected(browser);
    return item && item_browse_request(item, request);
}

bool item_browse_request(const CatalogItem *item, BrowseRequest *request) {
    if ((item->kind != ITEM_ALBUM && item->kind != ITEM_PLAYLIST && item->kind != ITEM_ARTIST) ||
        !spotify_id_valid(item->id)) {
        return false;
    }

    memset(request, 0, sizeof(*request));
    request->kind = item->kind == ITEM_ALBUM    ? BROWSE_ALBUM_TRACKS
                    : item->kind == ITEM_ARTIST ? BROWSE_ARTIST_ALBUMS
                                                : BROWSE_PLAYLIST_TRACKS;
    snprintf(request->id, sizeof(request->id), "%s", item->id);
    copy_utf8(request->title, sizeof(request->title), item->title);
    copy_utf8(request->artwork, sizeof(request->artwork), item->artwork);

    return true;
}

static bool valid_input(const char *utf8, size_t size) {
    for (size_t i = 0; i < size;) {
        unsigned char lead = (unsigned char)utf8[i++];

        if (lead < 32 || lead == 127) {
            return false;
        }
        if (lead < 128) {
            continue;
        }

        unsigned trailing = lead >= 0xc2 && lead <= 0xdf   ? 1
                            : lead >= 0xe0 && lead <= 0xef ? 2
                            : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                           : 0;

        if (!trailing || trailing > size - i) {
            return false;
        }

        unsigned char next = (unsigned char)utf8[i];

        if ((lead == 0xe0 && next < 0xa0) || (lead == 0xed && next >= 0xa0) ||
            (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next >= 0x90)) {
            return false;
        }

        for (unsigned j = 0; j < trailing; ++j) {
            if (((unsigned char)utf8[i++] & 0xc0) != 0x80) {
                return false;
            }
        }
    }

    return true;
}

bool search_append(char query[SEARCH_CAPACITY], const char *utf8, size_t size) {
    size_t length = strlen(query);

    if (size >= SEARCH_CAPACITY - length || !valid_input(utf8, size)) {
        return false;
    }

    memcpy(query + length, utf8, size);
    query[length + size] = 0;

    return true;
}

void search_backspace(char query[SEARCH_CAPACITY]) {
    size_t end = strlen(query);

    if (!end) {
        return;
    }

    --end;

    while (end > 0 && ((unsigned char)query[end] & 0xc0) == 0x80) {
        --end;
    }

    query[end] = 0;
}

#define SEARCH_TOKEN_LIMIT 12
#define SEARCH_WORD_LIMIT 32

typedef struct {
    char clean[SEARCH_CAPACITY];
    const char *tokens[SEARCH_TOKEN_LIMIT];
    char storage[SEARCH_CAPACITY];
    unsigned count;
    bool op_artist;
    bool op_album;
    bool op_track;
    bool op_playlist;
    bool op_catalog;
    bool hint_playlist;
    bool hint_album;
    bool hint_track;
} SearchContext;

static char fold_latin1(unsigned char b) {
    if (b >= 0xa0) {
        b -= 0x20;
    }

    if (b >= 0x80 && b <= 0x85) {
        return 'a';
    }

    if (b == 0x87) {
        return 'c';
    }

    if (b >= 0x88 && b <= 0x8b) {
        return 'e';
    }

    if (b >= 0x8c && b <= 0x8f) {
        return 'i';
    }

    if (b == 0x90) {
        return 'd';
    }

    if (b == 0x91) {
        return 'n';
    }

    if ((b >= 0x92 && b <= 0x96) || b == 0x98) {
        return 'o';
    }

    if (b >= 0x99 && b <= 0x9c) {
        return 'u';
    }

    if (b == 0x9d || b == 0x9f) {
        return 'y';
    }

    return 0;
}

static char fold_latin_ext(unsigned char c1, unsigned char c2) {
    unsigned code = ((unsigned)(c1 & 0x1f) << 6) | (unsigned)(c2 & 0x3f);

    if (code <= 0x105) {
        return 'a';
    }

    if (code <= 0x10d) {
        return 'c';
    }

    if (code <= 0x111) {
        return 'd';
    }

    if (code <= 0x11b) {
        return 'e';
    }

    if (code <= 0x123) {
        return 'g';
    }

    if (code <= 0x127) {
        return 'h';
    }

    if (code <= 0x131) {
        return 'i';
    }

    if (code <= 0x135) {
        return 'j';
    }

    if (code <= 0x138) {
        return 'k';
    }

    if (code <= 0x142) {
        return 'l';
    }

    if (code <= 0x14b) {
        return 'n';
    }

    if (code <= 0x151) {
        return 'o';
    }

    if (code >= 0x154 && code <= 0x159) {
        return 'r';
    }

    if (code <= 0x161) {
        return 's';
    }

    if (code <= 0x167) {
        return 't';
    }

    if (code <= 0x173) {
        return 'u';
    }

    if (code <= 0x175) {
        return 'w';
    }

    if (code <= 0x178) {
        return 'y';
    }

    if (code <= 0x17e) {
        return 'z';
    }

    return 0;
}

static void normalize_text(const char *src, char *out, size_t capacity) {
    if (!capacity) {
        return;
    }

    if (!src) {
        out[0] = 0;
        return;
    }

    size_t pos = 0;
    bool pending_space = false;

    for (const unsigned char *p = (const unsigned char *)src; *p && pos + 1 < capacity;) {
        if (*p < 0x80) {
            unsigned char c = *p++;

            if (c >= 'A' && c <= 'Z') {
                c = (unsigned char)(c + 32);
            }

            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                if (pending_space && pos) {
                    out[pos++] = ' ';
                }

                pending_space = false;

                if (pos + 1 < capacity) {
                    out[pos++] = (char)c;
                }
            } else if (c != '\'') {
                pending_space = pos > 0;
            }

            continue;
        }

        if (p[0] == 0xc3 && p[1] >= 0x80 && p[1] <= 0xbf) {
            unsigned char b = p[1];
            p += 2;

            if (b == 0x9f) {
                if (pending_space && pos && pos + 1 < capacity) {
                    out[pos++] = ' ';
                }

                pending_space = false;

                if (pos + 1 < capacity) {
                    out[pos++] = 's';
                }

                continue;
            }

            if (b == 0x86 || b == 0xa6) {
                if (pending_space && pos && pos + 1 < capacity) {
                    out[pos++] = ' ';
                }

                pending_space = false;

                if (pos + 2 < capacity) {
                    out[pos++] = 'a';
                    out[pos++] = 'e';
                }

                continue;
            }

            char folded = fold_latin1(b);

            if (folded) {
                if (pending_space && pos) {
                    out[pos++] = ' ';
                }

                pending_space = false;

                if (pos + 1 < capacity) {
                    out[pos++] = folded;
                }
            } else {
                pending_space = pos > 0;
            }

            continue;
        }

        if ((p[0] == 0xc4 || p[0] == 0xc5) && (p[1] & 0xc0) == 0x80) {
            char folded = fold_latin_ext(p[0], p[1]);
            p += 2;

            if (folded) {
                if (pending_space && pos) {
                    out[pos++] = ' ';
                }

                pending_space = false;

                if (pos + 1 < capacity) {
                    out[pos++] = folded;
                }
            }

            continue;
        }

        if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] >= 0x90 && p[2] <= 0x99)) {
            unsigned char third = p[2];
            p += 3;

            if (third != 0x98 && third != 0x99) {
                pending_space = pos > 0;
            }

            continue;
        }

        size_t len = (p[0] & 0xe0) == 0xc0 ? 2 : (p[0] & 0xf0) == 0xe0 ? 3 : (p[0] & 0xf8) == 0xf0 ? 4 : 1;

        for (size_t k = 1; k < len; ++k) {
            if ((p[k] & 0xc0) != 0x80) {
                len = 1;
                break;
            }
        }

        if (pending_space && pos && pos + 1 < capacity) {
            out[pos++] = ' ';
        }

        pending_space = false;

        for (size_t k = 0; k < len && pos + 1 < capacity; ++k) {
            out[pos++] = (char)*p++;
        }
    }

    out[pos] = 0;
}

static bool has_edition_keyword(const char *suffix) {
    char norm[256];
    normalize_text(suffix, norm, sizeof(norm));

    static const char *keywords[] = {
        "remaster", "remastered", "deluxe", "live",   "acoustic", "version",  "edit",        "mix",
        "remix",    "feat",       "ft",     "mono",   "stereo",   "explicit", "anniversary", "expanded",
        "bonus",    "edition",    "radio",  "single", "sped up",  "slowed",   "instrumental"};

    for (unsigned i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (strstr(norm, keywords[i])) {
            return true;
        }
    }

    for (const char *p = norm; *p; ++p) {
        if (((p[0] == '1' && p[1] == '9') || (p[0] == '2' && p[1] == '0')) && p[2] >= '0' && p[2] <= '9' &&
            p[3] >= '0' && p[3] <= '9') {
            return true;
        }
    }

    return false;
}

static void normalize_core_title(const char *title, char *out, size_t capacity) {
    char trimmed[256];
    copy_utf8(trimmed, sizeof(trimmed), title);

    const char *cut = NULL;
    const char *markers[] = {" - ", " \xe2\x80\x93 ", " (", " ["};

    for (unsigned i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        const char *found = strstr(trimmed, markers[i]);

        if (found && found > trimmed && has_edition_keyword(found)) {
            if (!cut || found < cut) {
                cut = found;
            }
        }
    }

    if (cut) {
        trimmed[cut - trimmed] = 0;
    }

    normalize_text(trimmed, out, capacity);
}

static unsigned split_words(char *text, const char **words, unsigned limit) {
    unsigned count = 0;

    for (char *p = text; *p && count < limit;) {
        while (*p == ' ') {
            ++p;
        }

        if (!*p) {
            break;
        }

        words[count++] = p;

        while (*p && *p != ' ') {
            ++p;
        }

        if (*p) {
            *p++ = 0;
        }
    }

    return count;
}

static bool has_prefix_ci(const char *text, const char *prefix, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char a = (unsigned char)text[i];
        unsigned char b = (unsigned char)prefix[i];

        if (!a) {
            return false;
        }

        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + 32);
        }

        if (a != b) {
            return false;
        }
    }

    return true;
}

static void append_clean_words(SearchContext *ctx, const char *raw) {
    char norm[SEARCH_CAPACITY];
    normalize_text(raw, norm, sizeof(norm));

    if (!norm[0]) {
        return;
    }

    size_t used = strlen(ctx->clean);

    if (used && used + 1 < sizeof(ctx->clean)) {
        ctx->clean[used++] = ' ';
    }

    copy_utf8(ctx->clean + used, sizeof(ctx->clean) - used, norm);
}

static void parse_query_context(const char *query, SearchContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    if (!query || !query[0]) {
        return;
    }

    for (const char *p = query; *p;) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }

        if (!*p) {
            break;
        }

        char piece[SEARCH_CAPACITY];
        size_t len = 0;
        bool in_quotes = false;

        while (*p && (in_quotes || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                in_quotes = !in_quotes;
                ++p;
                continue;
            }

            if (len + 1 < sizeof(piece)) {
                piece[len++] = *p;
            }

            ++p;
        }

        piece[len] = 0;

        if (has_prefix_ci(piece, "artist:", 7)) {
            ctx->op_artist = true;
            append_clean_words(ctx, piece + 7);
        } else if (has_prefix_ci(piece, "album:", 6)) {
            ctx->op_album = true;
            append_clean_words(ctx, piece + 6);
        } else if (has_prefix_ci(piece, "track:", 6)) {
            ctx->op_track = true;
            append_clean_words(ctx, piece + 6);
        } else if (has_prefix_ci(piece, "playlist:", 9)) {
            ctx->op_playlist = true;
            append_clean_words(ctx, piece + 9);
        } else if (has_prefix_ci(piece, "label:", 6) || has_prefix_ci(piece, "year:", 5) ||
                   has_prefix_ci(piece, "genre:", 6) || has_prefix_ci(piece, "tag:", 4) ||
                   has_prefix_ci(piece, "isrc:", 5) || has_prefix_ci(piece, "upc:", 4)) {
            ctx->op_catalog = true;
        } else {
            append_clean_words(ctx, piece);
        }
    }

    copy_utf8(ctx->storage, sizeof(ctx->storage), ctx->clean);
    ctx->count = split_words(ctx->storage, ctx->tokens, SEARCH_TOKEN_LIMIT);

    if (ctx->count > 1) {
        for (unsigned i = 0; i < ctx->count; ++i) {
            if (!strcmp(ctx->tokens[i], "playlist") || !strcmp(ctx->tokens[i], "playlists")) {
                ctx->hint_playlist = true;
            } else if (!strcmp(ctx->tokens[i], "album") || !strcmp(ctx->tokens[i], "ep")) {
                ctx->hint_album = true;
            } else if (!strcmp(ctx->tokens[i], "song") || !strcmp(ctx->tokens[i], "track")) {
                ctx->hint_track = true;
            }
        }
    }
}

static unsigned word_match_score(const char *token, bool is_last, const char **words, unsigned word_count) {
    size_t len = strlen(token);
    unsigned best = 0;

    for (unsigned i = 0; i < word_count; ++i) {
        if (!strcmp(token, words[i])) {
            return 100;
        }

        if ((len >= 2 || is_last) && !strncmp(words[i], token, len)) {
            best = 70;
        }
    }

    return best;
}

static bool phrase_match(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);

    if (!nlen) {
        return false;
    }

    const char *found = strstr(haystack, needle);

    while (found) {
        bool left_ok = found == haystack || found[-1] == ' ';
        bool right_ok = found[nlen] == 0 || found[nlen] == ' ';

        if (left_ok && right_ok) {
            return true;
        }

        found = strstr(found + 1, needle);
    }

    return false;
}

static unsigned artist_coherence(const CatalogItem *artist, const char *norm_artist,
                                 const CatalogItem *tracks, unsigned track_count, const CatalogItem *albums,
                                 unsigned album_count) {
    unsigned track_hits = 0;
    unsigned album_hits = 0;

    for (unsigned i = 0; i < track_count && i < 5; ++i) {
        if ((artist->id[0] && catalog_track_has_artist(&tracks[i], artist->id))) {
            ++track_hits;
            continue;
        }

        char norm_detail[256];
        normalize_text(tracks[i].detail, norm_detail, sizeof(norm_detail));

        if (norm_artist[0] && !strcmp(norm_detail, norm_artist)) {
            ++track_hits;
        }
    }

    for (unsigned i = 0; i < album_count && i < 5; ++i) {
        if (artist->id[0] && albums[i].artist_id[0] && !strcmp(albums[i].artist_id, artist->id)) {
            ++album_hits;
            continue;
        }

        char norm_detail[256];
        normalize_text(albums[i].detail, norm_detail, sizeof(norm_detail));

        if (norm_artist[0] && !strcmp(norm_detail, norm_artist)) {
            ++album_hits;
        }
    }

    unsigned score = track_hits * 24 + album_hits * 16;
    return score > 120 ? 120 : score;
}

static unsigned album_coherence(const CatalogItem *album, const char *norm_album, const char *norm_detail,
                                const CatalogItem *tracks, unsigned track_count, bool *is_single_wrapper) {
    unsigned matches = 0;
    *is_single_wrapper = false;

    for (unsigned i = 0; i < track_count && i < 5; ++i) {
        if (album->id[0] && tracks[i].album_id[0] && !strcmp(tracks[i].album_id, album->id)) {
            ++matches;
        } else {
            char t_album[256];
            char t_artist[256];
            normalize_text(tracks[i].album_title, t_album, sizeof(t_album));
            normalize_text(tracks[i].detail, t_artist, sizeof(t_artist));

            if (norm_album[0] && !strcmp(t_album, norm_album) &&
                (!norm_detail[0] || !strcmp(t_artist, norm_detail))) {
                ++matches;
            }
        }

        if (i < 3 && !strcmp(album->album_type, "single")) {
            char t_core[256];
            char t_artist[256];
            normalize_core_title(tracks[i].title, t_core, sizeof(t_core));
            normalize_text(tracks[i].detail, t_artist, sizeof(t_artist));

            if (norm_album[0] && !strcmp(t_core, norm_album) &&
                (!norm_detail[0] || !t_artist[0] || !strcmp(t_artist, norm_detail))) {
                *is_single_wrapper = true;
            }
        }
    }

    unsigned score = matches * 35;
    return score > 140 ? 140 : score;
}

static int score_search_candidate(const SearchContext *ctx, const CatalogItem *item, unsigned index,
                                  const CatalogItem *tracks, unsigned track_count, const CatalogItem *artists,
                                  unsigned artist_count, const CatalogItem *albums, unsigned album_count) {
    (void)artists;
    char norm_title[256];
    char core_title[256];
    char norm_detail[256];
    char norm_guests[256];
    char norm_album[256];

    normalize_text(item->title, norm_title, sizeof(norm_title));
    normalize_core_title(item->title, core_title, sizeof(core_title));
    normalize_text(item->detail, norm_detail, sizeof(norm_detail));
    normalize_text(item->guest_artists, norm_guests, sizeof(norm_guests));
    normalize_text(item->album_title, norm_album, sizeof(norm_album));

    char title_buf[256], detail_buf[256], guest_buf[256], album_buf[256];
    copy_utf8(title_buf, sizeof(title_buf), norm_title);
    copy_utf8(detail_buf, sizeof(detail_buf), norm_detail);
    copy_utf8(guest_buf, sizeof(guest_buf), norm_guests);
    copy_utf8(album_buf, sizeof(album_buf), norm_album);

    const char *title_words[SEARCH_WORD_LIMIT];
    const char *detail_words[SEARCH_WORD_LIMIT];
    const char *guest_words[SEARCH_WORD_LIMIT];
    const char *album_words[SEARCH_WORD_LIMIT];
    unsigned n_title = split_words(title_buf, title_words, SEARCH_WORD_LIMIT);
    unsigned n_detail = split_words(detail_buf, detail_words, SEARCH_WORD_LIMIT);
    unsigned n_guest = split_words(guest_buf, guest_words, SEARCH_WORD_LIMIT);
    unsigned n_album = split_words(album_buf, album_words, SEARCH_WORD_LIMIT);

    int score = 0;

    if (ctx->count > 0) {
        size_t clean_len = strlen(ctx->clean);

        if (!strcmp(norm_title, ctx->clean)) {
            score += 340;
        } else if (!strcmp(core_title, ctx->clean)) {
            score += 300;
        } else if (clean_len >= 3 && !strncmp(norm_title, ctx->clean, clean_len)) {
            score += 200;
        } else if (ctx->count >= 2 && phrase_match(norm_title, ctx->clean)) {
            score += 150;
        }

        unsigned title_sum = 0;
        unsigned all_sum = 0;
        unsigned title_hits = 0;
        unsigned detail_hits = 0;

        for (unsigned i = 0; i < ctx->count; ++i) {
            bool is_last = i + 1 == ctx->count;
            unsigned tw = word_match_score(ctx->tokens[i], is_last, title_words, n_title);
            unsigned dw = word_match_score(ctx->tokens[i], is_last, detail_words, n_detail);
            unsigned gw = word_match_score(ctx->tokens[i], is_last, guest_words, n_guest);
            unsigned aw = word_match_score(ctx->tokens[i], is_last, album_words, n_album);

            if (gw > dw) {
                dw = gw;
            }

            unsigned best = tw > dw ? tw : dw;

            if (aw > best) {
                best = aw;
            }

            title_sum += tw;
            all_sum += best;

            if (tw >= 70) {
                ++title_hits;
            }

            if (dw >= 70) {
                ++detail_hits;
            }
        }

        score += (int)((140u * title_sum) / (ctx->count * 100u));

        if (ctx->count >= 2 && all_sum >= ctx->count * 85u && title_hits > 0 && detail_hits > 0) {
            score += 260;
        } else if (title_hits > 0 && all_sum > title_sum) {
            score += (int)((50u * (all_sum - title_sum)) / (ctx->count * 100u));
        } else if (title_hits == 0 && all_sum > 0) {
            score += (int)((20u * all_sum) / (ctx->count * 100u));
        }

        if (n_title > title_hits) {
            unsigned extra = n_title - title_hits;
            score -= (int)(extra > 6 ? 36 : extra * 6);
        }

        if (item->kind == ITEM_ARTIST) {
            unsigned coh = artist_coherence(item, norm_title, tracks, track_count, albums, album_count);

            if (title_hits > 0) {
                score += (int)coh;
            }

            if (coh == 0 && track_count > 0 && !ctx->op_artist) {
                score -= 40;
            }
        } else if (item->kind == ITEM_ALBUM) {
            bool single_wrapper = false;
            unsigned coh =
                album_coherence(item, norm_title, norm_detail, tracks, track_count, &single_wrapper);

            if (single_wrapper) {
                score -= 100;
            } else if (title_hits > 0) {
                score += (int)coh;

                if (!strcmp(item->album_type, "album") && coh > 0) {
                    score += 25;
                }
            }
        } else if (item->kind == ITEM_PLAYLIST) {
            score -= 40;

            if (!strcmp(item->owner_id, "spotify")) {
                score += 65;
            }
        }
    } else if (item->kind == ITEM_ARTIST && !ctx->op_artist && !ctx->op_catalog && track_count > 0) {
        score -= 10;
    }

    if (index < 5) {
        score += (int)((5 - index) * 14);
    }

    score += (int)((item->popularity * 80u) / 100u);

    if (ctx->op_artist && item->kind == ITEM_ARTIST) {
        score += 400;
    }

    if (ctx->op_album && item->kind == ITEM_ALBUM) {
        score += 400;
    }

    if (ctx->op_track && item->kind == ITEM_TRACK) {
        score += 400;
    }

    if (ctx->op_playlist && item->kind == ITEM_PLAYLIST) {
        score += 400;
    }

    if (ctx->op_catalog && (item->kind == ITEM_TRACK || item->kind == ITEM_ALBUM)) {
        score += 220;
    }

    if (ctx->hint_playlist && item->kind == ITEM_PLAYLIST) {
        score += 90;
    }

    if (ctx->hint_album && item->kind == ITEM_ALBUM) {
        score += 70;
    }

    if (ctx->hint_track && item->kind == ITEM_TRACK) {
        score += 70;
    }

    (void)artist_count;
    return score;
}

const CatalogItem *browse_search_top(const char *query, const CatalogItem *tracks, unsigned track_count,
                                     const CatalogItem *artists, unsigned artist_count,
                                     const CatalogItem *albums, unsigned album_count,
                                     const CatalogItem *playlists, unsigned playlist_count) {
    SearchContext ctx;
    parse_query_context(query, &ctx);

    const CatalogItem *best = NULL;
    int best_score = INT_MIN;

    const CatalogItem *groups[] = {tracks, artists, albums, playlists};
    unsigned counts[] = {track_count, artist_count, album_count, playlist_count};

    for (unsigned g = 0; g < 4; ++g) {
        unsigned limit = counts[g] < 5 ? counts[g] : 5;

        for (unsigned i = 0; i < limit; ++i) {
            const CatalogItem *item = &groups[g][i];

            if (!item->id[0] && !item->title[0]) {
                continue;
            }

            int score = score_search_candidate(&ctx, item, i, tracks, track_count, artists, artist_count,
                                               albums, album_count);

            if (!best || score > best_score) {
                best = item;
                best_score = score;
            }
        }
    }

    return best;
}
