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
    case ACTION_SYNC:
        return engine_has_local_playback() ? LANE_SYNC : LANE_STATE;
    case ACTION_DEVICES:
    case ACTION_LIKE_STATUS:
    case ACTION_PLAYBACK_FOLLOWUP:
    case ACTION_SYNC_WEB:
        return LANE_STATE;
    case ACTION_LYRICS:
        return LANE_LYRICS;
    default:
        return LANE_CONTENT;
    }
}

bool app_player_command(Action action) {
    return app_lane_for(action) == LANE_PLAYER;
}

static bool transport(Action action) {
    return action == ACTION_PLAY || action == ACTION_PAUSE;
}

static bool same_transport(const Job *left, const Job *right) {
    return transport(left->action) && transport(right->action) && left->session == right->session &&
           left->target.has_device == right->target.has_device &&
           !strcmp(left->target.device_id, right->target.device_id);
}

static bool queued_action(const Worker *worker, Action action) {
    for (unsigned i = 0; i < worker->count; ++i) {
        if (worker->jobs[(worker->head + i) % worker->capacity].action == action) {
            return true;
        }
    }

    return false;
}

/* A newer request of the same kind makes a queued one pointless (drags, polls, list loads). */
static bool supersedes(Action queued, Action incoming) {
    if (incoming == ACTION_BROWSE) {
        return queued == ACTION_BROWSE || queued == ACTION_MORE || queued == ACTION_QUEUE;
    }

    return queued == incoming &&
           (incoming == ACTION_HOME || incoming == ACTION_QUEUE_PANEL || incoming == ACTION_SEEK ||
            incoming == ACTION_VOLUME || incoming == ACTION_SYNC || incoming == ACTION_DEVICES ||
            incoming == ACTION_MORE || incoming == ACTION_QUEUE || incoming == ACTION_LYRICS ||
            incoming == ACTION_PLAYBACK_FOLLOWUP || incoming == ACTION_SYNC_WEB);
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
        worker->head = (worker->head + 1) % worker->capacity;
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

    if (app_state.shutting_down || (job.session && job.session != app_state.view.session_revision) ||
        (job.action == ACTION_MORE && job.browse_seq != app_state.browse_seq) ||
        (job.local_seek && (!app_state.seek.pending || job.seq != app_state.seek.sequence))) {
        platform_unlock();
        return false;
    }

    job.session = app_state.view.session_revision;
    if (!job.local_seek) {
        job.seq = app_state.command_seq;
    }
    job.browse_seq = app_state.browse_seq;

    if (transport(job.action) && worker->count) {
        Job *last = &worker->jobs[(worker->head + worker->count - 1) % worker->capacity];

        if (same_transport(last, &job)) {
            *last = job;
            platform_unlock();
            return true;
        }
    }

    for (unsigned i = 0; i < worker->count; ++i) {
        Job *queued = &worker->jobs[(worker->head + i) % worker->capacity];

        if (supersedes(queued->action, job.action)) {
            *queued = job;
            platform_unlock();
            return true;
        }
    }

    if (worker->count == worker->capacity) {
        platform_unlock();
        return false;
    }

    worker->jobs[(worker->head + worker->count) % worker->capacity] = job;
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
        if (app_player_command(worker->jobs[(worker->head + i) % worker->capacity].action)) {
            return true;
        }
    }

    return false;
}

/* Lanes copy the shared tokens; refresh is single-flight because it happens under the token lock. */
static void lane_begin(Spotify *spotify) {
    platform_token_lock();
    spotify->tokens = app_state.tokens;

    if (!engine_has_local_playback() && spotify->tokens.refresh[0] &&
        spotify->tokens.expires_at <= platform_seconds() + 60) {
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
        app_state.view.loading_intro = true;
        app_state.startup_home_done = false;
        app_state.startup_library_done = false;
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

    // Desktop keeps Soloist ready from login; on-demand platforms start it with the first play.
    if (!engine_starts_on_play()) {
        engine_start();
    }

    app_load_library(spotify, session);
}

static void run_session(Worker *worker, Job *job) {
    Spotify *spotify = &worker->spotify;
    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));

    platform_lock();
    snprintf(spotify->client_id, sizeof(spotify->client_id), "%s", app_state.view.settings.client_id);
    platform_unlock();

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
            app_state.view.loading_intro = false;

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

