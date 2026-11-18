#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * "Add to queue" keeps songs in the app instead of Spotify's queue, because Spotify offers no way to
 * remove queued songs and they would otherwise outlive any playlist the listener starts next.
 *
 * Songs reach Spotify one at a time, just before they are due: near the end of the current track, or
 * when the listener presses Next. A song that was already handed over but is no longer wanted (the queue
 * was cleared or replaced) is remembered as stale and skipped as soon as Spotify starts it.
 */

#define HANDOFF_LEAD_MS 20000
#define STALE_TTL_MS (30 * 60 * 1000)

static unsigned find(const char *id) {
    for (unsigned i = 0; i < app_state.up_next_count; ++i) {
        if (!strcmp(app_state.up_next[i].id, id)) {
            return i;
        }
    }

    return app_state.up_next_count;
}

static void remove_at(unsigned index) {
    if (index >= app_state.up_next_count) {
        return;
    }

    --app_state.up_next_count;
    memmove(app_state.up_next + index, app_state.up_next + index + 1,
            (app_state.up_next_count - index) * sizeof(*app_state.up_next));
}

static bool is_stale(const char *id) {
    uint64_t now = platform_milliseconds();

    for (unsigned i = 0; i < UP_NEXT_STALE_LIMIT; ++i) {
        const StaleHandoff *stale = &app_state.stale_handoffs[i];

        if (stale->id[0] && stale->until_ms > now && !strcmp(stale->id, id)) {
            return true;
        }
    }

    return false;
}

static void forget_stale(const char *id) {
    for (unsigned i = 0; i < UP_NEXT_STALE_LIMIT; ++i) {
        if (!strcmp(app_state.stale_handoffs[i].id, id)) {
            app_state.stale_handoffs[i] = (StaleHandoff){0};
        }
    }
}

/* The song Spotify already holds is no longer wanted; skip it when it comes up. */
static void abandon_handoff(void) {
    if (!app_state.handoff_id[0]) {
        return;
    }

    StaleHandoff *slot = &app_state.stale_handoffs[0];

    for (unsigned i = 0; i < UP_NEXT_STALE_LIMIT; ++i) {
        if (!app_state.stale_handoffs[i].id[0]) {
            slot = &app_state.stale_handoffs[i];
            break;
        }
    }

    copy_utf8(slot->id, sizeof(slot->id), app_state.handoff_id);
    slot->until_ms = platform_milliseconds() + STALE_TTL_MS;
    app_state.handoff_id[0] = 0;
}

static void changed(void) {
    ++app_state.up_next_revision;
    app_save_playback_locked(true);
}

bool app_enqueue_item(const CatalogItem *item) {
    if (!item || !item_can_play(item) || item->kind != ITEM_TRACK) {
        return false;
    }

    platform_lock();
    bool ok = app_state.view.authenticated && app_state.up_next_count < UP_NEXT_LIMIT;

    if (ok) {
        app_state.up_next[app_state.up_next_count++] = *item;
        changed();
    }

    snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
             ok ? "" : app_state.view.authenticated ? "Your queue is full." : "Connect your Spotify account first.");
    platform_unlock();
    app_wake();
    return ok;
}

bool app_dequeue(const char *id) {
    platform_lock();
    unsigned index = find(id);
    bool ok = index < app_state.up_next_count;

    if (ok && !strcmp(app_state.handoff_id, id)) {
        abandon_handoff();
    }

    if (ok) {
        remove_at(index);
        changed();
    }

    platform_unlock();
    app_wake();
    return ok;
}

bool app_clear_queue(void) {
    platform_lock();
    bool ok = app_state.up_next_count > 0;

    if (ok) {
        app_replace_up_next_locked();
    }

    platform_unlock();
    app_wake();
    return ok;
}

