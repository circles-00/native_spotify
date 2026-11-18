#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Lane app_lane_for(Action action) {
    switch (action) {
    case ACTION_PREVIOUS:
    case ACTION_PLAY:
    case ACTION_PAUSE:
    case ACTION_NEXT:
    case ACTION_PLAY_ITEM:
    case ACTION_TRANSFER:
    case ACTION_SEEK:
    case ACTION_VOLUME:
    case ACTION_SHUFFLE:
    case ACTION_REPEAT:
    case ACTION_ENQUEUE:
    case ACTION_RADIO:
        return LANE_PLAYER;
    case ACTION_DEVICES:
    case ACTION_SYNC:
        return LANE_STATE;
    default:
        return LANE_CONTENT;
    }
}

bool app_player_command(Action action) {
    return app_lane_for(action) == LANE_PLAYER && action != ACTION_SYNC && action != ACTION_DEVICES;
}

/* A newer request of the same kind makes a queued one pointless (drags, polls, list loads). */
static bool supersedes(Action queued, Action incoming) {
    if (incoming == ACTION_BROWSE) {
        return queued == ACTION_BROWSE || queued == ACTION_MORE || queued == ACTION_QUEUE;
    }

    return queued == incoming &&
           (incoming == ACTION_HOME || incoming == ACTION_QUEUE_PANEL || incoming == ACTION_SEEK ||
            incoming == ACTION_VOLUME || incoming == ACTION_SYNC || incoming == ACTION_DEVICES ||
            incoming == ACTION_MORE || incoming == ACTION_QUEUE);
}

static void run_job(Worker *worker, Job *job);

static void work(void *argument) {
    Worker *worker = argument;

    for (;;) {
        platform_lock();

        if (!worker->count || app_state.shutting_down) {
            worker->running = false;
            platform_unlock();
            return;
        }

        Job job = worker->jobs[worker->head];
        worker->head = (worker->head + 1) % LANE_CAPACITY;
        --worker->count;
        worker->active = true;
        worker->action = job.action;
        platform_unlock();

        run_job(worker, &job);

        platform_lock();
        worker->active = false;
        platform_unlock();
        app_wake();
    }
}

bool app_submit(Job job) {
    Worker *worker = &app_state.workers[app_lane_for(job.action)];

    platform_lock();

    if (app_state.shutting_down) {
        platform_unlock();
        return false;
    }

    job.session = app_state.view.session_revision;
    job.seq = app_state.command_seq;
    job.browse_seq = app_state.browse_seq;

    for (unsigned i = 0; i < worker->count; ++i) {
        Job *queued = &worker->jobs[(worker->head + i) % LANE_CAPACITY];

        if (supersedes(queued->action, job.action)) {
            *queued = job;
            platform_unlock();
            return true;
        }
    }

    if (worker->count == LANE_CAPACITY) {
        platform_unlock();
        return false;
    }

    worker->jobs[(worker->head + worker->count) % LANE_CAPACITY] = job;
    ++worker->count;
    bool start = !worker->running;
    worker->running = true;
    platform_unlock();

    if (start && !platform_start_thread(work, worker)) {
        platform_lock();
        worker->running = false;
        worker->count = 0;
        platform_unlock();
        return false;
    }

    return true;
}

bool app_command_pending(void) {
    Worker *worker = &app_state.workers[LANE_PLAYER];

    if (worker->active && app_player_command(worker->action)) {
        return true;
    }

    for (unsigned i = 0; i < worker->count; ++i) {
        if (app_player_command(worker->jobs[(worker->head + i) % LANE_CAPACITY].action)) {
            return true;
        }
    }

    return false;
}

/* Lanes copy the shared tokens; refresh is single-flight because it happens under the token lock. */
static void lane_begin(Spotify *spotify) {
    platform_token_lock();
    spotify->tokens = app_state.tokens;

    if (spotify->tokens.refresh[0] && spotify->tokens.expires_at <= platform_seconds() + 60) {
        spotify_ensure_token(spotify);
        app_state.tokens = spotify->tokens;
    }

    platform_token_unlock();
}

