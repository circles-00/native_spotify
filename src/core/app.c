#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AppState app_state;

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
    app_state.has_deferred = false;
    devices_reset(&app_state.view.devices);
    ++app_state.view.devices_revision;
    memset(&app_state.view.queue, 0, sizeof(app_state.view.queue));
    memset(app_state.view.library, 0, sizeof(app_state.view.library));
    memset(app_state.view.library_loaded, 0, sizeof(app_state.view.library_loaded));
    memset(app_state.view.recent, 0, sizeof(app_state.view.recent));
    app_state.view.recent_count = 0;
    app_state.view.recent_loaded = false;
    app_state.view.recent_permission_required = false;
    app_state.queue_visible = false;
    memset(&app_state.view.list, 0, sizeof(app_state.view.list));
    memset(&app_state.view.playlists, 0, sizeof(app_state.view.playlists));
    ++app_state.browse_seq;
    app_state.viewport_first = 0;
    app_state.library_first = 0;
    app_state.view.list.selected = -1;
    app_state.view.engine_device_id[0] = 0;
    app_state.view.remote_selected = false;
    playback_reset(&app_state.view.playback);
    ++app_state.view.playback_revision;
    platform_unlock();
}

void app_initialize(void) {
    app_workers_configure();
    snprintf(app_state.view.status, sizeof(app_state.view.status), "Connect to Spotify to get started.");
    app_state.view.playback.volume_percent = -1;
    app_state.view.playback_revision = 1;
    app_state.view.devices_revision = 1;
    app_state.view.list.selected = -1;
    devices_reset(&app_state.view.devices);
}

void app_shutdown(void) {
    platform_lock();
    app_state.shutting_down = true;
    platform_unlock();
    engine_stop();
}

/* Applies a command's expected result immediately; the confirming read reconciles it. */
void app_optimistic(const Job *job) {
    uint64_t now = platform_milliseconds();
    Playback *playback = &app_state.view.playback;
    playback->position_ms = player_position(playback, now - app_state.view.playback_received_ms);

    switch (job->action) {
    case ACTION_PLAY:
        playback->playing = true;
        break;
    case ACTION_PAUSE:
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
    case ACTION_NEXT:
    case ACTION_PREVIOUS:
        playback->position_ms = 0;
        break;
    case ACTION_PLAY_ITEM:
        copy_utf8(playback->title, sizeof(playback->title), job->item.title);
        copy_utf8(playback->artist, sizeof(playback->artist), job->item.detail);
        snprintf(playback->artwork, sizeof(playback->artwork), "%s", job->item.artwork);
        playback->playing = true;
        playback->position_ms = 0;
        playback->duration_ms = 0;
        playback->position_known = false;
        break;
    default:
        return;
    }

    app_state.view.playback_received_ms = now;
    ++app_state.view.playback_revision;
    ++app_state.command_seq;

    if (job->action == ACTION_SEEK) {
        player_seek_requested(&app_state.seek, app_state.command_seq, now);
    }

    if (job->action == ACTION_NEXT || job->action == ACTION_PREVIOUS || job->action == ACTION_PLAY_ITEM) {
        app_state.seek.pending = false;
    }
}

static bool command(Job job) {
    platform_lock();
    bool ready = app_state.view.authenticated;

    if (ready) {
        app_optimistic(&job);
    }

    platform_unlock();

    if (!ready) {
        platform_lock();
        snprintf(app_state.view.status, sizeof(app_state.view.status), "Connect your Spotify account first.");
        platform_unlock();
        app_wake();
        return false;
    }

    app_wake();

    if (app_submit(job)) {
        return true;
    }

    platform_lock();
    app_state.seek.pending = false;
    player_sync_requested(&app_state.sync, platform_milliseconds());
    snprintf(app_state.view.status, sizeof(app_state.view.status),
             "Could not schedule playback control. Please try again.");
    platform_unlock();
    app_wake();
    return false;
}