/* Skips straight to a queued song: the songs ahead of it are dropped, like Spotify does. */
bool app_play_queued(const char *id) {
    platform_lock();
    unsigned index = find(id);
    bool ok = index < app_state.up_next_count && app_state.view.playback.has_device;

    if (ok && strcmp(app_state.handoff_id, id) != 0) {
        abandon_handoff();
    }

    for (unsigned i = 0; ok && i < index; ++i) {
        remove_at(0);
    }

    if (ok) {
        changed();
    }

    platform_unlock();

    if (ok) {
        app_action(ACTION_NEXT);
    }

    return ok;
}

/* Call with the view lock held whenever the listener starts something new: the queue is replaced. */
void app_replace_up_next_locked(void) {
    abandon_handoff();
    ++app_state.up_next_generation;

    if (app_state.up_next_count) {
        app_state.up_next_count = 0;
        changed();
    }
}

/* Call with the view lock held before explicitly playing a song so it is never skipped as stale. */
void app_explicit_play_locked(const char *id) {
    forget_stale(id);
}

void app_append_up_next_locked(const CatalogItem *items, unsigned count) {
    for (unsigned i = 0; i < count && app_state.up_next_count < UP_NEXT_LIMIT; ++i) {
        if (item_can_play(&items[i]) && find(items[i].id) == app_state.up_next_count) {
            app_state.up_next[app_state.up_next_count++] = items[i];
        }
    }

    changed();
}

static bool reserve_order(unsigned needed) {
    if (needed > PLAY_ORDER_LIMIT) {
        return false;
    }

    if (needed <= app_state.play_order_capacity) {
        return true;
    }

    unsigned capacity = needed + 32;

    if (capacity > PLAY_ORDER_LIMIT) {
        capacity = PLAY_ORDER_LIMIT;
    }

    CatalogItem *grown = realloc(app_state.play_order, capacity * sizeof(*grown));

    if (!grown) {
        return false;
    }

    app_state.play_order = grown;
    app_state.play_order_capacity = capacity;
    return true;
}

static void append_order(const CatalogItem *item) {
    if (item_can_play(item) && reserve_order(app_state.play_order_count + 1)) {
        app_state.play_order[app_state.play_order_count++] = *item;
    }
}

/* Call with the view lock held when playback starts from the open list. Spotify only reports about 20
 * upcoming songs, so the queue view continues from the list the listener started instead. */
void app_capture_play_order_locked(const Job *job) {
    const BrowseRequest *source = &app_state.play_order_source;

    // Jumping to a later song of the playing playlist keeps the order already remembered.
    if (app_state.play_order_follow && app_state.play_order_count && job->item.kind == ITEM_TRACK &&
        job->browse.kind == source->kind && !strcmp(job->browse.id, source->id) && !job->track_count) {
        return;
    }

    app_state.play_order_count = 0;
    app_state.play_order_pos = 0;
    app_state.play_order_follow = false;
    app_state.play_order_context[0] = 0;

    BrowseKind kind = job->browse.kind;
    bool track_list = kind == BROWSE_PLAYLIST_TRACKS || kind == BROWSE_SAVED_TRACKS || kind == BROWSE_ALBUM_TRACKS ||
                      kind == BROWSE_ARTIST_LIKES || kind == BROWSE_SONG_RADIO;
    bool whole = job->item.kind == ITEM_PLAYLIST || job->item.kind == ITEM_ALBUM;

    if (!track_list || kind != app_state.view.list.request.kind ||
        strcmp(job->browse.id, app_state.view.list.request.id) != 0 ||
        (!whole && !item_can_play(&job->item))) {
        free(app_state.play_order);
        app_state.play_order = NULL;
        app_state.play_order_capacity = 0;
        return;
    }

    // Songs sent directly are all Spotify will play; a playlist or album context continues to its end.
    if (job->track_count) {
        if (app_state.play_order_capacity > job->track_count + 32) {
            free(app_state.play_order);
            app_state.play_order = NULL;
            app_state.play_order_capacity = 0;
        }

        for (unsigned i = 0; i < job->track_count; ++i) {
            for (unsigned j = 0; j < app_state.view.list.count; ++j) {
                const CatalogItem *row = app_list_item(j);

                if (!strcmp(row->id, job->track_ids[i])) {
                    append_order(row);
                    break;
                }
            }
        }

        return;
    }

    for (unsigned i = 0; i < app_state.view.list.count; ++i) {
        const CatalogItem *row = app_list_item(i);

        if (!whole && !app_state.play_order_count && strcmp(row->id, job->item.id) != 0) {
            continue;
        }

        append_order(row);
    }

    bool arranged = app_state.view.list.sort != LIST_SORT_NONE || app_state.view.list.filter[0];
    app_state.play_order_source = app_state.view.list.request;
    app_state.play_order_followed = app_state.collection_count;
    app_state.play_order_follow = !arranged;

    if (kind == BROWSE_PLAYLIST_TRACKS || kind == BROWSE_ALBUM_TRACKS) {
        snprintf(app_state.play_order_context, sizeof(app_state.play_order_context), "spotify:%s:%s",
                 kind == BROWSE_PLAYLIST_TRACKS ? "playlist" : "album", job->browse.id);
    }

    if (kind == BROWSE_SAVED_TRACKS && job->browse.owner_id[0]) {
        snprintf(app_state.play_order_context, sizeof(app_state.play_order_context), "spotify:user:%.64s:collection",
                 job->browse.owner_id);
    }
}

