#include "app_internal.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

AppState app_state;

static bool data_file_path(const char *name, char path[PLATFORM_PATH_CAPACITY]) {
    char dir[PLATFORM_PATH_CAPACITY];

    if (!platform_data_dir(dir, sizeof(dir))) {
        return false;
    }

    int length = snprintf(path, PLATFORM_PATH_CAPACITY, "%s/%s", dir, name);
    return length > 0 && length < PLATFORM_PATH_CAPACITY;
}

static void load_settings(void) {
    app_state.view.settings = (AppSettings){.search_radio = true};
    char path[PLATFORM_PATH_CAPACITY];
    char text[4096];

    if (!data_file_path("settings.json", path) || !platform_read_file(path, text, sizeof(text))) {
        return;
    }

    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *search_radio = yyjson_obj_get(root, "search_radio");

    if (yyjson_is_bool(search_radio)) {
        app_state.view.settings.search_radio = yyjson_get_bool(search_radio);
    }

    yyjson_doc_free(doc);
}

static void save_settings_locked(void) {
    char path[PLATFORM_PATH_CAPACITY];
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (root) {
        yyjson_mut_obj_add_bool(doc, root, "search_radio", app_state.view.settings.search_radio);
    }

    char *text = yyjson_mut_write(doc, 0, NULL);

    if (text && data_file_path("settings.json", path)) {
        platform_write_file(path, text);
    }

    free(text);
    yyjson_mut_doc_free(doc);
}

static uint64_t next_usage_stamp_locked(void) {
    time_t now = time(NULL);
    uint64_t stamp = now > 0 ? (uint64_t)now * 1000u : 1u;

    if (stamp <= app_state.playlist_usage_clock) {
        stamp = ++app_state.playlist_usage_clock;
    } else {
        app_state.playlist_usage_clock = stamp;
    }

    return stamp;
}

static uint64_t playlist_usage_stamp_locked(const char *id) {
    if (!id || !id[0]) {
        return 0;
    }

    for (unsigned i = 0; i < app_state.playlist_usage_count; ++i) {
        if (!strcmp(app_state.playlist_usage[i].id, id)) {
            return app_state.playlist_usage[i].last_played;
        }
    }

    return 0;
}

void app_sort_playlists_locked(CatalogItem *items, unsigned count) {
    if (!items || count < 2) {
        return;
    }

    for (unsigned i = 1; i < count; ++i) {
        CatalogItem key = items[i];
        uint64_t stamp = key.kind == ITEM_PLAYLIST ? playlist_usage_stamp_locked(key.id) : 0;
        unsigned j = i;

        while (j > 0) {
            uint64_t prev =
                items[j - 1].kind == ITEM_PLAYLIST ? playlist_usage_stamp_locked(items[j - 1].id) : 0;

            if (prev >= stamp) {
                break;
            }

            items[j] = items[j - 1];
            --j;
        }

        items[j] = key;
    }
}

static void save_playlist_usage_locked(void) {
    char path[PLATFORM_PATH_CAPACITY];
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *rows = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, rows);

    for (unsigned i = 0; rows && i < app_state.playlist_usage_count; ++i) {
        yyjson_mut_val *entry = yyjson_mut_arr(doc);

        if (entry) {
            yyjson_mut_arr_add_str(doc, entry, app_state.playlist_usage[i].id);
            yyjson_mut_arr_add_uint(doc, entry, app_state.playlist_usage[i].last_played);
            yyjson_mut_arr_append(rows, entry);
        }
    }

    char *text = yyjson_mut_write(doc, 0, NULL);

    if (text && data_file_path("playlist-usage.json", path)) {
        platform_write_file(path, text);
    }

    free(text);
    yyjson_mut_doc_free(doc);
}

