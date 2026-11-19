/*
 * JNI boundary between the Compose UI and the shared C core. Kotlin sends typed commands
 * and reads bounded JSON snapshots; it never sees core structs or pointers.
 */
#include "android_host.h"
#include "app.h"
#include "canvas.h"
#include "canvas_raster.h"
#include "collection.h"
#include "core.h"
#include "lyrics.h"
#include "platform.h"
#include "yyjson.h"

#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static bool started;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;

    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !android_host_load(vm, env)) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}

/* Copies a Java string into a fixed buffer as UTF-8; false if it does not fit. */
static bool text(JNIEnv *env, jstring source, char *out, size_t capacity) {
    out[0] = 0;

    if (!source) {
        return true;
    }

    const char *chars = (*env)->GetStringUTFChars(env, source, NULL);

    if (!chars) {
        return false;
    }

    size_t length = strlen(chars);
    bool fits = length < capacity;

    if (fits) {
        memcpy(out, chars, length + 1);
    }

    (*env)->ReleaseStringUTFChars(env, source, chars);
    return fits;
}

static void wake(void) {
    android_wake();
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_start(JNIEnv *env, jobject self,
                                                                   jstring directory) {
    (void)self;
    char path[PLATFORM_PATH_CAPACITY];

    if (!text(env, directory, path, sizeof(path))) {
        return JNI_FALSE;
    }

    // One core per process: activity recreation must not create a second session.
    pthread_mutex_lock(&start_lock);
    bool first = !started;

    if (first && android_set_data_dir(path) && platform_init()) {
        // Soloist runs in Termux under its own name, installed by the setup screen. It starts with the
        // session, like on the desktop, so it is ready by the time something is played.
        engine_set_options((EngineOptions){.device_name = "Native Spotify Phone",
                                           .hosted = true,
                                           .command = android_engine_command,
                                           .playback = android_engine_playback});
        app_initialize();
        app_set_wake(wake);
        app_action(ACTION_RESTORE);
        started = true;
    }

    bool ok = started;
    pthread_mutex_unlock(&start_lock);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_connect(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    app_action(ACTION_CONNECT);
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_disconnect(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    app_action(ACTION_DISCONNECT);
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_refresh(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    app_action(ACTION_REFRESH);
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_search(JNIEnv *env, jobject self, jstring query,
                                                                    jint filter) {
    (void)self;
    char value[SEARCH_CAPACITY];

    if (filter < SEARCH_ALL || filter > SEARCH_PLAYLISTS || !text(env, query, value, sizeof(value))) {
        return JNI_FALSE;
    }

    return app_search(value, (SearchFilter)filter) ? JNI_TRUE : JNI_FALSE;
}

/* Library destinations only; search and item pages have their own entry points. */
JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_browse(JNIEnv *env, jobject self, jint kind) {
    (void)env;
    (void)self;
    bool allowed = kind == BROWSE_SAVED_TRACKS || kind == BROWSE_SAVED_ALBUMS || kind == BROWSE_PLAYLISTS ||
                   kind == BROWSE_FOLLOWED_ARTISTS || kind == BROWSE_PROFILE_PLAYLISTS;

    return allowed && app_browse((BrowseKind)kind, NULL) ? JNI_TRUE : JNI_FALSE;
}

static void copy_field(yyjson_val *object, const char *key, char *out, size_t capacity) {
    const char *value = yyjson_get_str(yyjson_obj_get(object, key));
    copy_utf8(out, capacity, value ? value : "");
}

/* Rebuilds an item Kotlin received in a snapshot. Ids are validated where they are used. */
static bool item_from_json(JNIEnv *env, jstring source, CatalogItem *item) {
    *item = (CatalogItem){0};

    if (!source) {
        return false;
    }

    const char *chars = (*env)->GetStringUTFChars(env, source, NULL);

    if (!chars) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(chars, strlen(chars), 0);
    (*env)->ReleaseStringUTFChars(env, source, chars);
    yyjson_val *root = yyjson_doc_get_root(doc);
    int64_t kind = yyjson_get_sint(yyjson_obj_get(root, "kind"));
    bool ok = yyjson_is_obj(root) && kind >= ITEM_TRACK && kind <= ITEM_ARTIST;

    if (ok) {
        item->kind = (ItemKind)kind;
        copy_field(root, "id", item->id, sizeof(item->id));
        copy_field(root, "title", item->title, sizeof(item->title));
        copy_field(root, "detail", item->detail, sizeof(item->detail));
        copy_field(root, "artwork", item->artwork, sizeof(item->artwork));
        copy_field(root, "artistId", item->artist_id, sizeof(item->artist_id));
        copy_field(root, "albumId", item->album_id, sizeof(item->album_id));
        copy_field(root, "albumTitle", item->album_title, sizeof(item->album_title));
        copy_field(root, "ownerId", item->owner_id, sizeof(item->owner_id));
        copy_field(root, "albumType", item->album_type, sizeof(item->album_type));
        item->playable = yyjson_get_bool(yyjson_obj_get(root, "playable"));
        item->liked = yyjson_get_bool(yyjson_obj_get(root, "liked"));
        item->duration_ms = (unsigned)yyjson_get_uint(yyjson_obj_get(root, "durationMs"));
        item->duration_known = item->duration_ms > 0;
        ok = spotify_id_valid(item->id);
    }

    yyjson_doc_free(doc);
    return ok;
}

/* Opens a collection, artist or profile page. */
JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_open(JNIEnv *env, jobject self, jstring json) {
    (void)self;
    CatalogItem item;
    BrowseRequest request;

    if (!item_from_json(env, json, &item) || !item_browse_request(&item, &request)) {
        return JNI_FALSE;
    }

    return app_open_item(&item) ? JNI_TRUE : JNI_FALSE;
}

/* Plays a song inside the list it was picked from, exactly as a desktop click does. */
JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_play(JNIEnv *env, jobject self, jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_open_item(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_playHistory(JNIEnv *env, jobject self,
                                                                         jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_play_search_history(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_playUpcoming(JNIEnv *env, jobject self,
                                                                          jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_play_upcoming(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_startRadio(JNIEnv *env, jobject self,
                                                                        jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_start_radio(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_enqueue(JNIEnv *env, jobject self,
                                                                     jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_enqueue_item(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_dequeue(JNIEnv *env, jobject self, jstring id) {
    (void)self;
    char value[23];
    return text(env, id, value, sizeof(value)) && app_dequeue(value) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_playQueued(JNIEnv *env, jobject self,
                                                                        jstring id) {
    (void)self;
    char value[23];
    return text(env, id, value, sizeof(value)) && app_play_queued(value) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_clearQueue(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    return app_clear_queue() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_queueViewport(JNIEnv *env, jobject self,
                                                                       jint first) {
    (void)env;
    (void)self;
    app_queue_viewport(first > 0 ? (unsigned)first : 0);
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_queueVisible(JNIEnv *env, jobject self,
                                                                      jboolean visible) {
    (void)env;
    (void)self;
    app_queue_visible(visible);
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_lyricsVisible(JNIEnv *env, jobject self,
                                                                       jboolean visible) {
    (void)env;
    (void)self;
    app_lyrics_visible(visible);
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_playCollection(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    return app_play_collection() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_playList(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    return app_play_list() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_playPopular(JNIEnv *env, jobject self,
                                                                         jint index) {
    (void)env;
    (void)self;
    return index >= 0 && app_play_popular((unsigned)index) ? JNI_TRUE : JNI_FALSE;
}

/* Transport buttons; only the playback actions are accepted here. */
JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_action(JNIEnv *env, jobject self, jint action) {
    (void)env;
    (void)self;
    bool allowed = action == ACTION_PREVIOUS || action == ACTION_PLAY || action == ACTION_PAUSE ||
                   action == ACTION_NEXT || action == ACTION_SHUFFLE || action == ACTION_REPEAT ||
                   action == ACTION_TOGGLE_PLAYBACK;

    if (allowed) {
        app_action((Action)action);
    }
}

/* Seeks to a fraction of the song in thousandths, as the desktop scrubber does. */
JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_seek(JNIEnv *env, jobject self, jint fraction) {
    (void)env;
    (void)self;
    return fraction >= 0 && fraction <= 1000 && app_seek((unsigned)fraction) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_seekLyric(JNIEnv *env, jobject self, jstring uri,
                                                                       jint position) {
    (void)self;
    char value[128];
    return position >= 0 && text(env, uri, value, sizeof(value)) && app_seek_lyric(value, (unsigned)position)
               ? JNI_TRUE
               : JNI_FALSE;
}

/* Runs the core's timers; returns milliseconds until it needs to run again, or -1 for none. */
JNIEXPORT jint JNICALL Java_app_nativespotify_NativeCore_tick(JNIEnv *env, jobject self, jboolean visible) {
    (void)env;
    (void)self;
    return app_tick_with_progress(visible, false);
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_enginePlaybackChanged(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    app_engine_playback_changed();
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_devicesVisible(JNIEnv *env, jobject self,
                                                                        jboolean visible) {
    (void)env;
    (void)self;
    app_devices_visible(visible);
}

/* Moves playback to a Spotify Connect device, named by its id rather than a row position. */
JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_transferTo(JNIEnv *env, jobject self,
                                                                        jstring id) {
    (void)self;
    char value[DEVICE_ID_CAPACITY];
    View *view = malloc(sizeof(*view));
    int index = -1;

    if (!view || !text(env, id, value, sizeof(value)) || !value[0]) {
        free(view);
        return JNI_FALSE;
    }

    app_snapshot_into(view);

    for (unsigned i = 0; i < view->devices.count; ++i) {
        if (!strcmp(view->devices.items[i].id, value)) {
            index = (int)i;
        }
    }

    free(view);
    return index >= 0 && app_transfer_to(index) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_transferHere(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    return app_transfer_here() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_volume(JNIEnv *env, jobject self, jint percent) {
    (void)env;
    (void)self;
    return percent >= 0 && percent <= 100 && app_volume((unsigned)percent) ? JNI_TRUE : JNI_FALSE;
}

/* Quit on the phone stops Soloist; the next play starts it again. */
JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_engineStop(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    app_engine_stop();
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_engineExited(JNIEnv *env, jobject self, jint id,
                                                                      jint code) {
    (void)env;
    (void)self;
    android_engine_exited(id, code);
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_toggleLike(JNIEnv *env, jobject self,
                                                                        jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_toggle_song_like(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_artistLikes(JNIEnv *env, jobject self,
                                                                         jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_browse_artist_likes(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_songRadio(JNIEnv *env, jobject self,
                                                                       jstring json) {
    (void)self;
    CatalogItem item;
    return item_from_json(env, json, &item) && app_browse_song_radio(&item) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_historyAdd(JNIEnv *env, jobject self, jstring json) {
    (void)self;
    CatalogItem item;

    if (item_from_json(env, json, &item)) {
        app_search_history_add(&item);
    }
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_historyRemove(JNIEnv *env, jobject self,
                                                                       jstring id) {
    (void)self;
    char value[23];

    if (text(env, id, value, sizeof(value)) && spotify_id_valid(value)) {
        app_search_history_remove(value);
    }
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_toggleSaved(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    return app_toggle_collection_saved() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_discography(JNIEnv *env, jobject self,
                                                                         jstring group) {
    (void)self;
    char value[16];
    return text(env, group, value, sizeof(value)) && app_filter_discography(value[0] ? value : NULL)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_sort(JNIEnv *env, jobject self, jint sort,
                                                                  jboolean descending) {
    (void)env;
    (void)self;

    if (sort < LIST_SORT_NONE || sort > LIST_SORT_DURATION) {
        return JNI_FALSE;
    }

    return app_list_sort((ListSort)sort, descending) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_filter(JNIEnv *env, jobject self,
                                                                    jstring query) {
    (void)self;
    char value[SEARCH_CAPACITY];
    return text(env, query, value, sizeof(value)) && app_list_filter(value) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_loadMore(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    return app_load_more() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_viewport(JNIEnv *env, jobject self, jint first) {
    (void)env;
    (void)self;
    app_viewport(first > 0 ? (unsigned)first : 0);
}

JNIEXPORT void JNICALL Java_app_nativespotify_NativeCore_libraryViewport(JNIEnv *env, jobject self,
                                                                         jint first) {
    (void)env;
    (void)self;
    app_library_viewport(first > 0 ? (unsigned)first : 0);
}

JNIEXPORT jboolean JNICALL Java_app_nativespotify_NativeCore_setSearchRadio(JNIEnv *env, jobject self,
                                                                            jboolean enabled) {
    (void)env;
    (void)self;
    return app_set_search_radio(enabled) ? JNI_TRUE : JNI_FALSE;
}

static void add_item(yyjson_mut_doc *doc, yyjson_mut_val *array, const CatalogItem *item) {
    yyjson_mut_val *object = yyjson_mut_arr_add_obj(doc, array);
    yyjson_mut_obj_add_int(doc, object, "kind", item->kind);
    yyjson_mut_obj_add_str(doc, object, "id", item->id);
    yyjson_mut_obj_add_str(doc, object, "title", item->title);
    yyjson_mut_obj_add_str(doc, object, "detail", item->detail);
    yyjson_mut_obj_add_str(doc, object, "guests", item->guest_artists);
    yyjson_mut_obj_add_str(doc, object, "artwork", item->artwork);
    yyjson_mut_obj_add_bool(doc, object, "playable", item_can_play(item));
    yyjson_mut_obj_add_uint(doc, object, "durationMs", item->duration_known ? item->duration_ms : 0);
    yyjson_mut_obj_add_bool(doc, object, "explicit", item->explicit_content);
    yyjson_mut_obj_add_bool(doc, object, "liked", item->liked);
    yyjson_mut_obj_add_uint(doc, object, "trackNumber", item->track_number);
    yyjson_mut_obj_add_uint(doc, object, "discNumber", item->disc_number);
    yyjson_mut_obj_add_str(doc, object, "artistId", item->artist_id);
    yyjson_mut_obj_add_str(doc, object, "albumId", item->album_id);
    yyjson_mut_obj_add_str(doc, object, "albumTitle", item->album_title);
    yyjson_mut_obj_add_str(doc, object, "releaseDate", item->release_date);
    yyjson_mut_obj_add_str(doc, object, "albumType", item->album_type);
    yyjson_mut_obj_add_str(doc, object, "ownerId", item->owner_id);
}

static void add_items(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key, const CatalogItem *items,
                      unsigned count) {
    yyjson_mut_val *array = yyjson_mut_obj_add_arr(doc, object, key);

    for (unsigned i = 0; i < count; ++i) {
        add_item(doc, array, &items[i]);
    }
}

static void add_list(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key, const ListView *list) {
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, key);
    yyjson_mut_obj_add_int(doc, object, "kind", list->request.kind);
    yyjson_mut_obj_add_int(doc, object, "searchFilter", list->request.filter);
    yyjson_mut_obj_add_str(doc, object, "query", list->request.query);
    yyjson_mut_obj_add_str(doc, object, "id", list->request.id);
    yyjson_mut_obj_add_str(doc, object, "title", list->request.title);
    yyjson_mut_obj_add_str(doc, object, "artwork", list->request.artwork);
    yyjson_mut_obj_add_uint(doc, object, "first", list->first);
    yyjson_mut_obj_add_uint(doc, object, "count", list->count);
    yyjson_mut_obj_add_uint(doc, object, "total", list->total);
    yyjson_mut_obj_add_uint(doc, object, "durationMs", list->duration_known ? list->duration_ms : 0);
    yyjson_mut_obj_add_int(doc, object, "sort", list->sort);
    yyjson_mut_obj_add_bool(doc, object, "descending", list->sort_descending);
    yyjson_mut_obj_add_str(doc, object, "filter", list->filter);
    yyjson_mut_obj_add_bool(doc, object, "loaded", list->loaded);
    yyjson_mut_obj_add_bool(doc, object, "more", list->more);
    yyjson_mut_obj_add_bool(doc, object, "loading", list->loading);
    yyjson_mut_obj_add_uint(doc, object, "revision", list->revision);
    yyjson_mut_obj_add_bool(doc, object, "arrangeable", app_list_arrangeable(list->request.kind));
    add_items(doc, object, "rows", list->rows, list->rows_count);
}

static void add_page(yyjson_mut_doc *doc, yyjson_mut_val *array, const BrowsePage *page, bool loaded) {
    yyjson_mut_val *object = yyjson_mut_arr_add_obj(doc, array);
    yyjson_mut_obj_add_uint(doc, object, "total", page->total);
    yyjson_mut_obj_add_bool(doc, object, "loaded", loaded);
    add_items(doc, object, "items", page->items, page->count);
}

static void add_details(yyjson_mut_doc *doc, yyjson_mut_val *root, const CollectionInfo *details) {
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, "details");
    yyjson_mut_val *item = yyjson_mut_obj_add_arr(doc, object, "item");
    add_item(doc, item, &details->item);
    yyjson_mut_val *artist = yyjson_mut_obj_add_arr(doc, object, "artist");
    add_item(doc, artist, &details->artist);
    add_items(doc, object, "popular", details->popular, details->popular_count);

    char release[128] = {0};
    char duration[64] = {0};
    collection_release_label(details, release, sizeof(release));
    collection_duration_label(details->duration_ms, duration, sizeof(duration));
    yyjson_mut_obj_add_strcpy(doc, object, "release", release);
    yyjson_mut_obj_add_strcpy(doc, object, "duration", details->duration_ms ? duration : "");
    yyjson_mut_obj_add_str(doc, object, "label", details->label);
    yyjson_mut_obj_add_str(doc, object, "copyrights", details->copyrights);
    yyjson_mut_obj_add_uint(doc, object, "totalTracks", details->total_tracks);
    yyjson_mut_obj_add_uint(doc, object, "followers", details->followers_known ? details->followers : 0);
    yyjson_mut_obj_add_bool(doc, object, "followersKnown", details->followers_known);
    yyjson_mut_obj_add_bool(doc, object, "popularLoaded", details->popular_loaded);
    yyjson_mut_obj_add_bool(doc, object, "loaded", details->loaded);
    yyjson_mut_obj_add_bool(doc, object, "saved", details->saved);
    yyjson_mut_obj_add_bool(doc, object, "savedKnown", details->saved_known);
    yyjson_mut_obj_add_bool(doc, object, "saving", details->saving);
    yyjson_mut_obj_add_bool(doc, object, "permissionRequired", details->permission_required);
}

static void add_profile(yyjson_mut_doc *doc, yyjson_mut_val *root, const ProfileInfo *profile) {
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, "profile");
    yyjson_mut_obj_add_str(doc, object, "id", profile->id);
    yyjson_mut_obj_add_str(doc, object, "name", profile->name);
    yyjson_mut_obj_add_str(doc, object, "artwork", profile->artwork);
    yyjson_mut_obj_add_uint(doc, object, "followers", profile->followers_known ? profile->followers : 0);
    yyjson_mut_obj_add_uint(doc, object, "followingTotal", profile->following_total);
    yyjson_mut_obj_add_bool(doc, object, "followingKnown", profile->following_known);
    yyjson_mut_obj_add_bool(doc, object, "permissionRequired", profile->permission_required);
    yyjson_mut_obj_add_bool(doc, object, "loaded", profile->loaded);
    add_items(doc, object, "following", profile->following, profile->following_count);
}

static void add_playback(yyjson_mut_doc *doc, yyjson_mut_val *root, const View *view) {
    const Playback *playback = &view->playback;
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, "playback");
    yyjson_mut_obj_add_bool(doc, object, "playing", playback->playing);
    yyjson_mut_obj_add_bool(doc, object, "shuffle", playback->shuffle);
    yyjson_mut_obj_add_int(doc, object, "repeat", playback->repeat);
    yyjson_mut_obj_add_str(doc, object, "uri", playback->uri);
    yyjson_mut_obj_add_str(doc, object, "contextUri", playback->context_uri);
    yyjson_mut_obj_add_str(doc, object, "title", playback->title);
    yyjson_mut_obj_add_str(doc, object, "artist", playback->artist);
    yyjson_mut_obj_add_str(doc, object, "artistId", playback->artist_id);
    yyjson_mut_obj_add_str(doc, object, "album", playback->album);
    yyjson_mut_obj_add_str(doc, object, "albumId", playback->album_id);
    yyjson_mut_obj_add_str(doc, object, "artwork", playback->artwork);
    yyjson_mut_obj_add_str(doc, object, "device", playback->device);
    yyjson_mut_obj_add_uint(doc, object, "positionMs", playback->position_ms);
    yyjson_mut_obj_add_uint(doc, object, "durationMs", playback->duration_ms);
    yyjson_mut_obj_add_bool(doc, object, "hasDevice", playback->has_device);
    yyjson_mut_obj_add_bool(doc, object, "canSeek", !view->changing_track && playback_can_seek(playback));
    yyjson_mut_obj_add_bool(doc, object, "changingTrack", view->changing_track);
    yyjson_mut_obj_add_bool(doc, object, "canPrevious", !playback->skipping_previous_disallowed);
    yyjson_mut_obj_add_bool(doc, object, "canNext", !playback->skipping_next_disallowed);
    yyjson_mut_obj_add_bool(doc, object, "liked", view->playback_liked);
    yyjson_mut_obj_add_str(doc, object, "deviceId", playback->device_id);
    yyjson_mut_obj_add_int(doc, object, "volume", playback->volume_percent);
    yyjson_mut_obj_add_bool(doc, object, "canVolume", playback_can_volume(playback));
    yyjson_mut_obj_add_uint(doc, object, "revision", view->playback_revision);
}

static void add_devices(yyjson_mut_doc *doc, yyjson_mut_val *root, const View *view) {
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, "devices");
    yyjson_mut_obj_add_str(doc, object, "engineId", view->engine_device_id);
    yyjson_mut_obj_add_bool(doc, object, "remoteSelected", view->remote_selected);
    yyjson_mut_val *items = yyjson_mut_obj_add_arr(doc, object, "items");

    for (unsigned i = 0; i < view->devices.count; ++i) {
        const Device *device = &view->devices.items[i];
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, items);
        yyjson_mut_obj_add_str(doc, item, "id", device->id);
        yyjson_mut_obj_add_str(doc, item, "name", device->name);
        yyjson_mut_obj_add_str(doc, item, "type", device->type);
        yyjson_mut_obj_add_bool(doc, item, "active", device->active);
        yyjson_mut_obj_add_bool(doc, item, "restricted", device->restricted);
        yyjson_mut_obj_add_bool(doc, item, "canTransfer", device_can_transfer(device));
    }
}

static void add_queue(yyjson_mut_doc *doc, yyjson_mut_val *root, const Queue *queue) {
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, "queue");
    yyjson_mut_obj_add_uint(doc, object, "first", queue->first);
    yyjson_mut_obj_add_uint(doc, object, "total", queue->total);
    yyjson_mut_obj_add_uint(doc, object, "queued", queue->queued);
    yyjson_mut_obj_add_bool(doc, object, "hasCurrent", queue->has_current);

    if (queue->has_current) {
        add_item(doc, yyjson_mut_obj_add_arr(doc, object, "current"), &queue->current);
    }

    add_items(doc, object, "items", queue->items, queue->count);
}

static void add_lyrics(yyjson_mut_doc *doc, yyjson_mut_val *root, const View *view) {
    const Lyrics *lyrics = &view->lyrics;
    yyjson_mut_val *object = yyjson_mut_obj_add_obj(doc, root, "lyrics");
    bool current = lyrics_match(lyrics, &view->playback);
    yyjson_mut_obj_add_int(doc, object, "state", current ? lyrics->state : LYRICS_LOADING);
    yyjson_mut_obj_add_str(doc, object, "uri", lyrics->uri);

    // The highlighted line is the core's choice for the snapshot's playback position.
    int active = current && lyrics->state == LYRICS_SYNCED
                     ? lyrics_active_line(lyrics, view->playback.position_ms)
                     : -1;
    yyjson_mut_obj_add_int(doc, object, "active", active);

    if (current && lyrics->state == LYRICS_PLAIN) {
        yyjson_mut_obj_add_str(doc, object, "text", lyrics->text);
    }

    yyjson_mut_val *lines = yyjson_mut_obj_add_arr(doc, object, "lines");

    for (unsigned i = 0; current && lyrics->state == LYRICS_SYNCED && i < lyrics->count; ++i) {
        yyjson_mut_val *line = yyjson_mut_arr_add_obj(doc, lines);
        yyjson_mut_obj_add_uint(doc, line, "timeMs", lyrics->lines[i].time_ms);
        yyjson_mut_obj_add_str(doc, line, "text", lyrics->text + lyrics->lines[i].text);
    }
}

/* Builds the UI's read-only view of the core. Sizes are bounded by the core's fixed windows. */
JNIEXPORT jbyteArray JNICALL Java_app_nativespotify_NativeCore_snapshot(JNIEnv *env, jobject self) {
    (void)self;
    View *view = malloc(sizeof(*view));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    if (!view || !doc) {
        free(view);
        yyjson_mut_doc_free(doc);
        return NULL;
    }

    app_snapshot_into(view);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_bool(doc, root, "authenticated", view->authenticated);
    yyjson_mut_obj_add_bool(doc, root, "connecting", view->connecting);
    yyjson_mut_obj_add_bool(doc, root, "loadingIntro", view->loading_intro);
    yyjson_mut_obj_add_str(doc, root, "status", view->status);
    yyjson_mut_obj_add_uint(doc, root, "session", view->session_revision);
    yyjson_mut_obj_add_bool(doc, root, "searchRadio", view->settings.search_radio);
    add_profile(doc, root, &view->profile);

    yyjson_mut_val *library = yyjson_mut_obj_add_arr(doc, root, "library");

    for (unsigned i = 0; i < 3; ++i) {
        add_page(doc, library, &view->library[i], view->library_loaded[i]);
    }

    add_items(doc, root, "recent", view->recent, view->recent_count);
    yyjson_mut_obj_add_bool(doc, root, "recentLoaded", view->recent_loaded);
    yyjson_mut_obj_add_bool(doc, root, "recentPermissionRequired", view->recent_permission_required);
    add_list(doc, root, "list", &view->list);
    add_list(doc, root, "playlists", &view->playlists);
    add_details(doc, root, &view->details);
    add_items(doc, root, "searchArtists", view->search_artists, view->search_artist_count);
    add_items(doc, root, "searchAlbums", view->search_albums, view->search_album_count);
    add_items(doc, root, "searchPlaylists", view->search_playlists, view->search_playlist_count);
    add_items(doc, root, "history", view->search_history, view->search_history_count);
    yyjson_mut_obj_add_uint(doc, root, "artistLikedCount", view->artist_liked_count);
    yyjson_mut_obj_add_bool(doc, root, "artistLikesReady", view->artist_likes_ready);
    yyjson_mut_obj_add_bool(doc, root, "artistLikesFailed", view->artist_likes_failed);
    add_playback(doc, root, view);
    add_queue(doc, root, &view->queue);
    add_devices(doc, root, view);
    add_lyrics(doc, root, view);
    yyjson_mut_obj_add_int(doc, root, "engine", view->engine);
    yyjson_mut_obj_add_str(doc, root, "engineMessage", view->engine_message);
    yyjson_mut_obj_add_uint(doc, root, "setupRevision", view->setup_revision);

    // The core chooses the search top result the same way on desktop.
    if (view->list.request.kind == BROWSE_SEARCH && view->list.loaded) {
        const CatalogItem *top =
            browse_search_top(view->list.request.query, view->list.rows, view->list.rows_count,
                              view->search_artists, view->search_artist_count, view->search_albums,
                              view->search_album_count, view->search_playlists, view->search_playlist_count);

        if (top) {
            add_item(doc, yyjson_mut_obj_add_arr(doc, root, "searchTop"), top);
        }
    }

    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    jbyteArray result = json ? (*env)->NewByteArray(env, (jsize)length) : NULL;

    if (result) {
        (*env)->SetByteArrayRegion(env, result, 0, (jsize)length, (const jbyte *)json);
    }

    free(json);
    yyjson_mut_doc_free(doc);
    free(view);
    return result;
}

JNIEXPORT jintArray JNICALL Java_app_nativespotify_NativeBrand_pixels(JNIEnv *jni, jobject self, jint size) {
    (void)self;

    // Bound the request so a bad caller cannot ask for an enormous allocation.
    if (size <= 0 || size > 1024) {
        return NULL;
    }

    size_t count = (size_t)size * (size_t)size;
    uint32_t *pixels = malloc(count * sizeof(*pixels));

    if (!pixels) {
        return NULL;
    }

    canvas_brand_icon_pixels((unsigned)size, pixels);
    jintArray result = (*jni)->NewIntArray(jni, (jsize)count);

    if (result) {
        (*jni)->SetIntArrayRegion(jni, result, 0, (jsize)count, (const jint *)pixels);
    }

    free(pixels);
    return result;
}

static void icon_box(void *context, Rect rect, float radius, Color value) {
    canvas_raster_round_rect(context, rect, radius, value, value);
}

static void icon_path(void *context, const Point *points, unsigned count, bool closed, bool fill, float width,
                      Color value) {
    canvas_raster_path(context, points, count, closed, fill, width, value);
}

static void icon_ellipse(void *context, Rect rect, bool fill, float width, Color value) {
    canvas_raster_ellipse(context, rect, fill, width, value);
}

/*
 * Draws one desktop icon with the shared rasterizer and returns an alpha mask (white with
 * coverage as alpha) so Compose can tint it. `size` is in desktop units and `scale` maps
 * them to pixels, which keeps stroke weights the same as on the desktop.
 */
JNIEXPORT jintArray JNICALL Java_app_nativespotify_NativeIcons_render(JNIEnv *jni, jobject self, jint icon,
                                                                      jfloat size, jfloat scale) {
    (void)self;

    if (icon < 0 || icon > (jint)ICON_MUTE || size <= 0.0f || scale <= 0.0f) {
        return NULL;
    }

    int side = (int)(size * scale + 0.5f);

    if (side <= 0 || side > 1024) {
        return NULL;
    }

    size_t count = (size_t)side * (size_t)side;
    uint32_t *pixels = calloc(count, sizeof(*pixels));

    if (!pixels) {
        return NULL;
    }

    RasterSurface surface = {
        .pixels = pixels, .width = side, .height = side, .clip = {0, 0, side, side}, .scale = scale};
    Canvas canvas = {.context = &surface, .box = icon_box, .path = icon_path, .ellipse = icon_ellipse};
    canvas_icon(&canvas, (Icon)icon, (Rect){0.0f, 0.0f, size, size}, 0xffffff);

    // White on black: the red channel is the coverage.
    for (size_t i = 0; i < count; ++i) {
        uint32_t coverage = (pixels[i] >> 16) & 0xffu;
        pixels[i] = (coverage << 24) | 0xffffffu;
    }

    jintArray result = (*jni)->NewIntArray(jni, (jsize)count);

    if (result) {
        (*jni)->SetIntArrayRegion(jni, result, 0, (jsize)count, (const jint *)pixels);
    }

    free(pixels);
    return result;
}