static void lane_end(Spotify *spotify) {
    platform_token_lock();

    // A 401 can refresh mid-request; a rejected refresh token wipes the lane copy (session over).
    bool refreshed = spotify->tokens.refresh[0] && spotify->tokens.expires_at > app_state.tokens.expires_at;
    bool revoked = !spotify->tokens.refresh[0] && app_state.tokens.refresh[0] && spotify->last_status == 400;

    if (refreshed) {
        app_state.tokens = spotify->tokens;
    }

    if (revoked) {
        platform_wipe(&app_state.tokens, sizeof(app_state.tokens));
    }

    platform_token_unlock();
    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));

    if (revoked) {
        platform_lock();
        app_state.view.authenticated = false;
        snprintf(app_state.view.status, sizeof(app_state.view.status),
                 "Your Spotify session expired. Log in again.");
        platform_unlock();
    }
}

void app_replace_tokens(const Tokens *replacement) {
    platform_token_lock();
    app_state.tokens = *replacement;
    platform_token_unlock();
}

bool app_signed_in(void) {
    platform_token_lock();
    bool ok = app_state.tokens.refresh[0] != 0;
    platform_token_unlock();
    return ok;
}

void app_set_status(unsigned session, const char *message) {
    platform_lock();

    if (session == app_state.view.session_revision) {
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", message);
    }

    platform_unlock();
}

static void session_started(Spotify *spotify, unsigned session) {
    app_replace_tokens(&spotify->tokens);

    platform_lock();
    bool current = session == app_state.view.session_revision;

    if (current) {
        app_state.view.authenticated = true;
        app_state.view.connecting = false;
        app_state.view.status[0] = 0;
    }

    platform_unlock();
    app_wake();

    if (!current) {
        return;
    }

    app_submit((Job){.action = ACTION_SYNC});
    app_submit((Job){.action = ACTION_DEVICES});
    app_submit((Job){.action = ACTION_HOME});
    engine_start();
    app_load_library(spotify, session);
}

static void run_session(Worker *worker, Job *job) {
    Spotify *spotify = &worker->spotify;
    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));

    if (job->action == ACTION_DISCONNECT) {
        engine_forget();
        spotify_disconnect(spotify);

        Tokens empty = {0};
        app_replace_tokens(&empty);
        app_set_status(job->session, spotify->message);
        return;
    }

    bool ok = job->action == ACTION_RESTORE ? spotify_restore(spotify) : spotify_login(spotify);

    if (ok) {
        session_started(spotify, job->session);
    }

    if (!ok) {
        platform_lock();

        if (job->session == app_state.view.session_revision) {
            app_state.view.connecting = false;

            // A missing stored session on startup is not an error worth showing.
            if (job->action == ACTION_CONNECT) {
                snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", spotify->message);
            }
        }

        platform_unlock();
    }

    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));
}

static Playback engine_target(void) {
    Playback target;
    playback_reset(&target);
    target.has_device = true;
    snprintf(target.device_id, sizeof(target.device_id), "%s", app_state.view.engine_device_id);
    return target;
}

static bool engine_ready(void) {
    return app_state.view.engine == ENGINE_READY && app_state.view.engine_device_id[0];
}

static void run_sync(Spotify *spotify, const Job *job) {
    Playback playback;
    bool ok = spotify_playback(spotify, &playback);
    uint64_t now = platform_milliseconds();
    bool check_radio = false;

    platform_lock();

    // A command issued after this read started makes the read stale; keep the optimistic state.
    bool fresh = job->session == app_state.view.session_revision && job->seq == app_state.command_seq &&
                 !app_command_pending();

    if (ok && fresh &&
        player_seek_reconcile(&app_state.seek, &app_state.view.playback, app_state.view.playback_received_ms,
                              &playback, now)) {
        app_state.view.playback = playback;
        app_state.view.playback_received_ms = now;
        ++app_state.view.playback_revision;
        check_radio = playback.playing && playback.repeat == REPEAT_OFF && playback.duration_ms > 0 &&
                      playback.position_known && playback.duration_ms - playback.position_ms < 20000 &&
                      !strncmp(playback.uri, "spotify:track:", 14) &&
                      strcmp(playback.uri, app_state.radio_guarded_uri) != 0;
    }

    player_sync_received(&app_state.sync, now, ok, app_state.view.playback.playing, spotify->retry_at * 1000);

    if (ok && (!fresh || app_state.seek.pending)) {
        app_state.sync.next_ms = now + 300;
    }

    platform_unlock();

    if (!check_radio) {
        return;
    }

    Queue upcoming = {0};

    if (spotify_queue(spotify, &upcoming) && upcoming.count == 0) {
        CatalogItem seed = {.kind = ITEM_TRACK, .playable = true};
        copy_utf8(seed.id, sizeof(seed.id), playback.uri + 14);
        copy_utf8(seed.title, sizeof(seed.title), playback.title);
        copy_utf8(seed.detail, sizeof(seed.detail), playback.artist);

        if (spotify_play_radio(spotify, &playback, &seed, false)) {
            platform_lock();
            copy_utf8(app_state.radio_guarded_uri, sizeof(app_state.radio_guarded_uri), playback.uri);
            platform_unlock();
        }
    }
}