static void load_playlist_usage(void) {
    char path[PLATFORM_PATH_CAPACITY];

    if (!data_file_path("playlist-usage.json", path) || !platform_file_exists(path)) {
        return;
    }

    char *text = malloc(64 * 1024);

    if (!text) {
        return;
    }

    if (platform_read_file(path, text, 64 * 1024)) {
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        yyjson_val *rows = yyjson_doc_get_root(doc);
        size_t total = yyjson_arr_size(rows);

        if (total > 1024) {
            total = 1024;
        }

        for (size_t i = 0; i < total; ++i) {
            yyjson_val *row = yyjson_arr_get(rows, i);
            const char *id = yyjson_get_str(yyjson_arr_get(row, 0));
            yyjson_val *stamp_val = yyjson_arr_get(row, 1);
            uint64_t stamp = yyjson_is_uint(stamp_val) ? yyjson_get_uint(stamp_val) : 0;

            if (!spotify_id_valid(id) || !stamp) {
                continue;
            }

            if (app_state.playlist_usage_count == app_state.playlist_usage_capacity) {
                unsigned cap = app_state.playlist_usage_capacity ? app_state.playlist_usage_capacity * 2 : 64;
                PlaylistUsage *grown = realloc(app_state.playlist_usage, cap * sizeof(*grown));

                if (!grown) {
                    break;
                }

                app_state.playlist_usage = grown;
                app_state.playlist_usage_capacity = cap;
            }

            bool duplicate = false;

            for (unsigned j = 0; j < app_state.playlist_usage_count; ++j) {
                if (!strcmp(app_state.playlist_usage[j].id, id)) {
                    if (stamp > app_state.playlist_usage[j].last_played) {
                        app_state.playlist_usage[j].last_played = stamp;
                    }

                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                PlaylistUsage *entry = &app_state.playlist_usage[app_state.playlist_usage_count++];
                snprintf(entry->id, sizeof(entry->id), "%s", id);
                entry->last_played = stamp;
            }

            if (stamp > app_state.playlist_usage_clock) {
                app_state.playlist_usage_clock = stamp;
            }
        }

        yyjson_doc_free(doc);
    }

    free(text);
}

void app_record_playlist_used_locked(const char *id) {
    if (!spotify_id_valid(id)) {
        return;
    }

    uint64_t stamp = next_usage_stamp_locked();
    unsigned index = app_state.playlist_usage_count;

    for (unsigned i = 0; i < app_state.playlist_usage_count; ++i) {
        if (!strcmp(app_state.playlist_usage[i].id, id)) {
            index = i;
            break;
        }
    }

    if (index == app_state.playlist_usage_count) {
        if (app_state.playlist_usage_count >= 1024) {
            index = app_state.playlist_usage_count - 1;

            for (unsigned i = 1; i < app_state.playlist_usage_count; ++i) {
                if (app_state.playlist_usage[i].last_played < app_state.playlist_usage[index].last_played) {
                    index = i;
                }
            }
        } else {
            if (app_state.playlist_usage_count == app_state.playlist_usage_capacity) {
                unsigned cap = app_state.playlist_usage_capacity ? app_state.playlist_usage_capacity * 2 : 64;
                PlaylistUsage *grown = realloc(app_state.playlist_usage, cap * sizeof(*grown));

                if (!grown) {
                    return;
                }

                app_state.playlist_usage = grown;
                app_state.playlist_usage_capacity = cap;
            }

            index = app_state.playlist_usage_count++;
        }
    }

    snprintf(app_state.playlist_usage[index].id, sizeof(app_state.playlist_usage[index].id), "%s", id);
    app_state.playlist_usage[index].last_played = stamp;
    snprintf(app_state.last_played_playlist_id, sizeof(app_state.last_played_playlist_id), "%s", id);
    save_playlist_usage_locked();

    if (app_state.playlists && app_state.view.playlists.count > 0) {
        app_sort_playlists_locked(app_state.playlists, app_state.view.playlists.count);
        ++app_state.view.playlists.revision;
    }

    if (app_state.collection && app_state.view.list.request.kind == BROWSE_PLAYLISTS &&
        app_state.view.list.count > 0) {
        app_sort_playlists_locked(app_state.collection, app_state.view.list.count);
        ++app_state.view.list.revision;
    }
}

bool app_set_search_radio(bool enabled) {
    platform_lock();
    bool changed = app_state.view.settings.search_radio != enabled;
    app_state.view.settings.search_radio = enabled;
    save_settings_locked();
    platform_unlock();

    if (changed) {
        app_wake();
    }

    return true;
}

static bool valid_saved_media_uri(const char *uri) {
    if (!strncmp(uri, "spotify:track:", 14) && spotify_id_valid(uri + 14)) {
        return true;
    }

    return !strncmp(uri, "spotify:episode:", 16) && spotify_id_valid(uri + 16);
}

void app_sync_startup_intro_locked(void) {
    if (app_state.startup_home_done && app_state.startup_library_done) {
        app_state.view.loading_intro = false;
    }
}

void app_save_playback_locked(bool force) {
    uint64_t now = platform_milliseconds();

    if (!force && app_state.playback_saved_ms && now - app_state.playback_saved_ms < 5000) {
        return;
    }

    const Playback *pb = &app_state.view.playback;

    if (!pb->uri[0] && !app_state.view.queue.count && !app_state.up_next_count && pb->volume_percent < 0 &&
        !pb->shuffle && pb->repeat == REPEAT_OFF) {
        return;
    }

    char path[PLATFORM_PATH_CAPACITY];

    if (!data_file_path("session-playback.json", path)) {
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (!root) {
        yyjson_mut_doc_free(doc);
        return;
    }

    uint64_t elapsed =
        now >= app_state.view.playback_received_ms ? now - app_state.view.playback_received_ms : 0;
    unsigned pos = player_position(pb, elapsed);

    yyjson_mut_obj_add_str(doc, root, "uri", pb->uri);
    yyjson_mut_obj_add_str(doc, root, "context_uri", pb->context_uri);
    yyjson_mut_obj_add_str(doc, root, "title", pb->title);
    yyjson_mut_obj_add_str(doc, root, "artist", pb->artist);
    yyjson_mut_obj_add_str(doc, root, "artist_id", pb->artist_id);
    yyjson_mut_obj_add_str(doc, root, "album", pb->album);
    yyjson_mut_obj_add_str(doc, root, "album_id", pb->album_id);
    yyjson_mut_obj_add_str(doc, root, "artwork", pb->artwork);
    yyjson_mut_obj_add_uint(doc, root, "position_ms", pos);
    yyjson_mut_obj_add_uint(doc, root, "duration_ms", pb->duration_ms);
    yyjson_mut_obj_add_bool(doc, root, "shuffle", pb->shuffle);
    yyjson_mut_obj_add_uint(doc, root, "repeat", (unsigned)pb->repeat);
    yyjson_mut_obj_add_sint(doc, root, "volume_percent", pb->volume_percent);

    if (spotify_id_valid(app_state.search_radio_seed.id)) {
        yyjson_mut_val *seed = yyjson_mut_obj(doc);
        if (seed) {
            yyjson_mut_obj_add_str(doc, seed, "id", app_state.search_radio_seed.id);
            yyjson_mut_obj_add_str(doc, seed, "title", app_state.search_radio_seed.title);
            yyjson_mut_obj_add_str(doc, seed, "detail", app_state.search_radio_seed.detail);
            yyjson_mut_obj_add_str(doc, seed, "artwork", app_state.search_radio_seed.artwork);
            yyjson_mut_obj_add_val(doc, root, "search_radio_seed", seed);
        }
    }

    app_up_next_save_locked(doc, root);
    yyjson_mut_val *qarr = yyjson_mut_arr(doc);

    if (qarr) {
        for (unsigned i = 0; i < app_state.view.queue.count && i < QUEUE_LIMIT; ++i) {
            const CatalogItem *item = &app_state.view.queue.items[i];

            if (!spotify_id_valid(item->id)) {
                continue;
            }

            yyjson_mut_val *entry = yyjson_mut_obj(doc);

            if (!entry) {
                continue;
            }

            yyjson_mut_obj_add_uint(doc, entry, "kind", (unsigned)item->kind);
            yyjson_mut_obj_add_str(doc, entry, "id", item->id);
            yyjson_mut_obj_add_str(doc, entry, "title", item->title);
            yyjson_mut_obj_add_str(doc, entry, "detail", item->detail);
            yyjson_mut_obj_add_str(doc, entry, "artist_id", item->artist_id);
            yyjson_mut_obj_add_str(doc, entry, "album_id", item->album_id);
            yyjson_mut_obj_add_str(doc, entry, "album_title", item->album_title);
            yyjson_mut_obj_add_str(doc, entry, "artwork", item->artwork);
            yyjson_mut_obj_add_uint(doc, entry, "duration_ms", item->duration_ms);
            yyjson_mut_obj_add_bool(doc, entry, "duration_known", item->duration_known);
            yyjson_mut_obj_add_bool(doc, entry, "playable", item->playable);
            yyjson_mut_obj_add_bool(doc, entry, "explicit", item->explicit_content);
            yyjson_mut_arr_append(qarr, entry);
        }

        yyjson_mut_obj_add_val(doc, root, "queue", qarr);
    }

    char *text = yyjson_mut_write(doc, 0, NULL);

    if (text && platform_write_file(path, text)) {
        app_state.playback_saved_ms = now;
    }

    free(text);
    yyjson_mut_doc_free(doc);
}

static void clear_saved_playback(void) {
    char path[PLATFORM_PATH_CAPACITY];

    if (data_file_path("session-playback.json", path)) {
        platform_write_file(path, "{}");
    }
}

static void load_saved_playback_locked(void) {
    char path[PLATFORM_PATH_CAPACITY];

    if (!data_file_path("session-playback.json", path) || !platform_file_exists(path)) {
        return;
    }

    char *text = malloc(128 * 1024);

    if (!text) {
        return;
    }

    if (platform_read_file(path, text, 128 * 1024)) {
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);

        if (yyjson_is_obj(root)) {
            app_up_next_restore_locked(root);
            Playback pb;
            playback_reset(&pb);
            const char *uri = yyjson_get_str(yyjson_obj_get(root, "uri"));
            const char *ctx = yyjson_get_str(yyjson_obj_get(root, "context_uri"));
            const char *title = yyjson_get_str(yyjson_obj_get(root, "title"));
            const char *artist = yyjson_get_str(yyjson_obj_get(root, "artist"));
            const char *artist_id = yyjson_get_str(yyjson_obj_get(root, "artist_id"));
            const char *album = yyjson_get_str(yyjson_obj_get(root, "album"));
            const char *album_id = yyjson_get_str(yyjson_obj_get(root, "album_id"));
            const char *artwork = yyjson_get_str(yyjson_obj_get(root, "artwork"));
            yyjson_val *pos_v = yyjson_obj_get(root, "position_ms");
            yyjson_val *dur_v = yyjson_obj_get(root, "duration_ms");
            yyjson_val *rep_v = yyjson_obj_get(root, "repeat");
            yyjson_val *vol_v = yyjson_obj_get(root, "volume_percent");

            if (uri && valid_saved_media_uri(uri)) {
                copy_utf8(pb.uri, sizeof(pb.uri), uri);
            }

            if (ctx) {
                copy_utf8(pb.context_uri, sizeof(pb.context_uri), ctx);
            }

            if (pb.uri[0] && title) {
                copy_utf8(pb.title, sizeof(pb.title), title);
            }

            if (pb.uri[0] && artist) {
                copy_utf8(pb.artist, sizeof(pb.artist), artist);
            }

            if (artist_id && spotify_id_valid(artist_id)) {
                copy_utf8(pb.artist_id, sizeof(pb.artist_id), artist_id);
            }

            if (pb.uri[0] && album) {
                copy_utf8(pb.album, sizeof(pb.album), album);
            }

            if (album_id && spotify_id_valid(album_id)) {
                copy_utf8(pb.album_id, sizeof(pb.album_id), album_id);
            }

            if (artwork && artwork_url_valid(artwork)) {
                copy_utf8(pb.artwork, sizeof(pb.artwork), artwork);
            }

            pb.duration_ms = yyjson_is_uint(dur_v) && yyjson_get_uint(dur_v) <= UINT32_MAX
                                 ? (unsigned)yyjson_get_uint(dur_v)
                                 : 0;
            pb.position_ms = yyjson_is_uint(pos_v) && yyjson_get_uint(pos_v) <= UINT32_MAX
                                 ? (unsigned)yyjson_get_uint(pos_v)
                                 : 0;

            if (pb.duration_ms > 0 && pb.position_ms > pb.duration_ms) {
                pb.position_ms = pb.duration_ms;
            }

            pb.position_known = pb.uri[0] && pb.duration_ms > 0;
            pb.shuffle = yyjson_is_true(yyjson_obj_get(root, "shuffle"));

            if (yyjson_is_uint(rep_v) && yyjson_get_uint(rep_v) <= (uint64_t)REPEAT_TRACK) {
                pb.repeat = (RepeatMode)yyjson_get_uint(rep_v);
            }

            if (yyjson_is_int(vol_v) && yyjson_get_sint(vol_v) >= 0 && yyjson_get_sint(vol_v) <= 100) {
                pb.volume_percent = (int)yyjson_get_sint(vol_v);
            }

            Queue restored_q = {0};
            yyjson_val *qarr = yyjson_obj_get(root, "queue");

            for (size_t i = 0; i < yyjson_arr_size(qarr) && restored_q.count < QUEUE_LIMIT; ++i) {
                yyjson_val *entry = yyjson_arr_get(qarr, i);
                const char *id = yyjson_get_str(yyjson_obj_get(entry, "id"));
                const char *qtitle = yyjson_get_str(yyjson_obj_get(entry, "title"));

                if (!spotify_id_valid(id) || !qtitle) {
                    continue;
                }

                CatalogItem *item = &restored_q.items[restored_q.count++];
                *item = (CatalogItem){0};
                yyjson_val *k_v = yyjson_obj_get(entry, "kind");
                item->kind =
                    yyjson_is_uint(k_v) && yyjson_get_uint(k_v) == ITEM_EPISODE ? ITEM_EPISODE : ITEM_TRACK;
                snprintf(item->id, sizeof(item->id), "%s", id);
                copy_utf8(item->title, sizeof(item->title), qtitle);
                const char *qd = yyjson_get_str(yyjson_obj_get(entry, "detail"));
                const char *qaid = yyjson_get_str(yyjson_obj_get(entry, "artist_id"));
                const char *qalbid = yyjson_get_str(yyjson_obj_get(entry, "album_id"));
                const char *qalbt = yyjson_get_str(yyjson_obj_get(entry, "album_title"));
                const char *qart = yyjson_get_str(yyjson_obj_get(entry, "artwork"));
                copy_utf8(item->detail, sizeof(item->detail), qd ? qd : "");
                if (qaid && spotify_id_valid(qaid)) {
                    snprintf(item->artist_id, sizeof(item->artist_id), "%s", qaid);
                }
                if (qalbid && spotify_id_valid(qalbid)) {
                    snprintf(item->album_id, sizeof(item->album_id), "%s", qalbid);
                }
                copy_utf8(item->album_title, sizeof(item->album_title), qalbt ? qalbt : "");
                if (qart && artwork_url_valid(qart)) {
                    copy_utf8(item->artwork, sizeof(item->artwork), qart);
                }
                yyjson_val *qdur = yyjson_obj_get(entry, "duration_ms");
                item->duration_ms = yyjson_is_uint(qdur) && yyjson_get_uint(qdur) <= UINT32_MAX
                                        ? (unsigned)yyjson_get_uint(qdur)
                                        : 0;
                item->duration_known = yyjson_is_true(yyjson_obj_get(entry, "duration_known"));
                item->playable = !yyjson_is_false(yyjson_obj_get(entry, "playable"));
                item->explicit_content = yyjson_is_true(yyjson_obj_get(entry, "explicit"));
            }

            restored_q.received = restored_q.count;

            yyjson_val *seed = yyjson_obj_get(root, "search_radio_seed");
            const char *sid = yyjson_get_str(yyjson_obj_get(seed, "id"));

            if (spotify_id_valid(sid)) {
                app_state.search_radio_seed = (CatalogItem){.kind = ITEM_TRACK, .playable = true};
                copy_utf8(app_state.search_radio_seed.id, sizeof(app_state.search_radio_seed.id), sid);
                const char *st = yyjson_get_str(yyjson_obj_get(seed, "title"));
                const char *sd = yyjson_get_str(yyjson_obj_get(seed, "detail"));
                const char *sa = yyjson_get_str(yyjson_obj_get(seed, "artwork"));
                copy_utf8(app_state.search_radio_seed.title, sizeof(app_state.search_radio_seed.title),
                          st ? st : "");
                copy_utf8(app_state.search_radio_seed.detail, sizeof(app_state.search_radio_seed.detail),
                          sd ? sd : "");
                copy_utf8(app_state.search_radio_seed.artwork, sizeof(app_state.search_radio_seed.artwork),
                          sa ? sa : "");
            }

            bool meaningful = pb.uri[0] || restored_q.count > 0 || pb.shuffle || pb.repeat != REPEAT_OFF ||
                              pb.volume_percent >= 0;

            if (meaningful) {
                app_state.view.playback = pb;
                app_state.view.playback_received_ms = platform_milliseconds();
                ++app_state.view.playback_revision;
                app_state.view.queue = restored_q;
                app_state.restored_pending = true;
                app_state.restored_controls_synced = false;
            }
        }

        yyjson_doc_free(doc);
    }

    free(text);
}

static bool history_path(char path[PLATFORM_PATH_CAPACITY]) {
    return data_file_path("search-history.json", path);
}

static void load_search_history(void) {
    char path[PLATFORM_PATH_CAPACITY];

    if (!history_path(path) || !platform_file_exists(path)) {
        return;
    }

    char *text = malloc(32 * 1024);

    if (!text) {
        return;
    }

    if (platform_read_file(path, text, 32 * 1024)) {
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        yyjson_val *rows = yyjson_doc_get_root(doc);
        View *view = &app_state.view;

        for (size_t i = 0; i < yyjson_arr_size(rows) && view->search_history_count < SEARCH_HISTORY_LIMIT;
             ++i) {
            yyjson_val *row = yyjson_arr_get(rows, i);
            const char *id = yyjson_get_str(yyjson_arr_get(row, 0));
            const char *title = yyjson_get_str(yyjson_arr_get(row, 1));
            const char *artist = yyjson_get_str(yyjson_arr_get(row, 2));
            const char *art = yyjson_get_str(yyjson_arr_get(row, 3));
            bool duplicate = false;

            for (unsigned j = 0; id && j < view->search_history_count; ++j) {
                duplicate = duplicate || !strcmp(id, view->search_history[j].id);
            }

            if (!duplicate && spotify_id_valid(id) && title && artist && art) {
                CatalogItem *item = &view->search_history[view->search_history_count++];
                *item = (CatalogItem){.kind = ITEM_TRACK, .playable = true};
                copy_utf8(item->id, sizeof(item->id), id);
                copy_utf8(item->title, sizeof(item->title), title);
                copy_utf8(item->detail, sizeof(item->detail), artist);
                copy_utf8(item->artwork, sizeof(item->artwork), art);
            }
        }

        yyjson_doc_free(doc);
    }

    free(text);
}

static void save_search_history(void) {
    char path[PLATFORM_PATH_CAPACITY];
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *rows = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, rows);

    for (unsigned i = 0; rows && i < app_state.view.search_history_count; ++i) {
        CatalogItem *item = &app_state.view.search_history[i];
        const char *fields[] = {item->id, item->title, item->detail, item->artwork};
        yyjson_mut_arr_append(rows, yyjson_mut_arr_with_str(doc, fields, 4));
    }

    char *text = yyjson_mut_write(doc, 0, NULL);

    if (text && history_path(path)) {
        platform_write_file(path, text);
    }

    free(text);
    yyjson_mut_doc_free(doc);
}

