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

        const char *type = request->filter == SEARCH_ALL       ? "track,artist,album"
                           : request->filter == SEARCH_SONGS   ? "track"
                           : request->filter == SEARCH_ARTISTS ? "artist"
                           : request->filter == SEARCH_ALBUMS  ? "album"
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

bool item_context_body(const CatalogItem *item, const BrowseRequest *context, char *out, size_t capacity) {
    if ((item->kind == ITEM_ARTIST || item->kind == ITEM_ALBUM || item->kind == ITEM_PLAYLIST) &&
        spotify_id_valid(item->id)) {
        int size =
            snprintf(out, capacity, "{\"context_uri\":\"spotify:%s:%s\"}", item_type(item->kind), item->id);
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
        root = yyjson_obj_get(root, request->filter == SEARCH_ARTISTS  ? "artists"
                                    : request->filter == SEARCH_ALBUMS ? "albums"
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
                           (request->filter == SEARCH_ARTISTS || request->filter == SEARCH_ALBUMS);
            bool ok =
                catalog
                    ? catalog_item_parse(yyjson_arr_get(items, i),
                                         request->filter == SEARCH_ARTISTS ? ITEM_ARTIST : ITEM_ALBUM, &item)
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
