#include "collection.h"
#include "catalog.h"
#include "core.h"

#include <stdio.h>
#include <string.h>

bool collection_page(BrowseKind kind) {
    return kind == BROWSE_ALBUM_TRACKS || kind == BROWSE_ARTIST_ALBUMS || kind == BROWSE_PROFILE_PLAYLISTS;
}

bool profile_parse(const char *json, ProfileInfo *profile) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *id = yyjson_get_str(yyjson_obj_get(root, "id"));
    const char *name = yyjson_get_str(yyjson_obj_get(root, "display_name"));
    bool valid = id && id[0] && strlen(id) < sizeof(profile->id);

    if (valid) {
        *profile = (ProfileInfo){.loaded = true};
        copy_utf8(profile->id, sizeof(profile->id), id);
        copy_utf8(profile->name, sizeof(profile->name), name && name[0] ? name : id);
        artwork_url_parse(yyjson_obj_get(root, "images"), profile->artwork);
        yyjson_val *followers = yyjson_obj_get(yyjson_obj_get(root, "followers"), "total");
        profile->followers_known = yyjson_is_uint(followers);
        profile->followers = yyjson_get_uint(followers);
    }

    yyjson_doc_free(doc);
    return valid;
}

bool collection_parse(const char *json, ItemKind kind, CollectionInfo *info) {
    if (kind != ITEM_ARTIST && kind != ITEM_ALBUM) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    CollectionInfo parsed = {0};
    bool valid = catalog_item_parse(root, kind, &parsed.item);

    if (valid) {
        parsed.loaded = true;
        catalog_item_parse(yyjson_arr_get(yyjson_obj_get(root, "artists"), 0), ITEM_ARTIST, &parsed.artist);
        yyjson_val *total = yyjson_obj_get(root, "total_tracks");
        parsed.total_tracks = yyjson_is_uint(total) && yyjson_get_uint(total) <= UINT32_MAX
                                  ? (unsigned)yyjson_get_uint(total)
                                  : 0;
        yyjson_val *followers = yyjson_obj_get(yyjson_obj_get(root, "followers"), "total");
        parsed.followers_known = yyjson_is_uint(followers);
        parsed.followers = yyjson_get_uint(followers);
        const char *precision = yyjson_get_str(yyjson_obj_get(root, "release_date_precision"));
        const char *label = yyjson_get_str(yyjson_obj_get(root, "label"));
        copy_utf8(parsed.release_precision, sizeof(parsed.release_precision), precision ? precision : "");
        copy_utf8(parsed.label, sizeof(parsed.label), label ? label : "");
        yyjson_val *copyrights = yyjson_obj_get(root, "copyrights");

        for (size_t i = 0; i < yyjson_arr_size(copyrights) && i < 8; ++i) {
            const char *text = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(copyrights, i), "text"));
            const char *type = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(copyrights, i), "type"));

            if (!text) {
                continue;
            }

            char line[512];
            snprintf(line, sizeof(line), "%s%s%s", parsed.copyrights[0] ? "\n" : "",
                     type && !strcmp(type, "P") ? "℗ " : "© ", text);
            size_t length = strlen(parsed.copyrights);
            copy_utf8(parsed.copyrights + length, sizeof(parsed.copyrights) - length, line);
        }

        yyjson_val *tracks = yyjson_obj_get(yyjson_obj_get(root, "tracks"), "items");
        parsed.duration_complete = kind == ITEM_ALBUM && parsed.total_tracks > 0 && yyjson_is_arr(tracks) &&
                                   yyjson_arr_size(tracks) == parsed.total_tracks;

        for (size_t i = 0; parsed.duration_complete && i < yyjson_arr_size(tracks); ++i) {
            yyjson_val *duration = yyjson_obj_get(yyjson_arr_get(tracks, i), "duration_ms");
            parsed.duration_complete = yyjson_is_uint(duration) && yyjson_get_uint(duration) <= UINT32_MAX;
            parsed.duration_ms += parsed.duration_complete ? yyjson_get_uint(duration) : 0;
        }

        *info = parsed;
    }

    yyjson_doc_free(doc);
    return valid;
}

bool collection_popular_parse(const char *json, CollectionInfo *info) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *tracks = yyjson_obj_get(yyjson_doc_get_root(doc), "tracks");
    bool valid = yyjson_is_arr(tracks) && yyjson_arr_size(tracks) <= 10;

    if (valid) {
        info->popular_count = 0;

        for (size_t i = 0; i < yyjson_arr_size(tracks); ++i) {
            if (catalog_item_parse(yyjson_arr_get(tracks, i), ITEM_TRACK,
                                   &info->popular[info->popular_count])) {
                ++info->popular_count;
            }
        }

        info->popular_loaded = true;
    }

    yyjson_doc_free(doc);
    return valid;
}

void collection_release_label(const CollectionInfo *info, char *out, size_t capacity) {
    static const char *months[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    int fields = sscanf(info->item.release_date, "%u-%u-%u", &year, &month, &day);
    out[0] = 0;

    if (fields < 1 || year < 1000 || year > 9999) {
        return;
    }

    if (!strcmp(info->release_precision, "day") && fields == 3 && month >= 1 && month <= 12 && day >= 1 &&
        day <= 31) {
        snprintf(out, capacity, "%s %u, %u", months[month - 1], day, year);
        return;
    }

    if (!strcmp(info->release_precision, "month") && fields >= 2 && month >= 1 && month <= 12) {
        snprintf(out, capacity, "%s %u", months[month - 1], year);
        return;
    }

    snprintf(out, capacity, "%u", year);
}

void collection_duration_label(uint64_t duration_ms, char *out, size_t capacity) {
    uint64_t seconds = duration_ms / 1000;

    if (seconds >= 3600) {
        snprintf(out, capacity, "%llu hr %llu min", (unsigned long long)(seconds / 3600),
                 (unsigned long long)(seconds / 60 % 60));
        return;
    }

    snprintf(out, capacity, "%llu min %llu sec", (unsigned long long)(seconds / 60),
             (unsigned long long)(seconds % 60));
}