void app_search_history_add(const CatalogItem *item) {
    if (!item || item->kind != ITEM_TRACK || !spotify_id_valid(item->id)) {
        return;
    }

    platform_lock();
    View *view = &app_state.view;

    for (unsigned i = 0; i < view->search_history_count; ++i) {
        if (!strcmp(item->id, view->search_history[i].id)) {
            platform_unlock();
            return;
        }
    }

    if (view->search_history_count == SEARCH_HISTORY_LIMIT) {
        --view->search_history_count;
    }

    memmove(view->search_history + 1, view->search_history,
            view->search_history_count * sizeof(*view->search_history));
    view->search_history[0] = *item;
    ++view->search_history_count;
    save_search_history();
    platform_unlock();
    app_wake();
}

void app_search_history_remove(const char *id) {
    platform_lock();
    View *view = &app_state.view;

    for (unsigned i = 0; i < view->search_history_count; ++i) {
        if (!strcmp(id, view->search_history[i].id)) {
            --view->search_history_count;
            memmove(view->search_history + i, view->search_history + i + 1,
                    (view->search_history_count - i) * sizeof(*view->search_history));
            save_search_history();
            break;
        }
    }

    platform_unlock();
    app_wake();
}

const CatalogItem *list_row(const ListView *list, unsigned index) {
    if (index < list->first || index - list->first >= list->rows_count) {
        return NULL;
    }

    return &list->rows[index - list->first];
}