static void run_devices(Spotify *spotify, const Job *job) {
    Devices devices;
    devices_reset(&devices);

    platform_lock();
    Devices previous = app_state.view.devices;
    platform_unlock();

    devices.selected = previous.selected;
    bool ok = spotify_devices(spotify, &devices);
    int engine = ok ? engine_find_device(&devices) : -1;

    platform_lock();

    if (ok && job->session == app_state.view.session_revision) {
        app_state.view.devices = devices;
        ++app_state.view.devices_revision;
        snprintf(app_state.view.engine_device_id, sizeof(app_state.view.engine_device_id), "%s",
                 engine >= 0 ? devices.items[engine].id : "");
    }

    platform_unlock();

    if (ok) {
        engine_device_seen(engine >= 0);
    }
}

void app_run_player(Spotify *spotify, const Job *job) {
    if (job->action == ACTION_SYNC) {
        run_sync(spotify, job);
        return;
    }

    if (job->action == ACTION_DEVICES) {
        run_devices(spotify, job);
        return;
    }

    platform_lock();
    Playback target = job->target;
    bool local = !app_state.view.remote_selected && engine_ready();

    // New playback goes to this computer unless the listener chose a remote device.
    if ((job->action == ACTION_PLAY_ITEM || job->action == ACTION_RADIO) && local) {
        target = engine_target();
    }

    Device engine = {.supports_volume = true};
    snprintf(engine.id, sizeof(engine.id), "%s", app_state.view.engine_device_id);
    platform_unlock();

    bool accepted = false;

    switch (job->action) {
    case ACTION_PREVIOUS:
        accepted = spotify_command(spotify, &target, "previous");
        break;
    case ACTION_PLAY:
        accepted = target.has_device ? spotify_command(spotify, &target, "play")
                   : local           ? spotify_transfer(spotify, &engine, true)
                                     : false;

        if (!target.has_device && !local) {
            snprintf(spotify->message, sizeof(spotify->message), "Choose a track to start playing.");
        }
        break;
    case ACTION_PAUSE:
        accepted = spotify_command(spotify, &target, "pause");
        break;
    case ACTION_NEXT:
        accepted = spotify_command(spotify, &target, "next");
        break;
    case ACTION_PLAY_ITEM:
        accepted = spotify_play_item(spotify, &target, &job->item, &job->browse);
        break;
    case ACTION_TRANSFER:
        accepted = spotify_transfer(spotify, &job->device, job->target.playing);

        if (accepted) {
            platform_lock();
            app_state.view.remote_selected = strcmp(job->device.id, app_state.view.engine_device_id) != 0;
            platform_unlock();
        }
        break;
    case ACTION_SEEK:
        accepted = spotify_seek(spotify, &target, job->value);
        break;
    case ACTION_VOLUME:
        accepted = spotify_volume(spotify, &target, job->value);
        break;
    case ACTION_SHUFFLE:
    case ACTION_REPEAT:
        accepted = spotify_options(spotify, &target, job->action == ACTION_SHUFFLE, job->value);
        break;
    case ACTION_ENQUEUE:
        accepted = spotify_enqueue(spotify, &target, &job->item);
        break;
    case ACTION_RADIO:
        accepted = spotify_play_radio(spotify, &target, &job->item, true);
        break;
    default:
        break;
    }

    platform_lock();

    // Confirm with Spotify either way: success settles the state, failure reverts the optimism.
    player_sync_requested(&app_state.sync, platform_milliseconds());

    if (job->action == ACTION_SEEK) {
        player_seek_completed(&app_state.seek, job->seq, platform_milliseconds(), accepted);
    }

    if (job->session == app_state.view.session_revision) {
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 accepted ? "" : spotify->message);
    }

    platform_unlock();
}