/* Call with the view lock held after the open list grows, so a playing playlist keeps up as it loads. */
void app_follow_play_order_locked(bool append) {
    const BrowseRequest *source = &app_state.play_order_source;
    const BrowseRequest *list = &app_state.view.list.request;

    if (!app_state.play_order_follow || source->kind != list->kind || strcmp(source->id, list->id) != 0) {
        return;
    }

    // A reload starts over; the order is rebuilt from the beginning, keeping the current position.
    if (!append) {
        app_state.play_order_count = 0;
        app_state.play_order_followed = 0;
    }

    for (unsigned i = app_state.play_order_followed; i < app_state.collection_count; ++i) {
        append_order(&app_state.collection[i]);
    }

    app_state.play_order_followed = app_state.collection_count;
}

/* Index after the playing song in the remembered order, or false when Spotify is playing something else. */
static bool order_next(unsigned *next) {
    const Playback *playback = &app_state.view.playback;
    unsigned prefix = !strncmp(playback->uri, "spotify:track:", 14) ? 14
                      : !strncmp(playback->uri, "spotify:episode:", 16) ? 16
                                                                      : 0;

    if (!app_state.play_order_count || playback->shuffle || !prefix ||
        strcmp(playback->context_uri, app_state.play_order_context) != 0) {
        return false;
    }

    const char *id = playback->uri + prefix;
    unsigned count = app_state.play_order_count;
    unsigned start = app_state.play_order_pos < count ? app_state.play_order_pos : 0;

    // Search onwards from the last known position so repeated songs resolve to the right occurrence.
    for (unsigned step = 0; step < count; ++step) {
        unsigned index = (start + step) % count;

        if (!strcmp(app_state.play_order[index].id, id)) {
            app_state.play_order_pos = index;
            *next = index + 1;
            return true;
        }
    }

    return false;
}

static bool spotify_row_shown(const CatalogItem *item) {
    bool handed_off = app_state.handoff_id[0] && !strcmp(item->id, app_state.handoff_id);
    return !handed_off && !is_stale(item->id);
}

/* Call with the view lock held. The listener's songs come first, then the rest of what is playing. */
unsigned app_queue_total_locked(const Queue *spotify) {
    unsigned next = 0;

    if (order_next(&next)) {
        return app_state.up_next_count + app_state.play_order_count - next;
    }

    unsigned shown = 0;

    for (unsigned i = 0; i < spotify->count; ++i) {
        shown += spotify_row_shown(&spotify->items[i]);
    }

    return app_state.up_next_count + shown;
}