void app_set_wake(AppWake wake) {
    platform_lock();
    app_state.wake = wake;
    platform_unlock();
}

void app_wake(void) {
    platform_lock();
    AppWake wake = app_state.wake;
    platform_unlock();

    if (wake) {
        wake();
    }
}

void app_reset_session(void) {
    platform_lock();
    ++app_state.view.session_revision;
    app_state.view.authenticated = false;
    memset(&app_state.browse_cache, 0, sizeof(app_state.browse_cache));
    memset(app_state.detail_cache, 0, sizeof(app_state.detail_cache));
    app_state.view.details = (CollectionInfo){0};
    app_state.view.profile = (ProfileInfo){0};
    app_state.sync = (PlayerSync){0};
    app_state.seek = (PlayerSeek){0};
    app_state.seek_due_ms = 0;
    app_state.seek_sent_ms = 0;
    app_state.has_deferred = false;
    devices_reset(&app_state.view.devices);
    ++app_state.view.devices_revision;
    memset(&app_state.view.queue, 0, sizeof(app_state.view.queue));
    app_state.up_next_count = 0;
    ++app_state.up_next_generation;

    for (unsigned i = 0; i < TRACK_CACHE_SLOTS; ++i) {
        free(app_state.track_cache[i].items);
        app_state.track_cache[i] = (TrackCacheEntry){0};
    }

    app_state.loading_snapshot[0] = 0;
    free(app_state.play_order);
    app_state.play_order = NULL;
    app_state.play_order_capacity = 0;
    app_state.play_order_count = 0;
    app_state.play_order_follow = false;
    app_state.queue_first = 0;
    app_state.handoff_id[0] = 0;
    app_state.handoff_inflight = false;
    memset(app_state.stale_handoffs, 0, sizeof(app_state.stale_handoffs));
    memset(app_state.view.library, 0, sizeof(app_state.view.library));
    memset(app_state.view.library_loaded, 0, sizeof(app_state.view.library_loaded));
    memset(app_state.view.recent, 0, sizeof(app_state.view.recent));
    app_state.view.recent_count = 0;
    app_state.view.recent_loaded = false;
    app_state.view.recent_permission_required = false;
    free(app_state.likes);
    app_state.likes = NULL;
    app_state.likes_capacity = 0;
    app_state.likes_count = 0;
    app_state.likes_offset = 0;
    app_state.likes_ready = false;
    app_state.likes_scanning = false;
    memset(app_state.pending_likes, 0, sizeof(app_state.pending_likes));
    memset(app_state.like_status, 0, sizeof(app_state.like_status));
    app_state.like_status_next = 0;
    app_state.like_status_inflight = false;
    app_state.like_status_retry_ms = 0;
    app_state.view.artist_liked_count = 0;
    app_state.view.artist_likes_ready = false;
    app_state.view.artist_likes_failed = false;
    app_state.queue_visible = false;
    app_state.devices_visible = false;
    app_state.lyrics_visible = false;
    app_state.view.lyrics = (Lyrics){0};
    memset(&app_state.view.list, 0, sizeof(app_state.view.list));
    free(app_state.collection);
    app_state.collection = NULL;
    app_state.collection_capacity = 0;
    app_state.collection_count = 0;
    free(app_state.order);
    app_state.order = NULL;
    app_state.order_capacity = 0;
    free(app_state.playlists);
    app_state.playlists = NULL;
    app_state.playlist_capacity = 0;
    memset(&app_state.view.playlists, 0, sizeof(app_state.view.playlists));
    ++app_state.browse_seq;
    app_state.viewport_first = 0;
    app_state.library_first = 0;
    app_state.view.list.selected = -1;
    app_state.view.engine_device_id[0] = 0;
    app_state.view.remote_selected = false;
    playback_reset(&app_state.view.playback);
    app_state.last_played_playlist_id[0] = 0;
    app_state.search_radio_seed = (CatalogItem){0};
    app_state.radio_guarded_uri[0] = 0;
    app_state.startup_home_done = false;
    app_state.startup_library_done = false;
    app_state.view.loading_intro = false;
    app_state.restored_pending = false;
    app_state.restored_controls_synced = false;
    ++app_state.view.playback_revision;
    platform_unlock();
    platform_trim_memory();
}

void app_initialize(void) {
    load_search_history();
    load_settings();
    load_playlist_usage();
    app_workers_configure();
    snprintf(app_state.view.status, sizeof(app_state.view.status), "Connect to Spotify to get started.");
    app_state.view.playback.volume_percent = -1;
    app_state.view.playback_revision = 1;
    app_state.view.devices_revision = 1;
    app_state.view.list.selected = -1;
    devices_reset(&app_state.view.devices);
    platform_lock();
    load_saved_playback_locked();
    platform_unlock();
}

void app_shutdown(void) {
    platform_lock();
    app_save_playback_locked(true);
    app_state.shutting_down = true;
    platform_unlock();
    engine_stop();
}