void app_action(Action action) {
    if (action == ACTION_RESTORE && getenv("NATIVE_CONTROLLER_OFFLINE")) {
        return;
    }

    if (action == ACTION_RESTORE || action == ACTION_CONNECT || action == ACTION_DISCONNECT) {
        app_reset_session();
        platform_lock();
        app_state.view.connecting = action != ACTION_DISCONNECT;
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 action == ACTION_CONNECT ? "Finish login in your browser. Waiting up to 120 seconds…" : "");
        platform_unlock();
        app_wake();
        app_submit((Job){.action = action});
        return;
    }

    platform_lock();
    Job job = {.action = action, .target = app_state.view.playback, .browse = app_state.view.list.request};
    job.value = action == ACTION_SHUFFLE  ? !app_state.view.playback.shuffle
                : action == ACTION_REPEAT ? ((unsigned)app_state.view.playback.repeat + 1) % 3
                                          : 0;
    bool listed = app_state.view.list.loaded && app_state.view.list.request.kind == BROWSE_QUEUE;
    bool loaded = app_state.view.list.loaded;
    bool needs_setup = action == ACTION_PLAY && !job.target.has_device &&
                       (app_state.view.engine == ENGINE_NEEDS_KEY || app_state.view.engine == ENGINE_FAILED);

    if (needs_setup) {
        ++app_state.view.setup_revision;
    }

    platform_unlock();

    if (needs_setup) {
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

int app_tick(bool visible) {
    platform_lock();

    if (!visible || !app_state.view.authenticated) {
        platform_unlock();
        return -1;
    }

    uint64_t now = platform_milliseconds();
    bool poll = now >= app_state.sync.next_ms;
    bool queue = poll && app_state.view.list.loaded && app_state.view.list.request.kind == BROWSE_QUEUE &&
                 !app_state.view.list.loading;
    bool panel_queue = poll && app_state.queue_visible;
    bool paint = app_state.view.playback.playing && now >= app_state.next_paint_ms;
    bool waiting = app_state.view.engine == ENGINE_STARTING || app_state.view.engine == ENGINE_PAIRING;
    bool devices = waiting && now >= app_state.next_devices_ms;

    if (poll) {
        // Hold off further polls until this one completes and reschedules.
        app_state.sync.next_ms = now + 30000;
    }

    if (paint) {
        app_state.next_paint_ms = now + 1000;
    }

    if (devices) {
        app_state.next_devices_ms = now + 3000;
    }

    uint64_t deadline = app_state.sync.next_ms;

    if (app_state.view.playback.playing && app_state.next_paint_ms < deadline) {
        deadline = app_state.next_paint_ms;
    }

    if (waiting && app_state.next_devices_ms < deadline) {
        deadline = app_state.next_devices_ms;
    }

    platform_unlock();

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
        *item = app_state.collection[app_state.view.list.selected];
    }

    platform_unlock();
    return ok;
}

static bool play_item(const CatalogItem *item) {
    platform_lock();
    Job job = {.action = ACTION_PLAY_ITEM,
               .item = *item,
               .target = app_state.view.playback,
               .browse = app_state.view.list.request};
    bool local = !app_state.view.remote_selected && app_state.view.engine == ENGINE_READY &&
                 app_state.view.engine_device_id[0];
    bool remote = app_state.view.remote_selected && app_state.view.playback.has_device;
    bool preparing = app_state.view.engine == ENGINE_DOWNLOADING ||
                     app_state.view.engine == ENGINE_STARTING || app_state.view.engine == ENGINE_PAIRING ||
                     (app_state.view.engine == ENGINE_OFF && app_state.view.authenticated);

    // Soloist is still getting ready: remember the choice and play it the moment it is.
    if (!local && !remote && preparing) {
        app_state.deferred = job;
        app_state.has_deferred = true;
        app_optimistic(&job);
        app_state.view.playback.playing = false;
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 app_state.view.engine_message[0] ? app_state.view.engine_message
                                                  : "Getting playback ready on this computer…");
    }

    platform_unlock();

    if (!local && !remote && preparing) {
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

bool app_open_item(const CatalogItem *item) {
    Job job = {.action = ACTION_BROWSE, .item = *item};

    if (item_browse_request(item, &job.browse)) {
        return app_begin_browse(job);
    }

    return item_can_play(item) && play_item(item);
}

bool app_play_collection(void) {
    platform_lock();
    CatalogItem item = app_state.view.details.item;
    bool valid = (app_state.view.list.request.kind == BROWSE_ALBUM_TRACKS ||
                  app_state.view.list.request.kind == BROWSE_ARTIST_ALBUMS) &&
                 spotify_id_valid(item.id);
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

bool app_enqueue_item(const CatalogItem *item) {
    if (!item || !item_can_play(item)) {
        return false;
    }

    platform_lock();
    Job job = {.action = ACTION_ENQUEUE, .item = *item, .target = app_state.view.playback};
    platform_unlock();
    return command(job);
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

    return command(job);
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

void app_engine_forget(void) {
    app_workers_forget_key();
}

View app_snapshot(void) {
    platform_lock();
    View snapshot = app_state.view;
    unsigned first = app_state.viewport_first > 16 ? app_state.viewport_first - 16 : 0;

    if (first > snapshot.list.count) {
        first = snapshot.list.count;
    }

    unsigned rows = snapshot.list.count - first < LIST_WINDOW ? snapshot.list.count - first : LIST_WINDOW;

    if (rows) {
        memcpy(snapshot.list.rows, app_state.collection + first, rows * sizeof(*app_state.collection));
    }

    snapshot.list.first = first;
    snapshot.list.rows_count = rows;

    unsigned library_start = app_state.library_first > 16 ? app_state.library_first - 16 : 0;

    if (library_start > app_state.view.playlists.count) {
        library_start = app_state.view.playlists.count;
    }

    unsigned library_rows = app_state.view.playlists.count - library_start;

    if (library_rows > LIST_WINDOW) {
        library_rows = LIST_WINDOW;
    }

    if (library_rows) {
        memcpy(snapshot.playlists.rows, app_state.playlists + library_start,
               library_rows * sizeof(*app_state.playlists));
    }

    snapshot.playlists.first = library_start;
    snapshot.playlists.rows_count = library_rows;
    platform_unlock();

    uint64_t now = platform_milliseconds();
    snapshot.playback.position_ms = player_position(
        &snapshot.playback, now >= snapshot.playback_received_ms ? now - snapshot.playback_received_ms : 0);

    return snapshot;
}