/* Continues a searched song with radio picks through the app's queue, so they never linger in Spotify's. */
static bool queue_search_radio(Spotify *spotify, const CatalogItem *seed, unsigned session) {
    CatalogItem *tracks = malloc(BROWSE_PAGE_SIZE * sizeof(*tracks));
    unsigned count = tracks ? spotify_radio_tracks(spotify, seed, tracks, BROWSE_PAGE_SIZE) : 0;

    platform_lock();
    bool current = session == app_state.view.session_revision;

    if (current && count) {
        app_append_up_next_locked(tracks, count);
    }

    if (current) {
        snprintf(app_state.radio_guarded_uri, sizeof(app_state.radio_guarded_uri), "spotify:track:%s", seed->id);
    }

    platform_unlock();
    free(tracks);
    app_wake();
    return current && count;
}

static void apply_restored_controls(Spotify *spotify, const Playback *target) {
    if (!target || !target->has_device || !target->device_id[0]) {
        return;
    }

    if (target->volume_percent >= 0 && target->supports_volume) {
        spotify_volume(spotify, target, (unsigned)target->volume_percent);
    }

    spotify_options(spotify, target, true, target->shuffle ? 1u : 0u);
    spotify_options(spotify, target, false, (unsigned)target->repeat);
}

enum { FOLLOW_TRACK_START = 1, FOLLOW_RADIO = 2 };

static void playback_followup(Spotify *spotify, const Playback *playback, unsigned flags, unsigned session) {
    app_up_next_sync(spotify, playback, flags & FOLLOW_TRACK_START ? "" : playback->uri, session);

    if (!(flags & FOLLOW_RADIO)) {
        return;
    }

    Queue upcoming = {0};

    if (spotify_queue(spotify, &upcoming) && upcoming.count == 0) {
        CatalogItem seed = {.kind = ITEM_TRACK, .playable = true};
        copy_utf8(seed.id, sizeof(seed.id), playback->uri + 14);
        copy_utf8(seed.title, sizeof(seed.title), playback->title);
        copy_utf8(seed.detail, sizeof(seed.detail), playback->artist);

        if (queue_search_radio(spotify, &seed, session)) {
            app_up_next_sync(spotify, playback, playback->uri, session);
        }
    }
}