/* Applies a command's expected result immediately; the confirming read reconciles it. */
void app_optimistic(const Job *job) {
    uint64_t now = platform_milliseconds();
    Playback *playback = &app_state.view.playback;
    if (!app_state.seek.pending || !app_state.seek.local) {
        playback->position_ms = player_position(playback, now - app_state.view.playback_received_ms);
    }

    switch (job->action) {
    case ACTION_PLAY:
        if (!playback->playing) {
            playback->pausing_disallowed = false;

            if (app_state.view.authenticated && !strncmp(playback->context_uri, "spotify:playlist:", 17) &&
                spotify_id_valid(playback->context_uri + 17)) {
                app_record_playlist_used_locked(playback->context_uri + 17);
            }
        }

        playback->playing = true;
        break;
    case ACTION_PAUSE:
        if (playback->playing) {
            playback->resuming_disallowed = false;
        }

        playback->playing = false;
        break;
    case ACTION_SEEK:
        playback->position_ms = job->value;
        break;
    case ACTION_VOLUME:
        playback->volume_percent = (int)job->value;
        break;
    case ACTION_SHUFFLE:
        playback->shuffle = job->value != 0;
        break;
    case ACTION_REPEAT:
        playback->repeat = (RepeatMode)job->value;
        break;
    case ACTION_NEXT: {
        const CatalogItem *next = !app_state.restored_pending && !app_command_pending() && playback->has_device
                                      ? app_queue_next_locked()
                                      : NULL;

        if (next && (next->kind == ITEM_TRACK || next->kind == ITEM_EPISODE) && spotify_id_valid(next->id)) {
            snprintf(playback->uri, sizeof(playback->uri), "spotify:%s:%s",
                     next->kind == ITEM_EPISODE ? "episode" : "track", next->id);
            copy_utf8(playback->title, sizeof(playback->title), next->title);
            copy_utf8(playback->artist, sizeof(playback->artist), next->detail);
            copy_utf8(playback->artist_id, sizeof(playback->artist_id), next->artist_id);
            copy_utf8(playback->album, sizeof(playback->album), next->album_title);
            copy_utf8(playback->album_id, sizeof(playback->album_id), next->album_id);
            snprintf(playback->artwork, sizeof(playback->artwork), "%s", next->artwork);
            playback->duration_ms = next->duration_known ? next->duration_ms : 0;
            playback->position_known = next->duration_known;
            playback->position_ms = 0;
        }

        break;
    }
    case ACTION_PREVIOUS:
        // The device decides whether Previous restarts or goes back a track.
        break;
    case ACTION_PLAY_ITEM:
        copy_utf8(playback->title, sizeof(playback->title), job->item.title);
        copy_utf8(playback->artist, sizeof(playback->artist), job->item.detail);
        snprintf(playback->artwork, sizeof(playback->artwork), "%s", job->item.artwork);

        if ((job->item.kind == ITEM_TRACK || job->item.kind == ITEM_EPISODE) &&
            spotify_id_valid(job->item.id)) {
            snprintf(playback->uri, sizeof(playback->uri), "spotify:%s:%s",
                     job->item.kind == ITEM_EPISODE ? "episode" : "track", job->item.id);
            copy_utf8(playback->artist_id, sizeof(playback->artist_id), job->item.artist_id);
            copy_utf8(playback->album, sizeof(playback->album), job->item.album_title);
            copy_utf8(playback->album_id, sizeof(playback->album_id), job->item.album_id);
        }

        if (job->item.kind == ITEM_PLAYLIST && spotify_id_valid(job->item.id)) {
            snprintf(playback->context_uri, sizeof(playback->context_uri), "spotify:playlist:%s",
                     job->item.id);
        } else if (job->browse.kind == BROWSE_PLAYLIST_TRACKS && spotify_id_valid(job->browse.id)) {
            snprintf(playback->context_uri, sizeof(playback->context_uri), "spotify:playlist:%s",
                     job->browse.id);
        } else if (job->browse.kind == BROWSE_ALBUM_TRACKS && spotify_id_valid(job->browse.id)) {
            snprintf(playback->context_uri, sizeof(playback->context_uri), "spotify:album:%s",
                     job->browse.id);
        } else {
            playback->context_uri[0] = 0;
        }

        playback->playing = true;
        playback->position_ms = 0;
        playback->duration_ms = job->item.duration_known ? job->item.duration_ms : 0;
        playback->position_known = playback->duration_ms > 0;
        break;
    default:
        return;
    }

    app_state.view.playback_received_ms = now;
    ++app_state.view.playback_revision;
    ++app_state.command_seq;

    if (job->action == ACTION_SEEK) {
        player_seek_requested(&app_state.seek, app_state.command_seq, now);
        app_state.seek.local = !app_state.view.remote_selected &&
                               !app_state.restored_pending && app_state.view.engine_device_id[0] &&
                               !strcmp(playback->device_id, app_state.view.engine_device_id);
        app_state.seek.target_ms = job->value;
        app_state.seek.previous_anchor_ms = playback->anchor_timestamp_ms;
    }

    if (job->action == ACTION_NEXT || job->action == ACTION_PREVIOUS || job->action == ACTION_PLAY_ITEM) {
        app_state.seek.pending = false;
        app_state.seek_due_ms = 0;
        app_state.seek_sent_ms = 0;
        app_state.view.changing_track = true;
        playback->position_ms = 0;

        if (!app_state.view.remote_selected) {
            playback->playing = false;
        }
    }

    if (job->action == ACTION_VOLUME || job->action == ACTION_SHUFFLE || job->action == ACTION_REPEAT ||
        (job->action == ACTION_SEEK && !app_state.seek.local) || job->action == ACTION_PLAY_ITEM) {
        app_save_playback_locked(job->action != ACTION_SEEK);
    }
}

static bool submit_command(Job job) {
    if (app_submit(job)) {
        app_wake();
        return true;
    }

    platform_lock();
    if (job.action != ACTION_SEEK || (app_state.seek.pending && job.seq == app_state.seek.sequence)) {
        app_state.seek.pending = false;
        player_sync_requested(&app_state.sync, platform_milliseconds());
        snprintf(app_state.view.status, sizeof(app_state.view.status),
                 "Could not schedule playback control. Please try again.");
    }
    platform_unlock();
    app_wake();
    return false;
}

static bool command(Job job) {
    platform_lock();
    bool ready = app_state.view.authenticated;

    if (job.action == ACTION_SEEK &&
        (app_state.view.changing_track || strcmp(job.target.uri, app_state.view.playback.uri) ||
         strcmp(job.target.device_id, app_state.view.playback.device_id))) {
        platform_unlock();
        return false;
    }

    if (ready) {
        job.session = app_state.view.session_revision;
        app_optimistic(&job);

        if (job.action == ACTION_SEEK) {
            job.seq = app_state.seek.sequence;
            job.local_seek = app_state.seek.local;

            if (job.local_seek) {
                uint64_t now = platform_milliseconds();
                uint64_t due = player_local_seek_due(app_state.seek_sent_ms, app_state.seek_due_ms != 0, now);

                if (due > now) {
                    app_state.delayed_seek = job;
                    app_state.seek_due_ms = due;
                    platform_unlock();
                    app_wake();
                    return true;
                }
            }

            app_state.seek_due_ms = 0;
        }
    }

    platform_unlock();

    if (!ready) {
        platform_lock();
        snprintf(app_state.view.status, sizeof(app_state.view.status), "Connect your Spotify account first.");
        platform_unlock();
        app_wake();
        return false;
    }

    return submit_command(job);
}

void app_action(Action action) {
    if (action == ACTION_RESTORE && getenv("NATIVE_CONTROLLER_OFFLINE")) {
        return;
    }

    if (action == ACTION_RESTORE || action == ACTION_CONNECT || action == ACTION_DISCONNECT) {
        if (action == ACTION_DISCONNECT) {
            clear_saved_playback();
        }

        app_reset_session();
        platform_lock();
        app_state.view.connecting = action != ACTION_DISCONNECT;
        app_state.view.loading_intro = action == ACTION_RESTORE;

        if (action == ACTION_RESTORE) {
            load_saved_playback_locked();
        }

        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 action == ACTION_CONNECT ? "Finish login in your browser. Waiting up to 120 seconds…" : "");
        platform_unlock();
        app_wake();
        app_submit((Job){.action = action});
        return;
    }

    platform_lock();

    if (action == ACTION_TOGGLE_PLAYBACK) {
        action = app_state.view.playback.playing ? ACTION_PAUSE : ACTION_PLAY;
    }

    Job job = {.action = action, .target = app_state.view.playback, .browse = app_state.view.list.request};

    if (action == ACTION_REFRESH && job.browse.kind == BROWSE_SONG_RADIO) {
        job.item = app_state.radio_seed;
    }

    job.value = action == ACTION_SHUFFLE  ? !app_state.view.playback.shuffle
                : action == ACTION_REPEAT ? ((unsigned)app_state.view.playback.repeat + 1) % 3
                                          : 0;
    bool listed = app_state.view.list.loaded && app_state.view.list.request.kind == BROWSE_QUEUE;
    bool loaded = app_state.view.list.loaded;
    bool needs_setup = action == ACTION_PLAY && !job.target.has_device &&
                       (app_state.view.engine == ENGINE_NEEDS_KEY || app_state.view.engine == ENGINE_FAILED);

    // An on-demand engine that is off starts now; the play waits for it like a deferred song.
    bool start_engine = action == ACTION_PLAY && !job.target.has_device && !needs_setup &&
                        app_state.view.engine == ENGINE_OFF && app_state.view.authenticated;

    if (needs_setup) {
        ++app_state.view.setup_revision;
    }

    if (start_engine) {
        app_state.deferred = job;
        app_state.has_deferred = true;
        snprintf(app_state.view.status, sizeof(app_state.view.status), "Getting playback ready…");
    }

    platform_unlock();

    if (needs_setup) {
        app_wake();
        return;
    }

    if (start_engine) {
        engine_start();
        app_wake();
        return;
    }

    if (app_player_command(action)) {
        command(job);
        return;
    }

    if (action == ACTION_QUEUE) {
        platform_lock();
        ++app_state.browse_seq;
        app_state.view.list = (ListView){.request = {.kind = BROWSE_QUEUE}, .selected = -1};
        app_state.viewport_first = 0;
        app_state.view.list.loading = true;
        platform_unlock();
    }

    if (action == ACTION_REFRESH) {
        app_submit((Job){.action = ACTION_SYNC});
        app_submit((Job){.action = ACTION_DEVICES});
        app_submit((Job){.action = ACTION_LIBRARY, .browse = {.kind = BROWSE_PLAYLISTS}, .value = 1});

        if (job.browse.kind == BROWSE_ARTIST_LIKES) {
            app_scan_likes();
        }

        if (job.browse.kind == BROWSE_ALBUM_TRACKS || job.browse.kind == BROWSE_ARTIST_ALBUMS) {
            app_submit((Job){.action = ACTION_DETAILS, .browse = job.browse});
        }

        if (listed) {
            app_submit((Job){.action = ACTION_QUEUE});
            return;
        }

        if (!loaded) {
            return;
        }
    }

    app_submit(job);
}