/* Call with the view lock held. */
const CatalogItem *app_queue_at_locked(const Queue *spotify, unsigned index) {
    if (index < app_state.up_next_count) {
        return &app_state.up_next[index];
    }

    index -= app_state.up_next_count;
    unsigned next = 0;

    if (order_next(&next)) {
        return next + index < app_state.play_order_count ? &app_state.play_order[next + index] : NULL;
    }

    for (unsigned i = 0; i < spotify->count; ++i) {
        if (!spotify_row_shown(&spotify->items[i])) {
            continue;
        }

        if (!index) {
            return &spotify->items[i];
        }

        --index;
    }

    return NULL;
}

/* Call with the view lock held. Fills a window of the queue starting near `first` for the snapshot. */
void app_compose_queue_locked(const Queue *spotify, Queue *out, unsigned first) {
    unsigned total = app_queue_total_locked(spotify);
    Queue composed = {.current = spotify->current, .has_current = spotify->has_current,
                      .queued = app_state.up_next_count, .total = total};

    composed.first = first < total ? first : 0;

    for (unsigned i = composed.first; i < total && composed.count < QUEUE_LIMIT; ++i) {
        const CatalogItem *item = app_queue_at_locked(spotify, i);

        if (!item) {
            break;
        }

        composed.items[composed.count++] = *item;
    }

    composed.received = composed.count;
    composed.truncated = spotify->truncated;
    *out = composed;
}

void app_queue_viewport(unsigned first) {
    platform_lock();
    app_state.queue_first = first;
    platform_unlock();
}

/* Hands the first queued song to Spotify unless one is already on its way. */
static bool hand_off(Spotify *spotify, const Playback *target, unsigned session) {
    platform_lock();
    bool claim = session == app_state.view.session_revision && app_state.up_next_count &&
                 !app_state.handoff_id[0] && !app_state.handoff_inflight;
    CatalogItem item = claim ? app_state.up_next[0] : (CatalogItem){0};
    unsigned generation = app_state.up_next_generation;
    app_state.handoff_inflight = app_state.handoff_inflight || claim;
    platform_unlock();

    if (!claim) {
        return false;
    }

    bool ok = spotify_enqueue(spotify, target, &item);

    platform_lock();
    app_state.handoff_inflight = false;

    // The queue was replaced while Spotify was taking the song, so it is already unwanted.
    if (ok && (generation != app_state.up_next_generation || find(item.id) == app_state.up_next_count)) {
        copy_utf8(app_state.handoff_id, sizeof(app_state.handoff_id), item.id);
        abandon_handoff();
        ok = false;
    } else if (ok) {
        copy_utf8(app_state.handoff_id, sizeof(app_state.handoff_id), item.id);
    }

    platform_unlock();
    return ok;
}

bool app_up_next_skip(Spotify *spotify, const Playback *target, unsigned session) {
    hand_off(spotify, target, session);
    return spotify_command(spotify, target, "next");
}

void app_up_next_sync(Spotify *spotify, const Playback *playback, const char *previous_uri, unsigned session) {
    if (strncmp(playback->uri, "spotify:track:", 14) != 0) {
        return;
    }

    const char *id = playback->uri + 14;
    bool started = strcmp(previous_uri, playback->uri) != 0;

    platform_lock();
    bool current = session == app_state.view.session_revision;
    bool skip = current && started && is_stale(id);

    if (skip) {
        forget_stale(id);
    }

    // The handed-off song is playing, so it leaves the app's queue.
    if (current && app_state.handoff_id[0] && !strcmp(app_state.handoff_id, id)) {
        remove_at(find(id));
        app_state.handoff_id[0] = 0;
        changed();
    }

    unsigned remaining = playback->duration_ms > playback->position_ms ? playback->duration_ms - playback->position_ms
                                                                       : 0;
    bool due = current && !skip && playback->playing && playback->position_known && playback->duration_ms &&
               playback->repeat != REPEAT_TRACK && remaining <= HANDOFF_LEAD_MS && app_state.up_next_count;
    platform_unlock();

    if (skip) {
        spotify_command(spotify, playback, "next");
        return;
    }

    if (due && hand_off(spotify, playback, session)) {
        app_wake();
    }
}

