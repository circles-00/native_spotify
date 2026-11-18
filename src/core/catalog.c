#include "catalog.h"
#include "core.h"

#include <stdio.h>
#include <string.h>

bool catalog_item_parse(yyjson_val *value, ItemKind kind, CatalogItem *item) {
    const char *id = yyjson_get_str(yyjson_obj_get(value, "id"));
    const char *name = yyjson_get_str(yyjson_obj_get(value, "name"));
    const char *type = yyjson_get_str(yyjson_obj_get(value, "type"));
    const char *expected = kind == ITEM_TRACK     ? "track"
                           : kind == ITEM_EPISODE ? "episode"
                           : kind == ITEM_ALBUM   ? "album"
                           : kind == ITEM_ARTIST  ? "artist"
                                                  : "playlist";

    if (!spotify_id_valid(id) || !name || (type && strcmp(type, expected)) ||
        yyjson_is_true(yyjson_obj_get(value, "is_local"))) {
        return false;
    }

    const char *detail =
        yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(value, "artists"), 0), "name"));

    if (kind == ITEM_PLAYLIST) {
        detail = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(value, "owner"), "display_name"));
    }

    if (kind == ITEM_EPISODE) {
        detail = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(value, "show"), "name"));
    }

    memset(item, 0, sizeof(*item));
    item->kind = kind;
    snprintf(item->id, sizeof(item->id), "%s", id);
    copy_utf8(item->title, sizeof(item->title), name[0] ? name : "Untitled");
    copy_utf8(item->detail, sizeof(item->detail), detail ? detail : "");
    const char *artist_id =
        yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(value, "artists"), 0), "id"));

    if (spotify_id_valid(artist_id)) {
        snprintf(item->artist_id, sizeof(item->artist_id), "%s", artist_id);
    }

    yyjson_val *album_obj = yyjson_obj_get(value, "album");
    const char *album_id = yyjson_get_str(yyjson_obj_get(album_obj, "id"));
    const char *album_title = yyjson_get_str(yyjson_obj_get(album_obj, "name"));

    if (album_id && spotify_id_valid(album_id)) {
        copy_utf8(item->album_id, sizeof(item->album_id), album_id);
    }
    copy_utf8(item->album_title, sizeof(item->album_title), album_title ? album_title : "");

    const char *date = yyjson_get_str(yyjson_obj_get(value, "release_date"));
    const char *album_type = yyjson_get_str(yyjson_obj_get(value, "album_type"));
    copy_utf8(item->release_date, sizeof(item->release_date), date ? date : "");
    copy_utf8(item->album_type, sizeof(item->album_type), album_type ? album_type : "");
    item->explicit_content = yyjson_is_true(yyjson_obj_get(value, "explicit"));
    const char *owner = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(value, "owner"), "id"));
    copy_utf8(item->owner_id, sizeof(item->owner_id), owner ? owner : "");
    item->public_playlist = yyjson_is_true(yyjson_obj_get(value, "public"));
    yyjson_val *number = yyjson_obj_get(value, "track_number");
    yyjson_val *disc = yyjson_obj_get(value, "disc_number");
    item->track_number = yyjson_is_uint(number) && yyjson_get_uint(number) <= UINT32_MAX
                             ? (unsigned)yyjson_get_uint(number)
                             : 0;
    item->disc_number =
        yyjson_is_uint(disc) && yyjson_get_uint(disc) <= UINT32_MAX ? (unsigned)yyjson_get_uint(disc) : 0;

    yyjson_val *images =
        yyjson_obj_get(kind == ITEM_TRACK ? yyjson_obj_get(value, "album") : value, "images");
    artwork_url_parse(images, item->artwork);

    item->playable = !yyjson_is_false(yyjson_obj_get(value, "is_playable")) &&
                     !yyjson_is_obj(yyjson_obj_get(value, "restrictions"));
    yyjson_val *duration = yyjson_obj_get(value, "duration_ms");

    if (yyjson_is_uint(duration) && yyjson_get_uint(duration) <= UINT32_MAX) {
        item->duration_ms = (unsigned)yyjson_get_uint(duration);
        item->duration_known = true;
    }

    return true;
}

bool recent_albums_parse(const char *json, BrowsePage *page) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *items = yyjson_obj_get(yyjson_doc_get_root(doc), "items");
    bool valid = yyjson_is_arr(items) && yyjson_arr_size(items) <= 50;
    BrowsePage parsed = {.limit = 8};

    if (valid) {
        for (size_t i = 0; i < yyjson_arr_size(items) && parsed.count < 8; ++i) {
            yyjson_val *track = yyjson_obj_get(yyjson_arr_get(items, i), "track");
            CatalogItem album;

            if (!catalog_item_parse(yyjson_obj_get(track, "album"), ITEM_ALBUM, &album)) {
                continue;
            }

            bool duplicate = false;

            for (unsigned j = 0; j < parsed.count; ++j) {
                duplicate = duplicate || !strcmp(parsed.items[j].id, album.id);
            }

            if (!duplicate) {
                parsed.items[parsed.count++] = album;
            }
        }

        parsed.total = parsed.count;
        *page = parsed;
    }

    yyjson_doc_free(doc);
    return valid;
}