bool app_connect(void) {
    app_action(ACTION_CONNECT);
    return true;
}

void app_lyrics_visible(bool visible) {
    platform_lock();
    bool changed = app_state.lyrics_visible != visible;
    app_state.lyrics_visible = visible;

    if (changed && visible) {
        app_state.next_paint_ms = 0;

        if (app_state.view.lyrics.state == LYRICS_ERROR) {
            app_state.view.lyrics = (Lyrics){0};
        }
    }

    platform_unlock();

    if (changed) {
        app_wake();
    }
}

int app_tick(bool visible) {
    return app_tick_with_progress(visible, true);
}

void app_engine_playback_changed(void) {
    platform_lock();
    bool ready = app_state.view.authenticated && app_state.view.engine == ENGINE_READY;
    if (ready) {
        ++app_state.sync.engine_revision;
        app_state.sync.next_ms = platform_milliseconds();
    }
    platform_unlock();

    if (ready) {
        app_wake();
    }
}

int app_tick_with_progress(bool visible, bool publish_progress) {
    platform_lock();

    if (!visible || !app_state.view.authenticated) {
        platform_unlock();
        return -1;
    }

    uint64_t now = platform_milliseconds();
    Job seek_job;
    bool send_seek = app_state.seek_due_ms && now >= app_state.seek_due_ms;
    if (send_seek) {
        seek_job = app_state.delayed_seek;
        app_state.seek_due_ms = 0;
    }
    bool poll = now >= app_state.sync.next_ms;
    bool queue = poll && app_state.view.list.loaded && app_state.view.list.request.kind == BROWSE_QUEUE &&
                 !app_state.view.list.loading;
    bool panel_queue = poll && app_state.queue_visible;
    const Playback *track = &app_state.view.playback;
    Lyrics *lyrics = &app_state.view.lyrics;
    Job lyric_job = {.action = ACTION_LYRICS};
    bool need_lyrics = app_state.lyrics_visible && lyrics_track(track) && !lyrics_match(lyrics, track);
    bool load_lyrics = need_lyrics && now >= app_state.lyrics_next_request_ms;

    if (load_lyrics) {
        lyric_job.session = app_state.view.session_revision;
        lyric_job.target = *track;
        *lyrics = (Lyrics){.state = LYRICS_LOADING};
        copy_utf8(lyrics->uri, sizeof(lyrics->uri), track->uri);
    }

    bool animate = publish_progress && track->playing;
    bool paint = animate && now >= app_state.next_paint_ms;
    bool waiting = app_state.view.engine == ENGINE_STARTING || app_state.view.engine == ENGINE_PAIRING;
    bool watch_devices = waiting || app_state.devices_visible;
    bool devices = watch_devices && now >= app_state.next_devices_ms;

    if (poll) {
        // Hold off further polls until this one completes and reschedules.
        app_state.sync.next_ms = now + 30000;
    }

    if (paint) {
        app_state.next_paint_ms = now + 1000;
    }

    if (animate && app_state.lyrics_visible && track->has_device && track->position_known &&
        lyrics->state == LYRICS_SYNCED && lyrics_match(lyrics, track)) {
        unsigned position = player_position(track, now - app_state.view.playback_received_ms);
        unsigned next = (unsigned)(lyrics_active_line(lyrics, position) + 1);

        if (next < lyrics->count) {
            uint64_t line_deadline = now + lyrics->lines[next].time_ms - position;

            if (line_deadline < app_state.next_paint_ms) {
                app_state.next_paint_ms = line_deadline;
            }
        }
    }

    if (devices) {
        app_state.next_devices_ms = now + (waiting ? 3000 : 5000);
    }

    uint64_t deadline = app_state.sync.next_ms;

    if (app_state.seek_due_ms && app_state.seek_due_ms < deadline) {
        deadline = app_state.seek_due_ms;
    }

    if (need_lyrics && !load_lyrics && app_state.lyrics_next_request_ms < deadline) {
        deadline = app_state.lyrics_next_request_ms;
    }

    if (animate && app_state.next_paint_ms < deadline) {
        deadline = app_state.next_paint_ms;
    }

    if (watch_devices && app_state.next_devices_ms < deadline) {
        deadline = app_state.next_devices_ms;
    }

    platform_unlock();

    if (send_seek) {
        submit_command(seek_job);
    }

    if (poll) {
        app_submit((Job){.action = ACTION_SYNC});
    }

    if (devices) {
        app_submit((Job){.action = ACTION_DEVICES});
    }

    if (queue) {
        app_submit((Job){.action = ACTION_QUEUE});
    }

    if (panel_queue) {
        app_submit((Job){.action = ACTION_QUEUE_PANEL});
    }

    if (load_lyrics && !app_submit(lyric_job)) {
        platform_lock();

        if (lyric_job.session == app_state.view.session_revision && lyrics_match(lyrics, &lyric_job.target)) {
            lyrics->state = LYRICS_ERROR;
        }

        platform_unlock();
        paint = true;
    }

    if (paint) {
        app_wake();
    }

    return deadline > now ? (int)(deadline - now) : 1;
}

bool app_selected_item(CatalogItem *item) {
    platform_lock();
    bool ok = app_state.view.list.selected >= 0 &&
              (unsigned)app_state.view.list.selected < app_state.view.list.count;

    if (ok) {
        *item = *app_list_item((unsigned)app_state.view.list.selected);
    }

    platform_unlock();
    return ok;
}

static bool play_item_with(const CatalogItem *item, const BrowseRequest *browse, bool use_list);

static bool play_item_in_context(const CatalogItem *item, bool use_list) {
    platform_lock();
    BrowseRequest browse = use_list ? app_state.view.list.request : (BrowseRequest){.kind = BROWSE_SEARCH};
    platform_unlock();
    return play_item_with(item, &browse, use_list);
}

/* Plays an upcoming song from the queue view, staying inside the playlist or album it belongs to. */
bool app_play_upcoming(const CatalogItem *item) {
    platform_lock();
    BrowseRequest source = app_state.play_order_source;
    bool context = app_state.play_order_context[0] && app_state.play_order_follow && app_state.play_order_count;
    platform_unlock();

    if (!item_can_play(item)) {
        return false;
    }

    return context ? play_item_with(item, &source, false) : play_item_in_context(item, false);
}