static void run_job(Worker *worker, Job *job) {
    Spotify *spotify = &worker->spotify;
    spotify->last_status = 0;

    if (job->action == ACTION_RESTORE || job->action == ACTION_CONNECT || job->action == ACTION_DISCONNECT) {
        run_session(worker, job);
        return;
    }

    platform_lock();
    bool current = job->session == app_state.view.session_revision &&
                   ((job->action != ACTION_DETAILS && job->action != ACTION_SAVE_COLLECTION) ||
                    job->browse_seq == app_state.browse_seq);
    platform_unlock();

    if (!current || !app_signed_in()) {
        platform_lock();

        if (current && app_lane_for(job->action) == LANE_CONTENT) {
            app_state.view.list.loading = false;
            snprintf(app_state.view.status, sizeof(app_state.view.status),
                     "Connect your Spotify account first.");
        }

        platform_unlock();
        return;
    }

    lane_begin(spotify);

    if (app_lane_for(job->action) != LANE_CONTENT) {
        app_run_player(spotify, job);
    }

    if (app_lane_for(job->action) == LANE_CONTENT) {
        app_run_content(spotify, job);
    }

    lane_end(spotify);
}

static void login_progress(const char *message) {
    platform_lock();
    snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", message);
    platform_unlock();
    app_wake();
}

static void engine_changed(EngineState state, const char *message) {
    platform_lock();
    app_state.view.engine = state;
    snprintf(app_state.view.engine_message, sizeof(app_state.view.engine_message), "%s", message);

    if (state == ENGINE_STARTING || state == ENGINE_PAIRING) {
        app_state.next_devices_ms = 0;
    }

    bool resume = state == ENGINE_READY && app_state.has_deferred;
    Job job = app_state.deferred;

    if (resume) {
        app_state.has_deferred = false;
        app_state.view.status[0] = 0;
    }

    // Playback was waiting on Soloist but it needs the listener: show the setup panel.
    if ((state == ENGINE_FAILED || state == ENGINE_NEEDS_KEY) && app_state.has_deferred) {
        app_state.has_deferred = false;
        app_state.view.status[0] = 0;
        ++app_state.view.setup_revision;
    }

    platform_unlock();

    if (resume) {
        app_submit(job);
    }

    app_wake();
}

void app_workers_configure(void) {
    for (unsigned i = 0; i < LANE_COUNT; ++i) {
        snprintf(app_state.workers[i].spotify.client_id, sizeof(app_state.workers[i].spotify.client_id), "%s",
                 NATIVE_SPOTIFY_CLIENT_ID);
        app_state.workers[i].spotify.notify = login_progress;
    }

    engine_configure(engine_changed);
}

typedef struct {
    char key[ENGINE_KEY_CAPACITY];
    size_t size;
} PendingKey;

static void save_key(void *argument) {
    PendingKey *pending = argument;
    bool ok = engine_set_key(pending->key, pending->size);

    platform_wipe(pending, sizeof(*pending));
    free(pending);

    if (!ok) {
        platform_lock();
        snprintf(app_state.view.engine_message, sizeof(app_state.view.engine_message),
                 "Could not save the key in your system credential store. Unlock it and try again.");
        app_state.view.engine = ENGINE_FAILED;
        platform_unlock();
        app_wake();
    }
}

bool app_workers_save_key(const char *key, size_t size) {
    PendingKey *pending = calloc(1, sizeof(*pending));

    if (!pending) {
        return false;
    }

    memcpy(pending->key, key, size);
    pending->size = size;

    platform_lock();
    app_state.view.engine = ENGINE_STARTING;
    snprintf(app_state.view.engine_message, sizeof(app_state.view.engine_message), "Saving your key…");
    platform_unlock();
    app_wake();

    // The credential store can block (e.g. an unlock prompt), so save off the UI thread.
    if (!platform_start_thread(save_key, pending)) {
        platform_wipe(pending, sizeof(*pending));
        free(pending);
        return false;
    }

    return true;
}

static void forget_key(void *argument) {
    (void)argument;
    engine_forget();
    engine_start();
}

void app_workers_forget_key(void) {
    platform_start_thread(forget_key, NULL);
}