/* Call with the view lock held. */
void app_up_next_save_locked(yyjson_mut_doc *doc, yyjson_mut_val *root) {
    yyjson_mut_val *list = yyjson_mut_arr(doc);

    if (!list) {
        return;
    }

    for (unsigned i = 0; i < app_state.up_next_count; ++i) {
        const CatalogItem *item = &app_state.up_next[i];
        yyjson_mut_val *entry = yyjson_mut_obj(doc);

        if (!entry) {
            continue;
        }

        yyjson_mut_obj_add_strcpy(doc, entry, "id", item->id);
        yyjson_mut_obj_add_strcpy(doc, entry, "title", item->title);
        yyjson_mut_obj_add_strcpy(doc, entry, "detail", item->detail);
        yyjson_mut_obj_add_strcpy(doc, entry, "artwork", item->artwork);
        yyjson_mut_obj_add_strcpy(doc, entry, "album_id", item->album_id);
        yyjson_mut_obj_add_strcpy(doc, entry, "album_title", item->album_title);
        yyjson_mut_obj_add_strcpy(doc, entry, "artist_id", item->artist_id);
        yyjson_mut_obj_add_uint(doc, entry, "duration_ms", item->duration_ms);
        yyjson_mut_obj_add_bool(doc, entry, "explicit", item->explicit_content);
        yyjson_mut_arr_append(list, entry);
    }

    yyjson_mut_obj_add_val(doc, root, "up_next", list);
}

static const char *text_field(yyjson_val *entry, const char *key) {
    const char *value = yyjson_get_str(yyjson_obj_get(entry, key));
    return value ? value : "";
}

/* Call with the view lock held. */
void app_up_next_restore_locked(yyjson_val *root) {
    yyjson_val *list = yyjson_obj_get(root, "up_next");
    app_state.up_next_count = 0;

    for (size_t i = 0; i < yyjson_arr_size(list) && app_state.up_next_count < UP_NEXT_LIMIT; ++i) {
        yyjson_val *entry = yyjson_arr_get(list, i);
        const char *id = text_field(entry, "id");

        if (!spotify_id_valid(id)) {
            continue;
        }

        CatalogItem *item = &app_state.up_next[app_state.up_next_count++];
        *item = (CatalogItem){.kind = ITEM_TRACK, .playable = true};
        copy_utf8(item->id, sizeof(item->id), id);
        copy_utf8(item->title, sizeof(item->title), text_field(entry, "title"));
        copy_utf8(item->detail, sizeof(item->detail), text_field(entry, "detail"));
        copy_utf8(item->album_title, sizeof(item->album_title), text_field(entry, "album_title"));

        if (artwork_url_valid(text_field(entry, "artwork"))) {
            copy_utf8(item->artwork, sizeof(item->artwork), text_field(entry, "artwork"));
        }

        if (spotify_id_valid(text_field(entry, "album_id"))) {
            copy_utf8(item->album_id, sizeof(item->album_id), text_field(entry, "album_id"));
        }

        if (spotify_id_valid(text_field(entry, "artist_id"))) {
            copy_utf8(item->artist_id, sizeof(item->artist_id), text_field(entry, "artist_id"));
        }

        yyjson_val *duration = yyjson_obj_get(entry, "duration_ms");
        item->duration_ms = yyjson_is_uint(duration) && yyjson_get_uint(duration) <= UINT32_MAX
                                ? (unsigned)yyjson_get_uint(duration)
                                : 0;
        item->duration_known = item->duration_ms > 0;
        item->explicit_content = yyjson_is_true(yyjson_obj_get(entry, "explicit"));
    }
}