static bool play_item_with(const CatalogItem *item, const BrowseRequest *browse, bool use_list) {
    platform_lock();
    Job job = {.action = ACTION_PLAY_ITEM, .item = *item, .target = app_state.view.playback, .browse = *browse};

    BrowseKind kind = job.browse.kind;
    bool arranged = use_list && (app_state.view.list.sort != LIST_SORT_NONE || app_state.view.list.filter[0]);

    // Liked Songs in their own order play as Spotify's Liked Songs context, so playback covers all of them.
    bool collection = kind == BROWSE_SAVED_TRACKS && !arranged && item->kind == ITEM_TRACK &&
                      app_state.view.profile.loaded && browse_user_id_valid(app_state.view.profile.id);

    if (collection) {
        copy_utf8(job.browse.owner_id, sizeof(job.browse.owner_id), app_state.view.profile.id);
    }

    // Lists without a playable Spotify context, or shown in a different order, play their rows directly.
    if (use_list && item->kind == ITEM_TRACK && !collection &&
        (kind == BROWSE_SONG_RADIO || kind == BROWSE_SAVED_TRACKS || kind == BROWSE_ARTIST_LIKES ||
         (kind == BROWSE_PLAYLIST_TRACKS && arranged))) {
        bool found = false;

        for (unsigned i = 0; i < app_state.view.list.count && job.track_count < SONG_RADIO_LIMIT; ++i) {
            const CatalogItem *track = app_list_item(i);
            found = found || !strcmp(track->id, item->id);

            if (found && item_can_play(track)) {
                memcpy(job.track_ids[job.track_count++], track->id, sizeof(track->id));
            }
        }
    }
    bool local = !app_state.view.remote_selected && app_state.view.engine == ENGINE_READY &&
                 app_state.view.engine_device_id[0];
    bool remote = app_state.view.remote_selected && app_state.view.playback.has_device;
    bool preparing = app_state.view.engine == ENGINE_DOWNLOADING ||
                     app_state.view.engine == ENGINE_STARTING || app_state.view.engine == ENGINE_PAIRING ||
                     (app_state.view.engine == ENGINE_OFF && app_state.view.authenticated);

    if (app_state.view.authenticated && (local || remote || preparing || job.target.has_device)) {
        // Starting something new replaces whatever the listener had queued.
        app_replace_up_next_locked();
        app_explicit_play_locked(job.item.id);
        app_capture_play_order_locked(&job);

        if (job.item.kind == ITEM_PLAYLIST && spotify_id_valid(job.item.id)) {
            app_record_playlist_used_locked(job.item.id);
        } else if (job.browse.kind == BROWSE_PLAYLIST_TRACKS && spotify_id_valid(job.browse.id)) {
            app_record_playlist_used_locked(job.browse.id);
        }

        if (job.browse.kind == BROWSE_SEARCH && job.item.kind == ITEM_TRACK && item_can_play(&job.item)) {
            app_state.search_radio_seed = job.item;
            app_state.radio_guarded_uri[0] = 0;
        } else {
            app_state.search_radio_seed = (CatalogItem){0};
        }
    }

    bool start_engine = app_state.view.engine == ENGINE_OFF && app_state.view.authenticated;

    // Soloist is still getting ready: remember the choice and play it the moment it is.
    if (!local && !remote && preparing) {
        app_state.deferred = job;
        app_state.has_deferred = true;
        app_optimistic(&job);
        app_state.view.playback.playing = false;
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 app_state.view.engine_message[0] ? app_state.view.engine_message
                                                  : "Getting playback ready…");
    }

    platform_unlock();

    if (!local && !remote && preparing) {
        if (start_engine) {
            engine_start();
        }

        app_wake();
        return true;
    }

    if (!local && !remote && !job.target.has_device) {
        platform_lock();
        ++app_state.view.setup_revision;
        platform_unlock();
        app_wake();
        return false;
    }

    return command(job);
}

static bool play_item(const CatalogItem *item) {
    return play_item_in_context(item, true);
}

bool app_play_search_history(const CatalogItem *item) {
    return item_can_play(item) && play_item_in_context(item, false);
}

bool app_open_item(const CatalogItem *item) {
    Job job = {.action = ACTION_BROWSE, .item = *item};

    if (item_browse_request(item, &job.browse)) {
        return app_begin_browse(job);
    }

    return item_can_play(item) && play_item(item);
}

/* Call with the view lock held. The newest intent wins while earlier writes settle. */
static bool visible_song_like(const char *id, bool confirmed) {
    const PendingLike *latest = NULL;

    for (unsigned i = 0; i < LANE_CAPACITY + 1; ++i) {
        const PendingLike *pending = &app_state.pending_likes[i];

        if (pending->ticket && (!latest || pending->ticket > latest->ticket) && !strcmp(pending->id, id)) {
            latest = pending;
        }
    }

    return latest ? latest->liked : confirmed;
}

bool app_toggle_song_like(const CatalogItem *item) {
    if (!item || item->kind != ITEM_TRACK || !spotify_id_valid(item->id)) {
        return false;
    }

    platform_lock();

    if (!app_state.view.authenticated) {
        platform_unlock();
        return false;
    }

    Job job = {.action = ACTION_TOGGLE_LIKE, .item = *item, .session = app_state.view.session_revision};
    job.value = !visible_song_like(item->id, item->liked || app_song_liked(item->id));

    for (unsigned i = 0; i < LANE_CAPACITY + 1; ++i) {
        PendingLike *pending = &app_state.pending_likes[i];

        if (!pending->ticket) {
            job.like_ticket = ++app_state.like_ticket;
            *pending = (PendingLike){.ticket = job.like_ticket, .liked = job.value != 0};
            copy_utf8(pending->id, sizeof(pending->id), item->id);
            break;
        }
    }

    platform_unlock();

    if (job.like_ticket && app_submit(job)) {
        app_wake();
        return true;
    }

    app_finish_song_like(&job, "Could not schedule the like change. Please try again.");
    app_wake();
    return false;
}

bool app_play_collection(void) {
    platform_lock();
    CatalogItem item = app_state.view.details.item;
    BrowseKind kind = app_state.view.list.request.kind;
    bool valid = (kind == BROWSE_ALBUM_TRACKS || kind == BROWSE_ARTIST_ALBUMS) && spotify_id_valid(item.id);

    if (!valid && kind == BROWSE_PLAYLIST_TRACKS && spotify_id_valid(app_state.view.list.request.id)) {
        item = (CatalogItem){.kind = ITEM_PLAYLIST};
        snprintf(item.id, sizeof(item.id), "%s", app_state.view.list.request.id);
        copy_utf8(item.title, sizeof(item.title), app_state.view.list.request.title);
        copy_utf8(item.artwork, sizeof(item.artwork), app_state.view.list.request.artwork);
        valid = true;
    }

    platform_unlock();
    return valid && play_item(&item);
}

bool app_play_popular(unsigned index) {
    platform_lock();
    bool valid = index < app_state.view.details.popular_count;
    CatalogItem item = valid ? app_state.view.details.popular[index] : (CatalogItem){0};
    platform_unlock();
    return valid && item_can_play(&item) && play_item(&item);
}

bool app_toggle_collection_saved(void) {
    platform_lock();
    bool valid =
        app_state.view.authenticated && app_state.view.details.loaded && !app_state.view.details.saving;
    bool authorize = valid && app_state.view.details.permission_required;
    Job job = {.action = ACTION_SAVE_COLLECTION,
               .item = app_state.view.details.item,
               .value = !app_state.view.details.saved};

    if (valid && !authorize) {
        app_state.view.details.saving = true;
        app_state.view.details.saved = job.value != 0;
    }

    platform_unlock();

    if (authorize) {
        return app_connect();
    }

    if (!valid) {
        return false;
    }

    if (app_submit(job)) {
        app_wake();
        return true;
    }

    platform_lock();
    app_state.view.details.saving = false;
    app_state.view.details.saved = job.value == 0;
    platform_unlock();
    return false;
}

bool app_open_link(void) {
    CatalogItem item;
    return app_selected_item(&item) && app_submit((Job){.action = ACTION_OPEN_LINK, .item = item});
}

bool app_open_item_link(const CatalogItem *item) {
    return item && spotify_id_valid(item->id) && app_submit((Job){.action = ACTION_OPEN_LINK, .item = *item});
}

bool app_start_radio(const CatalogItem *item) {
    if (!item || (!spotify_id_valid(item->id) && !item->title[0] && !item->detail[0])) {
        return false;
    }

    platform_lock();
    Job job = {.action = ACTION_RADIO, .item = *item, .target = app_state.view.playback};
    bool local = !app_state.view.remote_selected && app_state.view.engine == ENGINE_READY &&
                 app_state.view.engine_device_id[0];
    bool remote = app_state.view.remote_selected && app_state.view.playback.has_device;
    platform_unlock();

    if (!local && !remote && !job.target.has_device) {
        platform_lock();
        ++app_state.view.setup_revision;
        platform_unlock();
        app_wake();
        return false;
    }

    platform_lock();
    app_replace_up_next_locked();
    platform_unlock();
    return command(job);
}