static void run_sync(Spotify *spotify, const Job *job) {
    Playback playback;
    char device[DEVICE_ID_CAPACITY] = {0};
    platform_lock();
    unsigned engine_revision = app_state.sync.engine_revision;

    if (engine_has_local_playback() && engine_ready() && !app_state.view.remote_selected) {
        copy_utf8(device, sizeof(device), app_state.view.engine_device_id);
    }

    platform_unlock();
    bool from_engine = device[0] && engine_playback(device, &playback);

    if (job->action == ACTION_SYNC_WEB && from_engine) {
        return;
    }

    if (job->action == ACTION_SYNC && engine_has_local_playback() && !from_engine) {
        // Remote discovery must not block the next event from the local socket.
        app_submit((Job){.action = ACTION_SYNC_WEB, .session = job->session});
        return;
    }

    bool ok = from_engine;

    if (!from_engine) {
        lane_begin(spotify);
        ok = spotify_playback(spotify, &playback) && (!device[0] || strcmp(device, playback.device_id));
    }

    uint64_t now = platform_milliseconds();
    bool check_radio = false;
    bool fetch_upcoming = false;
    bool applied = false;

    platform_lock();
    char previous_uri[sizeof(app_state.view.playback.uri)];
    copy_utf8(previous_uri, sizeof(previous_uri), app_state.view.playback.uri);

    // A command issued after this read started makes the read stale; keep the optimistic state.
    bool engine_changed = engine_revision != app_state.sync.engine_revision;
    bool fresh = job->session == app_state.view.session_revision && job->seq == app_state.command_seq &&
                 !app_command_pending() && (!device[0] || !engine_changed);
    bool live_track = ok && (playback.uri[0] != 0 || playback.playing);

    if (ok && fresh) {
        player_sync_reconcile_volume(&app_state.sync, &playback, now);
    }

    bool settling = ok && fresh &&
                    ((from_engine && app_state.view.changing_track && live_track && !playback.position_known &&
                      now < app_state.sync.track_until_ms) ||
                     player_sync_waiting(&app_state.sync, &playback, now));
    SeekResult local_seek = SEEK_READY;

    if (ok && fresh && !settling && app_state.seek.pending && app_state.seek.local) {
        local_seek = player_seek_reconcile_local(&app_state.seek, &app_state.view.playback, &playback, now);

        if (local_seek == SEEK_TIMED_OUT) {
            snprintf(app_state.view.status, sizeof(app_state.view.status), "Soloist did not reach the seek position.");
        }
    }

    if (fresh && !settling && !app_state.has_deferred) {
        app_state.view.changing_track = false;
    }

    if (ok && fresh && app_state.restored_pending && !live_track) {
        if (playback.has_device && playback.device_id[0]) {
            app_state.view.playback.has_device = true;
            snprintf(app_state.view.playback.device_id, sizeof(app_state.view.playback.device_id), "%s",
                     playback.device_id);
            copy_utf8(app_state.view.playback.device, sizeof(app_state.view.playback.device),
                      playback.device);
            app_state.view.playback.restricted = playback.restricted;
            app_state.view.playback.supports_volume = playback.supports_volume;
            ++app_state.view.playback_revision;
        }
    } else if (ok && fresh && !settling && local_seek != SEEK_WAITING &&
               (app_state.seek.local || player_seek_reconcile(&app_state.seek, &app_state.view.playback,
                                                              app_state.view.playback_received_ms, &playback, now))) {
        if (live_track) {
            app_state.restored_pending = false;
        }

        Playback prev = app_state.view.playback;

        // Don't correct the displayed clock for subsecond jitter in a paused sample.
        if (!from_engine && !prev.playing && !playback.playing && prev.position_known && playback.position_known &&
            !strcmp(prev.uri, playback.uri) && !strcmp(prev.device_id, playback.device_id) &&
            llabs((long long)prev.position_ms - playback.position_ms) < 1000) {
            playback.position_ms = prev.position_ms;
        }

        const char *id = strrchr(playback.uri, ':');
        if (id && strcmp(app_state.sync.prefetched_uri, playback.uri)) {
            copy_utf8(app_state.sync.prefetched_uri, sizeof(app_state.sync.prefetched_uri), playback.uri);
            fetch_upcoming = !app_state.view.queue.has_current || strcmp(id + 1, app_state.view.queue.current.id);
        }

        app_state.view.playback = playback;
        app_state.view.playback_received_ms = now;
        ++app_state.view.playback_revision;
        applied = true;

        if (playback.uri[0]) {
            bool changed = strcmp(prev.uri, playback.uri) != 0 || prev.shuffle != playback.shuffle ||
                           prev.repeat != playback.repeat || prev.volume_percent != playback.volume_percent;
            app_save_playback_locked(changed);
        }

        if (playback.playing && !strncmp(playback.context_uri, "spotify:playlist:", 17) &&
            spotify_id_valid(playback.context_uri + 17)) {
            const char *pl_id = playback.context_uri + 17;

            if (strcmp(pl_id, app_state.last_played_playlist_id) != 0) {
                app_record_playlist_used_locked(pl_id);
            }
        } else if (strncmp(playback.context_uri, "spotify:playlist:", 17) != 0) {
            app_state.last_played_playlist_id[0] = 0;
        }

        if (!strncmp(playback.uri, "spotify:track:", 14) && app_state.search_radio_seed.id[0] &&
            strcmp(playback.uri + 14, app_state.search_radio_seed.id) != 0) {
            app_state.search_radio_seed = (CatalogItem){0};
        }

        bool seed_match = spotify_id_valid(app_state.search_radio_seed.id) &&
                          !strncmp(playback.uri, "spotify:track:", 14) &&
                          !strcmp(playback.uri + 14, app_state.search_radio_seed.id);
        bool near_end = playback.duration_ms > 0 && playback.position_known &&
                        (playback.position_ms >= playback.duration_ms ||
                         playback.duration_ms - playback.position_ms < 20000);
        check_radio = app_state.view.settings.search_radio && playback.playing &&
                      playback.repeat == REPEAT_OFF && !strncmp(playback.uri, "spotify:track:", 14) &&
                      strcmp(playback.uri, app_state.radio_guarded_uri) != 0 && (seed_match || near_end) &&
                      !app_state.up_next_count;
    }

    player_sync_received(&app_state.sync, now, ok, app_state.view.playback.playing,
                         from_engine ? 0 : spotify->retry_at * 1000);

    if ((ok || device[0]) && (!fresh || settling || app_state.seek.pending)) {
        app_state.sync.next_ms = now + (from_engine ? 100 : 300);
    }

    if (job->session == app_state.view.session_revision && engine_changed) {
        app_state.sync.next_ms = now;
    }

    platform_unlock();

    if (fetch_upcoming) {
        app_submit((Job){.action = ACTION_QUEUE_PANEL, .session = job->session});
    }

    if (applied) {
        unsigned flags = (strcmp(previous_uri, playback.uri) ? FOLLOW_TRACK_START : 0) |
                         (check_radio ? FOLLOW_RADIO : 0);

        if (from_engine) {
            app_submit((Job){.action = ACTION_PLAYBACK_FOLLOWUP, .target = playback, .value = flags,
                             .session = job->session});
        } else {
            playback_followup(spotify, &playback, flags, job->session);
        }
    }

    if (!from_engine) {
        lane_end(spotify);
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
    bool sync_restored = false;
    Playback restored_target = {0};

    platform_lock();

    if (ok && job->session == app_state.view.session_revision) {
        app_state.view.devices = devices;
        ++app_state.view.devices_revision;
        snprintf(app_state.view.engine_device_id, sizeof(app_state.view.engine_device_id), "%s",
                 engine >= 0 ? devices.items[engine].id : "");

        if (app_state.restored_pending && engine >= 0) {
            const Device *dev = &devices.items[engine];

            if (!app_state.view.playback.has_device || !app_state.view.playback.device_id[0]) {
                app_state.view.playback.has_device = true;
                snprintf(app_state.view.playback.device_id, sizeof(app_state.view.playback.device_id), "%s",
                         dev->id);
                copy_utf8(app_state.view.playback.device, sizeof(app_state.view.playback.device), dev->name);
                app_state.view.playback.supports_volume = dev->supports_volume;
                app_state.view.playback.restricted = dev->restricted;
                ++app_state.view.playback_revision;
            }

            if (!app_state.restored_controls_synced) {
                app_state.restored_controls_synced = true;
                sync_restored = true;
                restored_target = app_state.view.playback;
            }
        }
    }

    platform_unlock();

    if (ok) {
        engine_device_seen(engine >= 0);
    }

    if (sync_restored) {
        apply_restored_controls(spotify, &restored_target);
        app_wake();
    }
}

static EngineCommand local_command(Spotify *spotify, const Playback *target, const char *command,
                                   unsigned value, unsigned session) {
    platform_lock();
    bool local = engine_has_local_playback() && target->has_device &&
                 ((app_state.view.engine_device_id[0] && !strcmp(target->device_id, app_state.view.engine_device_id)) ||
                  !strcmp(target->device, engine_device_name()));
    bool ready = session == app_state.view.session_revision && engine_ready();
    platform_unlock();

    if (!local) {
        return ENGINE_COMMAND_UNAVAILABLE;
    }

    EngineCommand result = ready ? engine_command(command, value) : ENGINE_COMMAND_UNAVAILABLE;

    if (result == ENGINE_COMMAND_UNCONFIRMED) {
        snprintf(spotify->message, sizeof(spotify->message), "Soloist did not confirm the command.");
    } else if (result == ENGINE_COMMAND_UNAVAILABLE) {
        snprintf(spotify->message, sizeof(spotify->message), "Soloist could not run this playback command.");
        return ENGINE_COMMAND_REJECTED;
    }

    return result;
}

bool app_skip_command(Spotify *spotify, const Playback *target, const char *command, unsigned session) {
    EngineCommand result = local_command(spotify, target, command, 0, session);
    // A timed-out skip may already have run; don't send it twice.
    return result == ENGINE_COMMAND_UNAVAILABLE ? spotify_command(spotify, target, command)
                                                : result == ENGINE_COMMAND_ACCEPTED;
}

bool app_enqueue_command(Spotify *spotify, const Playback *target, const CatalogItem *item, unsigned session) {
    if (item->kind == ITEM_TRACK && spotify_id_valid(item->id)) {
        char command[64];
        snprintf(command, sizeof(command), "enqueue:spotify:track:%s", item->id);
        EngineCommand result = local_command(spotify, target, command, 0, session);

        if (result != ENGINE_COMMAND_UNAVAILABLE) {
            return result == ENGINE_COMMAND_ACCEPTED;
        }
    }

    return spotify_enqueue(spotify, target, item);
}

void app_run_player(Spotify *spotify, const Job *job) {
    if (job->action == ACTION_PLAYBACK_FOLLOWUP) {
        platform_lock();
        bool current = job->session == app_state.view.session_revision && !app_state.view.changing_track &&
                       !strcmp(job->target.uri, app_state.view.playback.uri);
        platform_unlock();

        if (current) {
            playback_followup(spotify, &job->target, job->value, job->session);
        }
        return;
    }

    if (job->action == ACTION_DEVICES) {
        run_devices(spotify, job);
        return;
    }

    if (job->action == ACTION_LIKE_STATUS) {
        bool liked[LIKE_CHECK_LIMIT];
        bool ok = spotify_songs_liked(spotify, (const char (*)[23])job->track_ids, job->track_count, liked);
        app_finish_like_status(job, ok ? liked : NULL);
        return;
    }

    platform_lock();
    Playback target = job->target;

    if (job->local_seek) {
        if (!app_state.seek.pending || job->seq != app_state.seek.sequence || app_state.view.changing_track ||
            app_state.view.remote_selected || strcmp(target.uri, app_state.view.playback.uri) ||
            strcmp(target.device_id, app_state.view.playback.device_id)) {
            if (job->seq == app_state.seek.sequence) {
                app_state.seek.pending = false;
            }
            platform_unlock();
            return;
        }

        app_state.seek_sent_ms = platform_milliseconds();
        app_state.seek.dispatched = true;
    }

    bool local = !app_state.view.remote_selected && engine_ready();
    bool soloist = local && engine_has_local_playback();
    bool local_pause = job->action == ACTION_PAUSE && !target.has_device && local;
    bool restored_now = app_state.restored_pending && job->session == app_state.view.session_revision;
    Playback restored_copy = app_state.view.playback;
    Queue restored_queue = app_state.view.queue;

    // New playback goes to this computer unless the listener chose a remote device.
    if (local_pause || (soloist && !target.has_device && job->action == ACTION_PLAY) ||
        ((job->action == ACTION_PLAY_ITEM || job->action == ACTION_RADIO ||
                         (job->action == ACTION_PLAY && restored_now)) &&
                        local)) {
        target = engine_target();
    }

    Device engine = {.supports_volume = true};
    snprintf(engine.id, sizeof(engine.id), "%s", app_state.view.engine_device_id);
    platform_unlock();

    bool accepted = false;

    switch (job->action) {
    case ACTION_PREVIOUS:
        accepted = app_skip_command(spotify, &target, "previous", job->session);
        break;
    case ACTION_PLAY:
        if (restored_now && restored_copy.uri[0] && (target.has_device || local)) {
            Playback play_target = target.has_device ? target : restored_copy;

            if (!play_target.has_device && local) {
                play_target.has_device = true;
                snprintf(play_target.device_id, sizeof(play_target.device_id), "%s", engine.id);
            }

            accepted = spotify_play_restored(spotify, &play_target, &restored_copy, &restored_queue);

            if (accepted) {
                apply_restored_controls(spotify, &restored_copy);
                platform_lock();

                if (job->session == app_state.view.session_revision) {
                    app_state.restored_pending = false;
                }

                platform_unlock();
            }
        } else {
            accepted = target.has_device ? app_skip_command(spotify, &target, "play", job->session)
                       : local           ? spotify_transfer(spotify, &engine, true)
                                         : false;
        }

        if (!target.has_device && !local) {
            snprintf(spotify->message, sizeof(spotify->message), "Choose a track to start playing.");
        }
        break;
    case ACTION_PAUSE:
        if (!target.has_device) {
            accepted = true;
            break;
        }

        if (local_pause && !soloist) {
            Playback current;

            if (spotify_playback(spotify, &current)) {
                accepted = !current.has_device || !current.playing;
            }
        }

        accepted = accepted || app_skip_command(spotify, &target, "pause", job->session);
        break;
    case ACTION_NEXT:
        if (restored_now && restored_queue.count > 0 && (target.has_device || local)) {
            Playback next_pb = restored_copy;
            next_pb.context_uri[0] = 0;
            next_pb.position_ms = 0;
            snprintf(next_pb.uri, sizeof(next_pb.uri), "spotify:%s:%s",
                     restored_queue.items[0].kind == ITEM_EPISODE ? "episode" : "track",
                     restored_queue.items[0].id);
            Queue shifted = restored_queue;

            if (shifted.count > 1) {
                memmove(shifted.items, shifted.items + 1, (shifted.count - 1) * sizeof(shifted.items[0]));
            }

            --shifted.count;
            Playback play_target = target.has_device ? target : restored_copy;

            if (!play_target.has_device && local) {
                play_target.has_device = true;
                snprintf(play_target.device_id, sizeof(play_target.device_id), "%s", engine.id);
            }

            accepted = spotify_play_restored(spotify, &play_target, &next_pb, &shifted);

            if (accepted) {
                apply_restored_controls(spotify, &restored_copy);
                platform_lock();

                if (job->session == app_state.view.session_revision) {
                    app_state.restored_pending = false;
                    app_state.view.queue = shifted;
                }

                platform_unlock();
            }
        } else {
            accepted = app_up_next_skip(spotify, &target, job->session);
        }
        break;
    case ACTION_PLAY_ITEM: {
        platform_lock();
        bool search_radio =
            !soloist && !job->track_count && job->browse.kind == BROWSE_SEARCH && job->item.kind == ITEM_TRACK &&
            spotify_id_valid(job->item.id) && job->session == app_state.view.session_revision &&
            app_state.view.settings.search_radio &&
            (job->target.repeat == REPEAT_OFF || app_state.view.playback.repeat == REPEAT_OFF);
        platform_unlock();

        // Search radio starts as one playback list. Queued songs would outlive it and play before the next
        // playlist, and Spotify offers no way to remove them.
        unsigned radio_count = 0;
        char (*radio_ids)[23] = search_radio ? malloc(SEARCH_RADIO_LIMIT * sizeof(*radio_ids)) : NULL;
        CatalogItem *radio_tracks =
            search_radio ? malloc((SEARCH_RADIO_LIMIT - 1) * sizeof(*radio_tracks)) : NULL;

        if (radio_ids && radio_tracks) {
            memcpy(radio_ids[0], job->item.id, sizeof(radio_ids[0]));
            unsigned found = spotify_radio_tracks(spotify, &job->item, radio_tracks, SEARCH_RADIO_LIMIT - 1);

            for (unsigned i = 0; i < found; ++i) {
                memcpy(radio_ids[i + 1], radio_tracks[i].id, sizeof(radio_ids[0]));
            }

            radio_count = 1 + found;
        }

        if (job->track_count) {
            accepted = spotify_play_tracks(spotify, &target, job->track_ids, job->track_count);
        } else if (radio_count > 1) {
            accepted = spotify_play_tracks(spotify, &target, (const char (*)[23])radio_ids, radio_count);
        } else {
            EngineCommand result = ENGINE_COMMAND_UNAVAILABLE;
            const char *type = job->item.kind == ITEM_TRACK      ? "track"
                               : job->item.kind == ITEM_EPISODE ? "episode"
                               : job->item.kind == ITEM_ALBUM   ? "album"
                               : job->item.kind == ITEM_PLAYLIST ? "playlist"
                                                                : NULL;
            bool offset = (job->item.kind == ITEM_TRACK || job->item.kind == ITEM_EPISODE) &&
                          (job->browse.kind == BROWSE_ALBUM_TRACKS || job->browse.kind == BROWSE_PLAYLIST_TRACKS ||
                           (job->browse.kind == BROWSE_SAVED_TRACKS && job->browse.owner_id[0]));

            if (!offset && type && spotify_id_valid(job->item.id)) {
                char command[64];
                snprintf(command, sizeof(command), "play:spotify:%s:%s", type, job->item.id);
                result = local_command(spotify, &target, command, 0, job->session);
            }

            accepted = result == ENGINE_COMMAND_UNAVAILABLE
                           ? spotify_play_item(spotify, &target, &job->item, &job->browse)
                           : result == ENGINE_COMMAND_ACCEPTED;
        }

        // If Spotify refuses the Liked Songs context, still play the chosen song.
        if (!accepted && job->browse.kind == BROWSE_SAVED_TRACKS && job->browse.owner_id[0] &&
            (spotify->last_status == 400 || spotify->last_status == 404)) {
            accepted = spotify_play_item(spotify, &target, &job->item, &(BrowseRequest){.kind = BROWSE_SEARCH});
        }

        free(radio_ids);
        free(radio_tracks);

        if (accepted && radio_count > 1) {
            platform_lock();

            // The radio is already in place, so playback sync must not queue another one for this song.
            if (job->session == app_state.view.session_revision) {
                snprintf(app_state.radio_guarded_uri, sizeof(app_state.radio_guarded_uri), "spotify:track:%s",
                         job->item.id);
            }

            platform_unlock();
        }

        if (accepted) {
            platform_lock();

            if (job->session == app_state.view.session_revision) {
                app_state.restored_pending = false;
                app_save_playback_locked(true);
            }

            platform_unlock();
        }

        break;
    }
    case ACTION_TRANSFER:
        accepted = spotify_transfer(spotify, &job->device, job->target.playing);

        if (accepted) {
            platform_lock();
            app_state.view.remote_selected = strcmp(job->device.id, app_state.view.engine_device_id) != 0;
            app_state.next_devices_ms = 0;
            platform_unlock();
        }
        break;
    case ACTION_SEEK: {
        if (restored_now) {
            accepted = true;
        } else {
            char command[sizeof(target.uri) + sizeof("seek:")];
            snprintf(command, sizeof(command), "seek:%s", target.uri);
            EngineCommand result = local_command(spotify, &target, command, job->value, job->session);
            if (result == ENGINE_COMMAND_UNAVAILABLE && job->local_seek) {
                snprintf(spotify->message, sizeof(spotify->message), "Soloist is not available for seeking.");
            }
            accepted = result == ENGINE_COMMAND_ACCEPTED ||
                       (result == ENGINE_COMMAND_UNAVAILABLE && !job->local_seek &&
                        spotify_seek(spotify, &target, job->value));
        }
        break;
    }
    case ACTION_VOLUME: {
        EngineCommand result = playback_can_volume(&target)
                                   ? local_command(spotify, &target, "volume", job->value, job->session)
                                   : ENGINE_COMMAND_UNAVAILABLE;
        accepted = result == ENGINE_COMMAND_UNAVAILABLE ? spotify_volume(spotify, &target, job->value)
                                                       : result == ENGINE_COMMAND_ACCEPTED;

        if (!accepted && restored_now) {
            accepted = true;
        }
        break;
    }
    case ACTION_SHUFFLE:
    case ACTION_REPEAT: {
        bool shuffle = job->action == ACTION_SHUFFLE;
        EngineCommand result = ENGINE_COMMAND_UNAVAILABLE;
        if (shuffle) {
            result = local_command(spotify, &target, "shuffle", job->value, job->session);
        } else {
            bool track = job->value == REPEAT_TRACK;
            result = local_command(spotify, &target, track ? "repeat_context" : "repeat_track", 0,
                                   job->session);
            if (result == ENGINE_COMMAND_ACCEPTED) {
                result = local_command(spotify, &target, track ? "repeat_track" : "repeat_context",
                                       job->value != REPEAT_OFF, job->session);
            }
        }

        accepted = result == ENGINE_COMMAND_UNAVAILABLE ? spotify_options(spotify, &target, shuffle, job->value)
                                                       : result == ENGINE_COMMAND_ACCEPTED;

        if (!accepted && restored_now) {
            accepted = true;
        }

        if (job->action == ACTION_REPEAT && accepted && job->value == REPEAT_OFF) {
            platform_lock();
            bool should_queue = job->session == app_state.view.session_revision &&
                                app_state.view.settings.search_radio &&
                                spotify_id_valid(app_state.search_radio_seed.id) &&
                                !strncmp(app_state.view.playback.uri, "spotify:track:", 14) &&
                                !strcmp(app_state.view.playback.uri + 14, app_state.search_radio_seed.id) &&
                                strcmp(app_state.view.playback.uri, app_state.radio_guarded_uri) != 0;
            CatalogItem seed = app_state.search_radio_seed;
            platform_unlock();

            if (should_queue) {
                queue_search_radio(spotify, &seed, job->session);
            }
        }
        break;
    }
    case ACTION_ENQUEUE:
        if (restored_now && spotify_id_valid(job->item.id)) {
            platform_lock();

            if (job->session == app_state.view.session_revision && app_state.view.queue.count < QUEUE_LIMIT) {
                app_state.view.queue.items[app_state.view.queue.count++] = job->item;
                app_state.view.queue.received = app_state.view.queue.count;
                app_save_playback_locked(true);
            }

            platform_unlock();
            accepted = true;
            break;
        }

        accepted = app_enqueue_command(spotify, &target, &job->item, job->session);
        break;
    case ACTION_RADIO:
        accepted = spotify_play_radio(spotify, &target, &job->item, true);

        if (accepted) {
            platform_lock();

            if (job->session == app_state.view.session_revision) {
                app_state.restored_pending = false;
            }

            platform_unlock();
        }
        break;
    default:
        break;
    }

    // Spotify can reject an already-satisfied play/pause request. Verify before hiding the error.
    if (!accepted && transport(job->action) && target.has_device && spotify->last_status == 403) {
        char message[sizeof(spotify->message)];
        copy_utf8(message, sizeof(message), spotify->message);
        Playback current;
        accepted = spotify_playback(spotify, &current) && current.has_device &&
                   !strcmp(current.device_id, target.device_id) &&
                   current.playing == (job->action == ACTION_PLAY);

        if (!accepted) {
            copy_utf8(spotify->message, sizeof(spotify->message), message);
        }
    }

    platform_lock();

    if (job->action == ACTION_SEEK) {
        player_seek_completed(&app_state.seek, job->seq, platform_milliseconds(), accepted);
    }

    Worker *worker = &app_state.workers[LANE_PLAYER];
    bool volume_superseded =
        job->action == ACTION_VOLUME && (app_state.volume_due_ms != 0 || queued_action(worker, ACTION_VOLUME));
    bool superseded = volume_superseded || (worker->count && same_transport(job, &worker->jobs[worker->head]));

    if (job->session == app_state.view.session_revision && !superseded) {
        uint64_t now = platform_milliseconds();

        if (job->action != ACTION_VOLUME || !accepted) {
            player_sync_requested(&app_state.sync, now);
        }

        if (job->action == ACTION_PLAY_ITEM || job->action == ACTION_NEXT || job->action == ACTION_PREVIOUS ||
            job->action == ACTION_TRANSFER || job->action == ACTION_RADIO) {
            app_state.sync.track_until_ms = 0;

            if (accepted && target.has_device && job->action != ACTION_TRANSFER && job->action != ACTION_RADIO) {
                const char *expected = "";
                const char *uri = app_state.view.playback.uri;

                if (job->action == ACTION_PLAY_ITEM && job->seq == app_state.command_seq &&
                    (job->item.kind == ITEM_TRACK || job->item.kind == ITEM_EPISODE) &&
                    strcmp(uri, job->target.uri)) {
                    expected = uri;
                }

                player_sync_expect_track(&app_state.sync, &job->target, target.device_id, expected,
                                         job->action != ACTION_NEXT, now);
            }
        }

        // A successful write can still be followed by a read of the old state.
        if (accepted && target.has_device) {
            if (job->seq == app_state.command_seq && transport(job->action)) {
                player_sync_expect_playing(&app_state.sync, target.device_id, job->action == ACTION_PLAY,
                                           now);
            }

            if (job->action == ACTION_VOLUME && app_state.view.playback.has_device &&
                !strcmp(target.device_id, app_state.view.playback.device_id) &&
                app_state.view.playback.volume_percent == (int)job->value) {
                player_sync_expect_volume(&app_state.sync, target.device_id, (int)job->value, now);
                app_save_playback_locked(true);
            }
        }

        if (!accepted && job->action == ACTION_VOLUME && app_state.sync.volume_until_ms &&
            !strcmp(target.device_id, app_state.sync.volume_device) &&
            app_state.sync.volume_percent == (int)job->value) {
            app_state.sync.volume_until_ms = 0;
        }

        if (!job->local_seek || job->seq == app_state.seek.sequence) {
            snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", accepted ? "" : spotify->message);
        }
    }

    platform_unlock();
}

static void run_lyrics(const Job *job) {
    platform_lock();
    bool current = job->session == app_state.view.session_revision && app_state.lyrics_visible &&
                   lyrics_match(&app_state.view.lyrics, &job->target) &&
                   !strcmp(app_state.view.playback.uri, job->target.uri);
    uint64_t next = app_state.lyrics_next_request_ms;
    bool backed_off = next > platform_milliseconds() + 250;

    if ((!current || backed_off) && job->session == app_state.view.session_revision &&
        lyrics_match(&app_state.view.lyrics, &job->target)) {
        app_state.view.lyrics = (Lyrics){0};
    }

    platform_unlock();

    if (!current || backed_off) {
        return;
    }

    Lyrics lyrics;
    lyrics_fetch(&job->target, &lyrics, &next);
    platform_lock();
    app_state.lyrics_next_request_ms = next;

    if (job->session == app_state.view.session_revision && lyrics_match(&app_state.view.lyrics, &job->target)) {
        app_state.view.lyrics = lyrics;
    }

    platform_unlock();
}

static void run_job(Worker *worker, Job *job) {
    if (job->action == ACTION_LYRICS) {
        run_lyrics(job);
        return;
    }

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
    bool authenticated = app_state.view.authenticated;
    platform_unlock();

    if (job->action == ACTION_SYNC || job->action == ACTION_SYNC_WEB) {
        if (current && authenticated) {
            run_sync(spotify, job);
        }
        return;
    }

    if (!current || !app_signed_in()) {
        if (job->action == ACTION_LIKE_STATUS) {
            app_finish_like_status(job, NULL);
            return;
        }

        if (job->action == ACTION_TOGGLE_LIKE) {
            app_finish_song_like(job, "Connect your Spotify account first.");
            return;
        }

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

        // A move to this device waited for Soloist; its device id is known only now.
        if (job.action == ACTION_TRANSFER) {
            copy_utf8(job.device.id, sizeof(job.device.id), app_state.view.engine_device_id);
        }
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
        app_state.workers[i].jobs = i == LANE_SYNC ? &app_state.sync_job : app_state.worker_jobs[i];
        app_state.workers[i].capacity = i == LANE_SYNC ? 1 : LANE_CAPACITY;
        snprintf(app_state.workers[i].spotify.client_id, sizeof(app_state.workers[i].spotify.client_id), "%s",
                 app_state.view.settings.client_id);
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
                 "Could not save the key to your credential storage. Check it and try again.");
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