bool app_play_song_radio(void) {
    platform_lock();
    bool radio = app_state.view.list.request.kind == BROWSE_SONG_RADIO;
    platform_unlock();
    return radio && app_play_list();
}

/* Plays the open track list from its first playable row, in the order it is shown. */
bool app_play_list(void) {
    CatalogItem item = {0};
    platform_lock();

    if (app_list_arrangeable(app_state.view.list.request.kind)) {
        for (unsigned i = 0; i < app_state.view.list.count; ++i) {
            const CatalogItem *row = app_list_item(i);

            if (item_can_play(row)) {
                item = *row;
                break;
            }
        }
    }

    platform_unlock();
    return item_can_play(&item) && play_item(&item);
}

bool app_open_collection_link(void) {
    platform_lock();
    CatalogItem item = app_state.view.details.item;
    bool valid = spotify_id_valid(item.id);
    platform_unlock();
    return valid && app_submit((Job){.action = ACTION_OPEN_LINK, .item = item});
}

bool app_select_device(int index) {
    platform_lock();
    bool ok = devices_select(&app_state.view.devices, index);
    platform_unlock();

    if (ok) {
        app_wake();
    }

    return ok;
}

bool app_transfer(void) {
    platform_lock();
    const Device *device = devices_selected(&app_state.view.devices);
    bool ok = device_can_transfer(device);
    Job job = {.action = ACTION_TRANSFER, .target = app_state.view.playback};

    if (ok) {
        job.device = *device;
    }

    platform_unlock();
    return ok && app_submit(job);
}

/*
 * Moves playback to this app's own Soloist. An on-demand Soloist starts first and the move
 * waits for it like a deferred play; one that needs setup asks for the setup panel.
 */
bool app_transfer_here(void) {
    platform_lock();
    Job job = {.action = ACTION_TRANSFER, .target = app_state.view.playback};
    EngineState engine = app_state.view.engine;
    bool ready = engine == ENGINE_READY && app_state.view.engine_device_id[0];
    bool start = engine == ENGINE_OFF && app_state.view.authenticated;
    bool waiting = !ready && (start || engine == ENGINE_DOWNLOADING || engine == ENGINE_STARTING ||
                              engine == ENGINE_PAIRING);

    if (ready) {
        copy_utf8(job.device.id, sizeof(job.device.id), app_state.view.engine_device_id);
    }

    if (waiting) {
        app_state.deferred = job;
        app_state.has_deferred = true;
        snprintf(app_state.view.status, sizeof(app_state.view.status), "Getting playback ready…");
    }

    if (!ready && !waiting) {
        ++app_state.view.setup_revision;
    }

    platform_unlock();

    if (ready) {
        return app_submit(job);
    }

    if (start) {
        engine_start();
    }

    app_wake();
    return waiting;
}

/* Selects a device and moves playback to it in one step. */
bool app_transfer_to(int index) {
    return app_select_device(index) && app_transfer();
}

bool app_seek(unsigned fraction) {
    platform_lock();
    Playback playback = app_state.view.playback;
    platform_unlock();

    unsigned position;

    if (!player_seek_position(&playback, fraction, &position)) {
        return false;
    }

    return command((Job){.action = ACTION_SEEK, .target = playback, .value = position});
}

bool app_seek_lyric(const char *uri, unsigned position_ms) {
    platform_lock();
    Playback playback = app_state.view.playback;
    platform_unlock();

    if (strcmp(uri, playback.uri) || !playback_can_seek(&playback) || position_ms >= playback.duration_ms) {
        return false;
    }

    return command((Job){.action = ACTION_SEEK, .target = playback, .value = position_ms});
}

bool app_volume(unsigned percent) {
    platform_lock();
    Playback playback = app_state.view.playback;
    platform_unlock();

    if (percent > 100 || !playback_can_volume(&playback)) {
        return false;
    }

    return command((Job){.action = ACTION_VOLUME, .target = playback, .value = percent});
}

bool app_engine_key(const char *text, size_t size) {
    char key[ENGINE_KEY_CAPACITY];
    bool valid = engine_key_normalize(text, size, key, sizeof(key));

    if (!valid) {
        platform_wipe(key, sizeof(key));
        platform_lock();
        snprintf(app_state.view.engine_message, sizeof(app_state.view.engine_message),
                 "The clipboard does not contain a Soloist key. Copy the key, then press Paste key again.");
        platform_unlock();
        app_wake();
        return false;
    }

    bool started = app_workers_save_key(key, strlen(key));
    platform_wipe(key, sizeof(key));
    return started;
}

void app_engine_dashboard(void) {
    platform_open_url(ENGINE_DASHBOARD_URL);
}

/* Stops Soloist but keeps the key; an on-demand engine starts again with the next play. */
void app_engine_stop(void) {
    engine_stop();

    platform_lock();
    app_state.view.engine = ENGINE_OFF;
    app_state.view.engine_message[0] = 0;
    app_state.view.engine_device_id[0] = 0;
    app_state.view.changing_track = false;
    app_state.has_deferred = false;
    platform_unlock();

    app_wake();
}

void app_engine_forget(void) {
    app_workers_forget_key();
}

void app_snapshot_into(View *out) {
    if (!out) {
        return;
    }

    platform_lock();
    *out = app_state.view;
    out->seeking = app_state.seek.pending && app_state.seek.local;
    app_compose_queue_locked(&app_state.view.queue, &out->queue,
                             app_state.queue_first > 8 ? app_state.queue_first - 8 : 0);
    unsigned first = app_state.viewport_first > 16 ? app_state.viewport_first - 16 : 0;

    if (first > out->list.count) {
        first = out->list.count;
    }

    unsigned rows = out->list.count - first < LIST_WINDOW ? out->list.count - first : LIST_WINDOW;

    for (unsigned i = 0; i < rows; ++i) {
        out->list.rows[i] = *app_list_item(first + i);
    }

    out->list.first = first;
    out->list.rows_count = rows;

    for (unsigned i = 0; i < rows; ++i) {
        CatalogItem *item = &out->list.rows[i];
        item->liked = item->kind == ITEM_TRACK &&
                      visible_song_like(item->id, out->list.request.kind == BROWSE_SAVED_TRACKS ||
                                                      app_song_liked(item->id));
    }

    for (unsigned i = 0; i < out->queue.count; ++i) {
        CatalogItem *item = &out->queue.items[i];
        item->liked = item->kind == ITEM_TRACK && visible_song_like(item->id, app_song_liked(item->id));
    }

    for (unsigned i = 0; i < out->details.popular_count; ++i) {
        CatalogItem *item = &out->details.popular[i];
        item->liked = visible_song_like(item->id, app_song_liked(item->id));
    }

    for (unsigned i = 0; i < out->search_history_count; ++i) {
        CatalogItem *item = &out->search_history[i];
        item->liked = visible_song_like(item->id, app_song_liked(item->id));
    }

    out->playback_liked = !strncmp(out->playback.uri, "spotify:track:", 14) &&
                          visible_song_like(out->playback.uri + 14, app_song_liked(out->playback.uri + 14));

    unsigned library_start = app_state.library_first > 16 ? app_state.library_first - 16 : 0;

    if (library_start > app_state.view.playlists.count) {
        library_start = app_state.view.playlists.count;
    }

    unsigned library_rows = app_state.view.playlists.count - library_start;

    if (library_rows > LIST_WINDOW) {
        library_rows = LIST_WINDOW;
    }

    if (library_rows) {
        memcpy(out->playlists.rows, app_state.playlists + library_start,
               library_rows * sizeof(*app_state.playlists));
    }

    out->playlists.first = library_start;
    out->playlists.rows_count = library_rows;
    platform_unlock();

    app_request_like_status(out);

    uint64_t now = platform_milliseconds();
    if (!out->changing_track && !out->seeking) {
        out->playback.position_ms = player_position(
            &out->playback, now >= out->playback_received_ms ? now - out->playback_received_ms : 0);
    }
}

View app_snapshot(void) {
    View snapshot;
    app_snapshot_into(&snapshot);
    return snapshot;
}
