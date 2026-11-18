#include "app_internal.h"
#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Collection: every loaded row of the open list. Called with the view lock held. */
static bool store_collection(const BrowseRequest *request, const BrowsePage *page, const CatalogItem *items,
                             bool append) {
    unsigned start = append ? app_state.collection_count : 0;

    if (append && page->offset != app_state.view.list.next_offset) {
        return false;
    }

    unsigned needed = start + page->count;

    if (needed > LIST_LIMIT) {
        needed = LIST_LIMIT;
    }

    if (!append && needed == 0 && app_state.collection) {
        free(app_state.collection);
        app_state.collection = NULL;
        app_state.collection_capacity = 0;
    } else if (!append && app_state.collection_capacity > needed + 64 && app_state.collection_capacity > 128) {
        unsigned target = needed > 64 ? needed + 32 : 64;
        CatalogItem *shrunk = realloc(app_state.collection, target * sizeof(*shrunk));

        if (shrunk) {
            app_state.collection = shrunk;
            app_state.collection_capacity = target;
        }
    }

    if (needed > app_state.collection_capacity) {
        unsigned step = needed < 256 ? 64 : 128;
        unsigned capacity = needed + step;

        if (capacity > LIST_LIMIT) {
            capacity = LIST_LIMIT;
        }

        CatalogItem *grown = realloc(app_state.collection, capacity * sizeof(*grown));

        if (!grown) {
            return false;
        }

        app_state.collection = grown;
        app_state.collection_capacity = capacity;
    }

    if (needed > start) {
        memcpy(app_state.collection + start, items, (needed - start) * sizeof(*app_state.collection));
    }

    if (!append) {
        app_state.view.list.duration_ms = 0;
        app_state.view.list.duration_known = true;
        app_state.view.list.max_disc = 0;
        app_state.view.list.request = *request;
        app_state.view.list.request.offset = 0;
        app_state.view.list.selected = -1;
        ++app_state.view.list.revision;

        if (request->kind == BROWSE_SEARCH) {
            memcpy(app_state.view.search_artists, page->artists, sizeof(app_state.view.search_artists));
            memcpy(app_state.view.search_albums, page->albums, sizeof(app_state.view.search_albums));
            memcpy(app_state.view.search_playlists, page->playlists, sizeof(app_state.view.search_playlists));
            app_state.view.search_artist_count = page->artist_count;
            app_state.view.search_album_count = page->album_count;
            app_state.view.search_playlist_count = page->playlist_count;
        }
    }

    app_state.collection_count = needed;
    app_state.view.list.count = needed;

    if (request->kind == BROWSE_PLAYLISTS && app_state.collection && needed > 0) {
        app_sort_playlists_locked(app_state.collection, needed);
    }

    for (unsigned i = 0; i < needed - start; ++i) {
        app_state.view.list.duration_ms += items[i].duration_ms;
        app_state.view.list.duration_known = app_state.view.list.duration_known && items[i].duration_known;

        if (items[i].disc_number > app_state.view.list.max_disc) {
            app_state.view.list.max_disc = items[i].disc_number;
        }
    }

    app_state.view.list.total = page->total;
    app_state.view.list.next_offset = page->offset + page->limit;
    app_state.view.list.more = page->has_next && needed < LIST_LIMIT;
    snprintf(app_state.view.list.next_after, sizeof(app_state.view.list.next_after), "%s", page->next_after);

    if (request->kind == BROWSE_PROFILE_PLAYLISTS && !app_state.view.list.more) {
        app_state.view.list.total = app_state.view.list.count;
    }

    app_state.view.list.loaded = true;
    app_arrange_list_locked();
    app_follow_play_order_locked(append);
    return true;
}

bool app_list_arrangeable(BrowseKind kind) {
    return kind == BROWSE_PLAYLIST_TRACKS || kind == BROWSE_SAVED_TRACKS || kind == BROWSE_ARTIST_LIKES ||
           kind == BROWSE_SONG_RADIO;
}

static bool list_arranged(void) {
    return app_list_arrangeable(app_state.view.list.request.kind) &&
           (app_state.view.list.sort != LIST_SORT_NONE || app_state.view.list.filter[0]);
}

/* Call with the view lock held. Maps a displayed row to its loaded row. */
CatalogItem *app_list_item(unsigned index) {
    if (index >= app_state.view.list.count) {
        return NULL;
    }

    return &app_state.collection[list_arranged() ? app_state.order[index] : index];
}

static int fold(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int compare_text(const char *left, const char *right) {
    while (*left && fold((unsigned char)*left) == fold((unsigned char)*right)) {
        ++left;
        ++right;
    }

    return fold((unsigned char)*left) - fold((unsigned char)*right);
}

static bool contains_text(const char *text, const char *needle) {
    for (; *text; ++text) {
        size_t i = 0;

        while (needle[i] && fold((unsigned char)text[i]) == fold((unsigned char)needle[i])) {
            ++i;
        }

        if (!needle[i]) {
            return true;
        }
    }

    return !needle[0];
}

static int compare_rows(const void *left, const void *right) {
    unsigned a = *(const unsigned *)left;
    unsigned b = *(const unsigned *)right;
    const CatalogItem *x = &app_state.collection[a];
    const CatalogItem *y = &app_state.collection[b];
    int result = 0;

    if (app_state.view.list.sort == LIST_SORT_TITLE) {
        result = compare_text(x->title, y->title);
    }

    if (app_state.view.list.sort == LIST_SORT_ALBUM) {
        result = compare_text(x->album_title, y->album_title);
    }

    if (app_state.view.list.sort == LIST_SORT_DURATION) {
        result = (x->duration_ms > y->duration_ms) - (x->duration_ms < y->duration_ms);
    }

    if (app_state.view.list.sort_descending) {
        result = -result;
    }

    // Keep Spotify's order between equal rows so sorting is stable.
    return result ? result : (a > b) - (a < b);
}

/* Call with the view lock held. Rebuilds the displayed rows after a load, sort or filter change. */
void app_arrange_list_locked(void) {
    unsigned total = app_state.collection_count;

    if (!list_arranged()) {
        if (app_state.order) {
            free(app_state.order);
            app_state.order = NULL;
            app_state.order_capacity = 0;
        }

        app_state.view.list.count = total;
        return;
    }

    if (total == 0 && app_state.order) {
        free(app_state.order);
        app_state.order = NULL;
        app_state.order_capacity = 0;
        app_state.view.list.count = 0;
        return;
    }

    if (app_state.order_capacity > total + 128 && app_state.order_capacity > 256) {
        unsigned *shrunk = realloc(app_state.order, total * sizeof(*shrunk));

        if (shrunk) {
            app_state.order = shrunk;
            app_state.order_capacity = total;
        }
    }

    if (total > app_state.order_capacity) {
        unsigned *grown = realloc(app_state.order, total * sizeof(*grown));

        if (!grown) {
            app_state.view.list.sort = LIST_SORT_NONE;
            app_state.view.list.filter[0] = 0;
            app_state.view.list.count = total;
            return;
        }

        app_state.order = grown;
        app_state.order_capacity = total;
    }

    const char *filter = app_state.view.list.filter;
    unsigned count = 0;

    for (unsigned i = 0; i < total; ++i) {
        const CatalogItem *item = &app_state.collection[i];

        if (contains_text(item->title, filter) || contains_text(item->detail, filter) ||
            contains_text(item->album_title, filter)) {
            app_state.order[count++] = i;
        }
    }

    if (app_state.view.list.sort != LIST_SORT_NONE) {
        qsort(app_state.order, count, sizeof(*app_state.order), compare_rows);
    }

    app_state.view.list.count = count;
}

static bool rearrange(void) {
    app_state.view.list.selected = -1;
    ++app_state.view.list.revision;
    app_arrange_list_locked();
    bool load = list_arranged() && app_state.view.list.more;
    platform_unlock();

    // Sorting or searching only makes sense over the whole playlist, so fetch the remaining pages.
    if (load) {
        app_load_more();
    }

    app_wake();
    return true;
}

bool app_list_sort(ListSort sort, bool descending) {
    platform_lock();

    if (!app_list_arrangeable(app_state.view.list.request.kind) || !app_state.view.list.loaded) {
        platform_unlock();
        return false;
    }

    app_state.view.list.sort = sort;
    app_state.view.list.sort_descending = sort != LIST_SORT_NONE && descending;
    return rearrange();
}

bool app_list_filter(const char *text) {
    platform_lock();

    if (!app_list_arrangeable(app_state.view.list.request.kind) || !app_state.view.list.loaded) {
        platform_unlock();
        return false;
    }

    copy_utf8(app_state.view.list.filter, sizeof(app_state.view.list.filter), text ? text : "");
    return rearrange();
}

bool app_collection_store(const BrowseRequest *request, const BrowsePage *page, bool append) {
    return store_collection(request, page, page->items, append);
}

bool app_collection_store_radio(const BrowseRequest *request, const SongRadio *radio) {
    BrowsePage page = {.count = radio->count, .limit = radio->count, .total = radio->count};
    return store_collection(request, &page, radio->tracks, false);
}

void app_store_library(const BrowseRequest *request, const BrowsePage *page) {
    int library = request->kind == BROWSE_SAVED_TRACKS   ? 0
                  : request->kind == BROWSE_SAVED_ALBUMS ? 1
                  : request->kind == BROWSE_PLAYLISTS    ? 2
                                                         : -1;

    if (library >= 0 && request->offset == 0) {
        app_state.view.library[library] = *page;
        app_state.view.library_loaded[library] = true;
    }
}

static bool fetch_page(Spotify *spotify, const BrowseRequest *request, BrowsePage *page, bool fresh,
                       unsigned session) {
    platform_lock();
    bool current = session == app_state.view.session_revision;

    if (current && fresh) {
        browse_cache_invalidate(&app_state.browse_cache, request);
    }

    bool cached = current && !fresh &&
                  browse_cache_get(&app_state.browse_cache, request, platform_milliseconds(), page);
    platform_unlock();

    if (cached) {
        return true;
    }

    if (!current || !spotify_browse(spotify, request, page)) {
        return false;
    }

    platform_lock();

    if (session == app_state.view.session_revision) {
        browse_cache_store(&app_state.browse_cache, request, page, platform_milliseconds());
    }

    platform_unlock();
    return true;
}

static unsigned like_index(const char *id) {
    for (unsigned i = 0; i < app_state.likes_count; ++i) {
        if (!strcmp(app_state.likes[i].id, id)) {
            return i;
        }
    }

    return app_state.likes_count;
}

static LikeStatus *like_status(const char *id) {
    for (unsigned i = 0; i < LIKE_STATUS_CAPACITY; ++i) {
        if (app_state.like_status[i].id[0] && !strcmp(app_state.like_status[i].id, id)) {
            return &app_state.like_status[i];
        }
    }

    return NULL;
}

/* Call with the view lock held. */
bool app_song_liked(const char *id) {
    if (like_index(id) < app_state.likes_count) {
        return true;
    }

    if (app_state.likes_ready) {
        return false;
    }

    const LikeStatus *status = like_status(id);
    return status && status->known && status->liked;
}

static LikeStatus *remember_status(const char *id) {
    LikeStatus *status = like_status(id);

    if (status) {
        return status;
    }

    status = &app_state.like_status[app_state.like_status_next];
    app_state.like_status_next = (app_state.like_status_next + 1) % LIKE_STATUS_CAPACITY;
    *status = (LikeStatus){0};
    copy_utf8(status->id, sizeof(status->id), id);
    return status;
}

void app_remember_like_locked(const char *id, bool liked) {
    LikeStatus *status = remember_status(id);
    status->known = true;
    status->liked = liked;
}

static void request_like(Job *job, const CatalogItem *item) {
    if (job->track_count >= LIKE_CHECK_LIMIT || item->kind != ITEM_TRACK || !spotify_id_valid(item->id) ||
        like_index(item->id) < app_state.likes_count || like_status(item->id)) {
        return;
    }

    memcpy(job->track_ids[job->track_count++], item->id, sizeof(item->id));

    // An unknown entry marks the song as requested so the next frame does not ask again.
    remember_status(item->id);
}

/* Asks Spotify about songs on screen whose like state the library scan has not reached yet. */
void app_request_like_status(const View *view) {
    platform_lock();

    if (!app_state.view.authenticated || app_state.likes_ready || app_state.like_status_inflight ||
        platform_milliseconds() < app_state.like_status_retry_ms) {
        platform_unlock();
        return;
    }

    Job job = {.action = ACTION_LIKE_STATUS, .session = app_state.view.session_revision};

    if (!strncmp(view->playback.uri, "spotify:track:", 14)) {
        CatalogItem playing = {.kind = ITEM_TRACK};
        copy_utf8(playing.id, sizeof(playing.id), view->playback.uri + 14);
        request_like(&job, &playing);
    }

    if (view->list.request.kind != BROWSE_SAVED_TRACKS) {
        unsigned visible = app_state.viewport_first > view->list.first ? app_state.viewport_first - view->list.first
                                                                        : 0;

        for (unsigned i = visible; i < view->list.rows_count; ++i) {
            request_like(&job, &view->list.rows[i]);
        }
    }

    for (unsigned i = 0; i < view->queue.count; ++i) {
        request_like(&job, &view->queue.items[i]);
    }

    for (unsigned i = 0; i < view->details.popular_count; ++i) {
        request_like(&job, &view->details.popular[i]);
    }

    for (unsigned i = 0; i < view->search_history_count; ++i) {
        request_like(&job, &view->search_history[i]);
    }

    app_state.like_status_inflight = job.track_count > 0;
    platform_unlock();

    if (job.track_count && !app_submit(job)) {
        app_finish_like_status(&job, NULL);
    }
}

void app_finish_like_status(const Job *job, const bool *liked) {
    platform_lock();

    if (job->session != app_state.view.session_revision) {
        platform_unlock();
        return;
    }

    app_state.like_status_inflight = false;

    for (unsigned i = 0; i < job->track_count; ++i) {
        LikeStatus *status = like_status(job->track_ids[i]);

        if (!status || status->known) {
            continue;
        }

        if (liked) {
            status->known = true;
            status->liked = liked[i];
            continue;
        }

        // Forget the request so it is retried, but back off to avoid hammering a failing endpoint.
        *status = (LikeStatus){0};
        app_state.like_status_retry_ms = platform_milliseconds() + 15000;
    }

    platform_unlock();
    app_wake();
}

static bool reserve_likes(unsigned needed) {
    if (needed > LIKES_LIMIT) {
        return false;
    }

    if (needed <= app_state.likes_capacity) {
        return true;
    }

    unsigned capacity = needed + 128;

    if (capacity > LIKES_LIMIT) {
        capacity = LIKES_LIMIT;
    }

    CatalogItem *grown = realloc(app_state.likes, capacity * sizeof(*grown));

    if (!grown) {
        return false;
    }

    app_state.likes = grown;
    app_state.likes_capacity = capacity;
    return true;
}

static void publish_artist_likes(void) {
    BrowseRequest request = app_state.view.list.request;
    bool playlist = request.kind == BROWSE_ARTIST_LIKES;
    unsigned total = 0;
    app_state.view.artist_likes_ready = app_state.likes_ready || app_state.likes_count >= LIKES_LIMIT;

    if (request.kind != BROWSE_ARTIST_ALBUMS && !playlist) {
        app_state.view.artist_liked_count = 0;
        return;
    }

    BrowsePage page = {0};

    if (playlist) {
        app_collection_store(&request, &page, false);
    }

    for (unsigned i = 0; i < app_state.likes_count; ++i) {
        if (!catalog_track_has_artist(&app_state.likes[i], request.id)) {
            continue;
        }

        ++total;

        if (playlist && total <= LIST_LIMIT) {
            page.items[page.count++] = app_state.likes[i];

            if (page.count == BROWSE_PAGE_SIZE) {
                page.limit = page.count;
                app_collection_store(&request, &page, true);
                page.offset += page.count;
                page.count = 0;
            }
        }
    }

    app_state.view.artist_liked_count = total;

    if (playlist) {
        if (page.count) {
            page.limit = page.count;
            app_collection_store(&request, &page, true);
        }

        app_state.view.list.total = total;
        app_state.view.list.loading = app_state.likes_scanning && !total;
    }
}

static void stop_likes_scan(unsigned session, const char *error) {
    platform_lock();

    if (session == app_state.view.session_revision) {
        app_state.likes_scanning = false;
        app_state.view.artist_likes_failed = true;

        if (app_state.view.list.request.kind == BROWSE_ARTIST_LIKES) {
            app_state.view.list.loading = false;
            snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", error);
        }
    }

    platform_unlock();
    app_wake();
}

void app_scan_likes(void) {
    platform_lock();

    if (!app_state.view.authenticated || app_state.likes_ready || app_state.likes_scanning) {
        platform_unlock();
        return;
    }

    unsigned session = app_state.view.session_revision;
    app_state.likes_scanning = true;
    app_state.view.artist_likes_failed = false;

    if (app_state.view.list.request.kind == BROWSE_ARTIST_LIKES) {
        app_state.view.list.loading = !app_state.view.list.count;
        app_state.view.status[0] = 0;
    }

    platform_unlock();

    if (!app_submit((Job){.action = ACTION_LIKES, .session = session})) {
        stop_likes_scan(session, "Could not load liked songs. Refresh to try again.");
    }
}

bool app_browse_artist_likes(const CatalogItem *artist) {
    if (artist->kind != ITEM_ARTIST || !spotify_id_valid(artist->id)) {
        return false;
    }

    BrowseRequest request = {.kind = BROWSE_ARTIST_LIKES};
    copy_utf8(request.id, sizeof(request.id), artist->id);
    copy_utf8(request.artwork, sizeof(request.artwork), artist->artwork);
    static const char prefix[] = "Liked from ";
    memcpy(request.title, prefix, sizeof(prefix));
    copy_utf8(request.title + sizeof(prefix) - 1, sizeof(request.title) - sizeof(prefix) + 1, artist->title);
    platform_lock();

    if (!app_state.view.authenticated) {
        platform_unlock();
        return false;
    }

    ++app_state.browse_seq;
    unsigned revision = app_state.view.list.revision;
    app_state.view.list = (ListView){.request = request, .revision = revision, .selected = -1};
    app_state.viewport_first = 0;
    app_state.view.status[0] = 0;
    publish_artist_likes();
    platform_unlock();
    app_scan_likes();
    app_wake();
    return true;
}

static void handle_likes(Spotify *spotify, const Job *job) {
    platform_lock();
    unsigned offset = app_state.likes_offset;
    platform_unlock();

    TrackPage *page = malloc(sizeof(*page));
    BrowseRequest request = {.kind = BROWSE_SAVED_TRACKS, .offset = offset};
    bool ok = page && spotify_tracks_page(spotify, &request, page);
    bool has_next = ok && page->has_next;

    platform_lock();
    bool current = job->session == app_state.view.session_revision;
    bool more = false;
    unsigned room = app_state.likes_count < LIKES_LIMIT ? LIKES_LIMIT - app_state.likes_count : 0;
    unsigned target = app_state.likes_count + (page->count < room ? page->count : room);
    bool stored = current && ok && (!room || reserve_likes(target));

    if (stored) {
        for (unsigned i = 0; i < page->count && app_state.likes_count < LIKES_LIMIT; ++i) {
            if (like_index(page->items[i].id) == app_state.likes_count) {
                app_state.likes[app_state.likes_count++] = page->items[i];
            }
        }

        app_state.likes_offset = offset + page->scanned;
        bool capped = app_state.likes_count >= LIKES_LIMIT;
        more = has_next && !capped;
        app_state.likes_ready = !has_next;
        app_state.likes_scanning = more;
        publish_artist_likes();
    }

    platform_unlock();
    free(page);

    if (!more) {
        platform_trim_memory();
    }

    app_wake();

    if (current && (!stored || (has_next && !more && app_state.likes_count < LIKES_LIMIT))) {
        stop_likes_scan(job->session, ok ? "Could not finish loading liked songs. Refresh to try again."
                                       : spotify->message);
        return;
    }

    if (more && !app_submit((Job){.action = ACTION_LIKES, .session = job->session})) {
        stop_likes_scan(job->session, "Could not load the next page of liked songs. Refresh to try again.");
    }
}

static void finish_song_like_locked(const Job *job, const char *error) {
    if (job->session != app_state.view.session_revision) {
        return;
    }

    for (unsigned i = 0; job->like_ticket && i < LANE_CAPACITY + 1; ++i) {
        if (app_state.pending_likes[i].ticket == job->like_ticket) {
            app_state.pending_likes[i] = (PendingLike){0};
            break;
        }
    }

    snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", error ? error : "");
}

void app_finish_song_like(const Job *job, const char *error) {
    platform_lock();
    finish_song_like_locked(job, error);
    platform_unlock();
}

static void handle_toggle_like(Spotify *spotify, const Job *job) {
    const char *id = job->item.id;
    bool desired = job->value != 0;
    platform_lock();
    bool liked = app_song_liked(id);
    bool known = liked || app_state.likes_ready;
    platform_unlock();

    // A partial index cannot distinguish an unsaved song from a page we haven't loaded yet.
    if (!known) {
        spotify_song_liked(spotify, id, &liked);
    }

    if (!spotify_save_song(spotify, id, desired)) {
        app_finish_song_like(job, spotify->message[0] ? spotify->message : "Could not update this song's like.");
        return;
    }

    CatalogItem song = job->item;
    bool metadata = !desired || song.artist_id[0] || spotify_song(spotify, id, &song);
    copy_utf8(song.id, sizeof(song.id), id);

    platform_lock();

    if (job->session != app_state.view.session_revision) {
        platform_unlock();
        return;
    }

    app_remember_like_locked(id, desired);
    unsigned index = like_index(id);

    if (!desired && index < app_state.likes_count) {
        --app_state.likes_count;
        memmove(app_state.likes + index, app_state.likes + index + 1,
                (app_state.likes_count - index) * sizeof(*app_state.likes));

        if (app_state.likes_offset) {
            --app_state.likes_offset;
        }
    }

    if (desired && index == app_state.likes_count && reserve_likes(index + 1)) {
        memmove(app_state.likes + 1, app_state.likes, index * sizeof(*app_state.likes));
        app_state.likes[0] = song;
        ++app_state.likes_count;

        if (!liked && app_state.likes_offset) {
            ++app_state.likes_offset;
        }
    }

    BrowseRequest saved = {.kind = BROWSE_SAVED_TRACKS};
    browse_cache_invalidate(&app_state.browse_cache, &saved);

    if (liked != desired && app_state.view.library_loaded[0]) {
        unsigned *total = &app_state.view.library[0].total;
        *total = liked ? (*total ? *total - 1 : 0) : *total + 1;
    }

    if (app_state.view.list.request.kind == BROWSE_SAVED_TRACKS && !app_state.view.list.loading) {
        ListView *list = &app_state.view.list;

        bool present = false;

        for (unsigned i = 0; i < app_state.collection_count; ++i) {
            if (!strcmp(app_state.collection[i].id, id)) {
                present = true;

                if (desired) {
                    break;
                }

                --app_state.collection_count;
                memmove(app_state.collection + i, app_state.collection + i + 1,
                        (app_state.collection_count - i) * sizeof(*app_state.collection));
                list->next_offset -= list->next_offset > 0;
                list->selected = -1;
                break;
            }
        }

        if (desired && !present && app_state.collection_count < LIST_LIMIT) {
            if (app_state.collection_count == app_state.collection_capacity) {
                unsigned capacity = app_state.collection_count + 1;
                CatalogItem *grown = realloc(app_state.collection, capacity * sizeof(*grown));

                if (grown) {
                    app_state.collection = grown;
                    app_state.collection_capacity = capacity;
                }
            }

            if (app_state.collection_count < app_state.collection_capacity) {
                memmove(app_state.collection + 1, app_state.collection,
                        app_state.collection_count * sizeof(*app_state.collection));
                app_state.collection[0] = song;
                ++app_state.collection_count;
                ++list->next_offset;
                list->selected = -1;
            }
        }

        if (liked != desired) {
            list->total = liked ? (list->total ? list->total - 1 : 0) : list->total + 1;
            list->duration_known = false;
        }

        app_arrange_list_locked();
    }

    publish_artist_likes();
    finish_song_like_locked(job, metadata ? NULL : "Song liked, but Spotify could not load its details.");
    platform_unlock();
}

static TrackCacheEntry *track_cache_find(const char *id) {
    for (unsigned i = 0; i < TRACK_CACHE_SLOTS; ++i) {
        TrackCacheEntry *entry = &app_state.track_cache[i];

        if (entry->items && !strcmp(entry->id, id)) {
            return entry;
        }
    }

    return NULL;
}

static unsigned track_cache_total_items(const TrackCacheEntry *exclude) {
    unsigned total = 0;

    for (unsigned i = 0; i < TRACK_CACHE_SLOTS; ++i) {
        const TrackCacheEntry *slot = &app_state.track_cache[i];

        if (slot != exclude && slot->items) {
            total += slot->count;
        }
    }

    return total;
}

/* Call with the view lock held once a playlist has finished loading. */
static void track_cache_store(const char *id, const char *snapshot) {
    TrackCacheEntry *existing = track_cache_find(id);

    if (!app_state.collection_count || app_state.collection_count > TRACK_CACHE_ITEM_LIMIT) {
        if (existing) {
            free(existing->items);
            *existing = (TrackCacheEntry){0};
        }

        return;
    }

    unsigned count = app_state.collection_count;

    while (track_cache_total_items(existing) + count > TRACK_CACHE_ITEM_LIMIT) {
        TrackCacheEntry *oldest = NULL;

        for (unsigned i = 0; i < TRACK_CACHE_SLOTS; ++i) {
            TrackCacheEntry *slot = &app_state.track_cache[i];

            if (slot != existing && slot->items && (!oldest || slot->used < oldest->used)) {
                oldest = slot;
            }
        }

        if (!oldest) {
            break;
        }

        free(oldest->items);
        *oldest = (TrackCacheEntry){0};
    }

    TrackCacheEntry *entry = existing;

    // Otherwise reuse an empty slot, or the least recently opened playlist.
    if (!entry) {
        entry = &app_state.track_cache[0];

        for (unsigned i = 1; entry->items && i < TRACK_CACHE_SLOTS; ++i) {
            TrackCacheEntry *slot = &app_state.track_cache[i];

            if (!slot->items || slot->used < entry->used) {
                entry = slot;
            }
        }
    }

    CatalogItem *items = malloc(count * sizeof(*items));

    if (!items) {
        return;
    }

    memcpy(items, app_state.collection, count * sizeof(*items));
    free(entry->items);
    *entry = (TrackCacheEntry){.items = items, .count = count, .used = ++app_state.track_cache_clock};
    copy_utf8(entry->id, sizeof(entry->id), id);
    copy_utf8(entry->snapshot, sizeof(entry->snapshot), snapshot);
}

/* Call with the view lock held. Shows a remembered full list straight away; the worker then confirms it. */
static bool show_cached_tracks(const BrowseRequest *request) {
    const CatalogItem *items = NULL;
    unsigned count = 0;

    if (request->kind == BROWSE_SAVED_TRACKS && app_state.likes_ready &&
        app_state.likes_count < LIKES_LIMIT) {
        items = app_state.likes;
        count = app_state.likes_count;
    }

    TrackCacheEntry *entry = request->kind == BROWSE_PLAYLIST_TRACKS ? track_cache_find(request->id) : NULL;

    if (entry) {
        entry->used = ++app_state.track_cache_clock;
        items = entry->items;
        count = entry->count;
    }

    if (!items) {
        return false;
    }

    BrowsePage *meta = calloc(1, sizeof(*meta));

    if (!meta) {
        return false;
    }

    meta->count = count;
    meta->limit = count;
    meta->total = count;
    bool stored = store_collection(request, meta, items, false);
    free(meta);
    return stored;
}

bool app_begin_browse(Job job) {
    BrowsePage page;
    platform_lock();

    if (job.browse.kind == BROWSE_PROFILE_PLAYLISTS && app_state.view.profile.loaded) {
        copy_utf8(job.browse.owner_id, sizeof(job.browse.owner_id), app_state.view.profile.id);
    }

    if (job.browse.kind != BROWSE_ARTIST_ALBUMS && job.browse.kind != BROWSE_ARTIST_LIKES &&
        !app_state.likes_scanning && app_state.likes) {
        free(app_state.likes);
        app_state.likes = NULL;
        app_state.likes_capacity = 0;
        app_state.likes_count = 0;
        app_state.likes_offset = 0;
        app_state.likes_ready = false;
        app_state.view.artist_liked_count = 0;
        app_state.view.artist_likes_ready = false;
    }

    unsigned expected = ++app_state.browse_seq;
    unsigned revision = app_state.view.list.revision + 1;
    app_state.view.list =
        (ListView){.request = job.browse, .selected = -1, .revision = revision, .loading = true};

    if (job.browse.kind == BROWSE_SONG_RADIO) {
        app_state.radio_seed = job.item;
    }

    app_state.viewport_first = 0;
    app_state.view.search_artist_count = 0;
    app_state.view.search_album_count = 0;
    app_state.view.search_playlist_count = 0;
    app_state.view.status[0] = 0;
    app_state.view.details = (CollectionInfo){0};
    publish_artist_likes();
    bool details = job.browse.kind == BROWSE_ALBUM_TRACKS || job.browse.kind == BROWSE_ARTIST_ALBUMS;
    app_state.view.details.item.kind = job.browse.kind == BROWSE_ARTIST_ALBUMS ? ITEM_ARTIST : ITEM_ALBUM;
    snprintf(app_state.view.details.item.id, sizeof(app_state.view.details.item.id), "%s", job.browse.id);
    copy_utf8(app_state.view.details.item.title, sizeof(app_state.view.details.item.title), job.browse.title);
    copy_utf8(app_state.view.details.item.artwork, sizeof(app_state.view.details.item.artwork),
              job.browse.artwork);

    for (unsigned i = 0; details && i < DETAIL_CACHE_LIMIT; ++i) {
        if (app_state.detail_cache[i].info.loaded &&
            app_state.detail_cache[i].info.item.kind == app_state.view.details.item.kind &&
            !strcmp(app_state.detail_cache[i].info.item.id, job.browse.id) &&
            platform_milliseconds() - app_state.detail_cache[i].fetched_ms < 300000) {
            app_state.view.details = app_state.detail_cache[i].info;
            details = false;
        }
    }

    bool full_list = job.browse.kind == BROWSE_PLAYLIST_TRACKS || job.browse.kind == BROWSE_SAVED_TRACKS;

    // A remembered full list shows instantly; the job only checks whether it changed.
    if (full_list && app_state.view.authenticated && show_cached_tracks(&job.browse)) {
        job.value = 1;
    }

    bool cached = app_state.view.authenticated && !full_list &&
                  job.browse.kind != BROWSE_SONG_RADIO &&
                  (job.browse.kind != BROWSE_PROFILE_PLAYLISTS || app_state.view.profile.following_known) &&
                  browse_cache_get(&app_state.browse_cache, &job.browse, platform_milliseconds(), &page);

    if (cached) {
        cached = app_collection_store(&job.browse, &page, false);
        app_state.view.list.loading = !cached;
    }

    platform_unlock();

    if (cached) {
        if (details) {
            app_submit((Job){.action = ACTION_DETAILS, .browse = job.browse});
        }

        app_wake();
        return true;
    }

    if (app_submit(job)) {
        if (details) {
            app_submit((Job){.action = ACTION_DETAILS, .browse = job.browse});
        }

        app_wake();
        return true;
    }

    platform_lock();

    if (expected == app_state.browse_seq) {
        app_state.view.list.loading = false;
        snprintf(app_state.view.status, sizeof(app_state.view.status),
                 "Could not schedule this collection. Please try again.");
    }

    platform_unlock();
    app_wake();
    return false;
}

static bool browse(BrowseKind kind, const char *query, SearchFilter filter) {
    Job job = {.action = ACTION_BROWSE, .browse = {.kind = kind, .filter = filter}};
    char url[1200];

    bool valid = (!query || search_append(job.browse.query, query, strlen(query))) &&
                 browse_url(&job.browse, url, sizeof(url));

    if (!valid) {
        platform_lock();
        snprintf(app_state.view.status, sizeof(app_state.view.status),
                 "Enter a search of at most 255 characters.");
        platform_unlock();
        app_wake();
        return false;
    }

    return app_begin_browse(job);
}

bool app_browse(BrowseKind kind, const char *query) {
    return browse(kind, query, SEARCH_ALL);
}

bool app_browse_song_radio(const CatalogItem *item) {
    if (!item || item->kind != ITEM_TRACK || !spotify_id_valid(item->id)) {
        return false;
    }

    Job job = {.action = ACTION_BROWSE, .browse = {.kind = BROWSE_SONG_RADIO}, .item = *item};
    memcpy(job.browse.id, item->id, sizeof(job.browse.id));
    copy_utf8(job.browse.title, sizeof(job.browse.title) - sizeof(" Radio") + 1,
              item->title[0] ? item->title : "Song");
    strcat(job.browse.title, " Radio");
    copy_utf8(job.browse.artwork, sizeof(job.browse.artwork), item->artwork);
    return app_begin_browse(job);
}

bool app_search(const char *query, SearchFilter filter) {
    return browse(BROWSE_SEARCH, query, filter);
}

bool app_load_more(void) {
    platform_lock();
    Job job = {.action = ACTION_MORE, .browse = app_state.view.list.request,
               .session = app_state.view.session_revision, .browse_seq = app_state.browse_seq};
    job.browse.offset = app_state.view.list.next_offset;
    snprintf(job.browse.after, sizeof(job.browse.after), "%s", app_state.view.list.next_after);
    bool ok = app_state.view.authenticated && app_state.view.list.loaded && app_state.view.list.more &&
              !app_state.view.list.loading && app_state.view.list.request.kind != BROWSE_QUEUE;

    if (ok) {
        app_state.view.list.loading = true;
    }

    platform_unlock();

    if (ok && !app_submit(job)) {
        platform_lock();
        if (job.session == app_state.view.session_revision && job.browse_seq == app_state.browse_seq) {
            app_state.view.list.loading = false;
        }
        platform_unlock();
        return false;
    }

    if (ok) {
        app_wake();
    }

    return ok;
}

void app_viewport(unsigned first) {
    platform_lock();
    app_state.viewport_first = first;
    platform_unlock();
}

void app_library_viewport(unsigned first) {
    platform_lock();
    app_state.library_first = first;
    platform_unlock();
}

void app_devices_visible(bool visible) {
    platform_lock();
    bool opened = visible && !app_state.devices_visible;
    app_state.devices_visible = visible;

    // Refresh straight away when the panel opens, then keep it current while it stays open.
    if (opened) {
        app_state.next_devices_ms = 0;
    }

    platform_unlock();

    if (opened) {
        app_wake();
    }
}

void app_queue_visible(bool visible) {
    platform_lock();
    app_state.queue_visible = visible;
    platform_unlock();
}

bool app_select(int index) {
    platform_lock();
    bool ok = index >= -1 && (index < 0 || (unsigned)index < app_state.view.list.count);

    if (ok) {
        app_state.view.list.selected = index;

        if (index >= 0 && ((unsigned)index < app_state.viewport_first ||
                           (unsigned)index >= app_state.viewport_first + 24)) {
            app_state.viewport_first = (unsigned)index;
        }
    }

    platform_unlock();

    if (ok) {
        app_wake();
    }

    return ok;
}

bool app_activate(void) {
    CatalogItem item;
    return app_selected_item(&item) && app_open_item(&item);
}

bool app_filter_discography(const char *group) {
    platform_lock();
    Job job = {.action = ACTION_BROWSE, .browse = app_state.view.list.request};
    bool valid = job.browse.kind == BROWSE_ARTIST_ALBUMS && spotify_id_valid(job.browse.id);
    platform_unlock();

    if (!valid) {
        return false;
    }

    job.browse.offset = 0;
    job.browse.query[0] = 0;

    if (group && group[0] && !search_append(job.browse.query, group, strlen(group))) {
        return false;
    }

    return app_begin_browse(job);
}

static bool store_library_playlists_locked(const BrowsePage *page, unsigned start, bool has_more) {
    unsigned needed = start + page->count;

    if (needed > PLAYLISTS_LIMIT) {
        needed = PLAYLISTS_LIMIT;
    }

    if (needed > app_state.playlist_capacity) {
        unsigned capacity = needed + 32 < PLAYLISTS_LIMIT ? needed + 32 : PLAYLISTS_LIMIT;
        CatalogItem *grown = realloc(app_state.playlists, capacity * sizeof(*app_state.playlists));

        if (!grown) {
            return false;
        }

        app_state.playlists = grown;
        app_state.playlist_capacity = capacity;
    }

    unsigned copy_count = needed > start ? needed - start : 0;

    if (copy_count) {
        memcpy(app_state.playlists + start, page->items, copy_count * sizeof(*app_state.playlists));
    }

    app_sort_playlists_locked(app_state.playlists, needed);
    app_state.view.playlists.count = needed;
    app_state.view.playlists.total = page->total;
    app_state.view.playlists.loaded = true;
    app_state.view.playlists.more = has_more && needed < PLAYLISTS_LIMIT;
    app_state.view.playlists.next_offset = page->offset + page->limit;
    ++app_state.view.playlists.revision;
    return true;
}

void app_load_library(Spotify *spotify, unsigned session) {
    static const BrowseKind kinds[] = {BROWSE_SAVED_TRACKS, BROWSE_SAVED_ALBUMS, BROWSE_PLAYLISTS};
    bool playlist_loaded = false;
    BrowsePage playlist_page = {0};

    for (unsigned i = 0; i < 3; ++i) {
        BrowseRequest request = {.kind = kinds[i]};
        BrowsePage page;
        bool loaded = fetch_page(spotify, &request, &page, false, session);

        platform_lock();

        if (loaded && session == app_state.view.session_revision) {
            app_store_library(&request, &page);

            if (i == 0 && !app_state.view.list.loaded && !app_state.view.list.loading) {
                app_collection_store(&request, &page, false);
            }

            if (i == 2) {
                playlist_loaded = true;
                playlist_page = page;
                store_library_playlists_locked(&page, 0, page.has_next);
            }
        }

        platform_unlock();
        app_wake();
    }

    platform_lock();

    if (session == app_state.view.session_revision) {
        app_state.startup_library_done = true;
        app_sync_startup_intro_locked();
    }

    platform_unlock();
    app_wake();

    if (!playlist_loaded || playlist_page.has_next) {
        unsigned next_offset = playlist_loaded ? playlist_page.offset + playlist_page.limit : 0;
        app_submit(
            (Job){.action = ACTION_LIBRARY, .browse = {.kind = BROWSE_PLAYLISTS, .offset = next_offset}});
    }
}

static void publish_details(const Job *job, const CollectionInfo *info) {
    platform_lock();

    if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
        app_state.view.details = *info;
        unsigned slot = 0;

        for (unsigned i = 0; i < DETAIL_CACHE_LIMIT; ++i) {
            if (app_state.detail_cache[i].info.item.kind == info->item.kind &&
                !strcmp(app_state.detail_cache[i].info.item.id, info->item.id)) {
                slot = i;
                break;
            }

            if (app_state.detail_cache[i].fetched_ms < app_state.detail_cache[slot].fetched_ms) {
                slot = i;
            }
        }

        app_state.detail_cache[slot].info = *info;
        app_state.detail_cache[slot].fetched_ms = platform_milliseconds();
    }

    platform_unlock();
    app_wake();
}

static void handle_details(Spotify *spotify, const Job *job) {
    CollectionInfo info;
    ItemKind kind = job->browse.kind == BROWSE_ARTIST_ALBUMS ? ITEM_ARTIST : ITEM_ALBUM;

    if (!spotify_collection(spotify, kind, job->browse.id, &info)) {
        return;
    }

    publish_details(job, &info);

    if (kind == ITEM_ARTIST) {
        spotify_popular(spotify, job->browse.id, &info);
        info.popular_loaded = true;
        publish_details(job, &info);
    }

    info.saved_known = spotify_collection_saved(spotify, &info.item, &info.saved);
    info.permission_required = !info.saved_known && spotify->last_status == 403;
    publish_details(job, &info);
}

static void handle_save_collection(Spotify *spotify, const Job *job) {
    bool ok = spotify_save_collection(spotify, &job->item, job->value != 0);
    platform_lock();
    bool current = job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq;

    if (ok && job->session == app_state.view.session_revision) {
        BrowseRequest changed = {.kind = job->item.kind == ITEM_ARTIST ? BROWSE_FOLLOWED_ARTISTS
                                                                       : BROWSE_SAVED_ALBUMS};
        browse_cache_invalidate(&app_state.browse_cache, &changed);

        for (unsigned i = 0; i < DETAIL_CACHE_LIMIT; ++i) {
            CollectionInfo *info = &app_state.detail_cache[i].info;

            if (info->item.kind == job->item.kind && !strcmp(info->item.id, job->item.id)) {
                info->loaded = false;
            }
        }
    }

    if (current) {
        app_state.view.details.saved = ok ? job->value != 0 : job->value == 0;
        app_state.view.details.saved_known = ok;
        app_state.view.details.saving = false;
        app_state.view.details.permission_required = !ok && spotify->last_status == 403;

        if (ok && job->item.kind == ITEM_ARTIST && app_state.view.details.followers_known) {
            if (job->value != 0) {
                ++app_state.view.details.followers;
            } else if (app_state.view.details.followers > 0) {
                --app_state.view.details.followers;
            }
        }

        if (!ok && !app_state.view.details.permission_required) {
            snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", spotify->message);
        }
    }

    CollectionInfo info = app_state.view.details;
    platform_unlock();

    if (current) {
        publish_details(job, &info);
    }

    if (ok && job->item.kind == ITEM_ARTIST) {
        BrowsePage following;
        BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};
        bool followed_ok = fetch_page(spotify, &artists, &following, true, job->session);
        platform_lock();

        if (followed_ok && job->session == app_state.view.session_revision) {
            memcpy(app_state.view.profile.following, following.items,
                   following.count * sizeof(*app_state.view.profile.following));
            app_state.view.profile.following_count = following.count;
            app_state.view.profile.following_total = following.total;
            app_state.view.profile.following_known = true;
            app_state.view.profile.permission_required = false;
        }

        platform_unlock();
        app_wake();
    }
}

static void handle_home(Spotify *spotify, const Job *job) {
    BrowsePage page;
    bool ok = spotify_recent_albums(spotify, &page);
    platform_lock();
    bool need_profile = !app_state.view.profile.loaded;

    if (job->session == app_state.view.session_revision) {
        app_state.view.recent_loaded = true;
        app_state.view.recent_permission_required = !ok && spotify->last_status == 403;

        if (ok) {
            app_state.view.recent_count = page.count;
            memcpy(app_state.view.recent, page.items, page.count * sizeof(*app_state.view.recent));
        }
    }

    platform_unlock();

    if (!need_profile) {
        platform_lock();

        if (job->session == app_state.view.session_revision) {
            app_state.startup_home_done = true;
            app_sync_startup_intro_locked();
        }

        platform_unlock();
        app_wake();
        return;
    }

    ProfileInfo profile = {0};

    if (!spotify_profile(spotify, &profile)) {
        platform_lock();

        if (job->session == app_state.view.session_revision) {
            app_state.startup_home_done = true;
            app_sync_startup_intro_locked();
        }

        platform_unlock();
        app_wake();
        return;
    }

    BrowsePage following;
    BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};

    if (fetch_page(spotify, &artists, &following, false, job->session)) {
        memcpy(profile.following, following.items, following.count * sizeof(*profile.following));
        profile.following_count = following.count;
        profile.following_total = following.total;
        profile.following_known = true;
    } else if (spotify->last_status == 403) {
        profile.permission_required = true;
    }

    platform_lock();

    if (job->session == app_state.view.session_revision && !app_state.view.profile.loaded) {
        app_state.view.profile = profile;
    }

    if (job->session == app_state.view.session_revision) {
        app_state.startup_home_done = true;
        app_sync_startup_intro_locked();
    }

    platform_unlock();
    app_wake();
}

static void handle_library(Spotify *spotify, const Job *job) {
    BrowsePage page;
    bool ok = fetch_page(spotify, &job->browse, &page, job->value != 0, job->session);
    platform_lock();
    bool current = job->session == app_state.view.session_revision;
    unsigned start = job->browse.offset ? app_state.view.playlists.count : 0;

    if (current && ok) {
        ok = store_library_playlists_locked(&page, start, page.has_next);
    }

    bool more = current && ok && app_state.view.playlists.more;
    BrowseRequest next = {.kind = BROWSE_PLAYLISTS, .offset = app_state.view.playlists.next_offset};
    app_state.view.playlists.loading = more;
    platform_unlock();

    if (current && ok) {
        app_wake();
    }

    if (more) {
        app_submit((Job){.action = ACTION_LIBRARY, .browse = next, .value = job->value});
    }
}

static void handle_queue(Spotify *spotify, const Job *job) {
    Queue queue;
    bool ok = spotify_queue(spotify, &queue);
    platform_lock();

    if (ok && job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
        BrowseRequest request = {.kind = BROWSE_QUEUE};
        bool keep_restored = app_state.restored_pending && queue.count == 0 && app_state.view.queue.count > 0;
        const Queue *source = keep_restored ? &app_state.view.queue : &queue;

        if (!keep_restored) {
            app_state.view.queue = queue;
            app_save_playback_locked(false);
        }

        // The full-page queue matches the side panel: the listener's songs, then the rest of what plays.
        unsigned total = app_queue_total_locked(source);
        BrowsePage page = {.total = total};
        app_collection_store(&request, &page, false);

        for (unsigned offset = 0; offset < total && offset < LIST_LIMIT; offset += BROWSE_PAGE_SIZE) {
            page = (BrowsePage){.offset = offset, .limit = BROWSE_PAGE_SIZE, .total = total};

            for (unsigned i = offset; i < total && page.count < BROWSE_PAGE_SIZE; ++i) {
                const CatalogItem *item = app_queue_at_locked(source, i);

                if (item) {
                    page.items[page.count++] = *item;
                }
            }

            app_collection_store(&request, &page, offset > 0);
        }
    }

    if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
        app_state.view.list.loading = false;

        if (!ok) {
            snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", spotify->message);
        }
    }

    platform_unlock();
}

/* Whether a freshly fetched first page matches the list already on screen. */
static bool tracks_unchanged(const TrackPage *page) {
    if (page->total != app_state.collection_count || page->count > app_state.collection_count) {
        return false;
    }

    for (unsigned i = 0; i < page->count; ++i) {
        if (strcmp(page->items[i].id, app_state.collection[i].id) != 0) {
            return false;
        }
    }

    return true;
}

/* Playlists and Liked Songs always load in full, 50 songs per request, so search and sort see everything. */
static void handle_tracks(Spotify *spotify, const Job *job) {
    platform_lock();

    if (job->session != app_state.view.session_revision || job->browse_seq != app_state.browse_seq) {
        platform_unlock();
        return;
    }

    BrowseRequest request = job->browse;
    bool append = job->action == ACTION_MORE;
    bool validate = job->action == ACTION_BROWSE && job->value == 1;
    bool playlist = request.kind == BROWSE_PLAYLIST_TRACKS;
    request.offset = append ? job->browse.offset : 0;
    TrackCacheEntry *entry = playlist && validate ? track_cache_find(request.id) : NULL;
    char cached_snapshot[128] = {0};

    if (entry) {
        copy_utf8(cached_snapshot, sizeof(cached_snapshot), entry->snapshot);
    }

    char snapshot[128] = {0};

    if (playlist && append) {
        copy_utf8(snapshot, sizeof(snapshot), app_state.loading_snapshot);
    }

    platform_unlock();

    if (playlist && !append) {
        spotify_playlist_snapshot(spotify, request.id, snapshot, sizeof(snapshot));
    }

    // The remembered playlist is still current, so nothing needs reloading.
    if (validate && playlist && snapshot[0] && !strcmp(snapshot, cached_snapshot)) {
        platform_lock();

        if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
            app_state.view.list.loading = false;
        }

        platform_unlock();
        return;
    }

    TrackPage *page = malloc(sizeof(*page));
    bool ok = page && spotify_tracks_page(spotify, &request, page);
    bool consistent = false;

    if (ok && playlist && !page->has_next && snapshot[0]) {
        char final_snapshot[128];
        consistent = spotify_playlist_snapshot(spotify, request.id, final_snapshot, sizeof(final_snapshot)) &&
                     !strcmp(snapshot, final_snapshot);
    }

    platform_lock();
    bool current = job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq;
    bool more = false;

    if (ok && current && validate && !playlist && tracks_unchanged(page)) {
        app_state.view.list.loading = false;
        platform_unlock();
        free(page);
        return;
    }

    if (ok && current) {
        BrowsePage *meta = calloc(1, sizeof(*meta));
        bool stored = false;

        if (meta) {
            meta->count = page->count;
            meta->offset = request.offset;
            meta->limit = page->scanned;
            meta->total = page->total;
            meta->has_next = page->has_next;
            stored = store_collection(&request, meta, page->items, append);

            // The sidebar's Liked Songs count comes from the first page.
            if (stored && !append && !playlist) {
                meta->count = page->count < BROWSE_PAGE_SIZE ? page->count : BROWSE_PAGE_SIZE;
                app_store_library(&request, meta);
            }

            free(meta);
        }

        if (stored && !append && playlist) {
            copy_utf8(app_state.loading_snapshot, sizeof(app_state.loading_snapshot), snapshot);
        }

        more = stored && app_state.view.list.more;

        if (stored && consistent) {
            track_cache_store(request.id, snapshot);
        }

        ok = stored;
        snprintf(spotify->message, sizeof(spotify->message), "%s", stored ? "" : "Could not store playlist tracks.");
        app_state.view.status[0] = 0;
    }

    if (current) {
        app_state.view.list.loading = false;
    }

    if (!ok && current) {
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 page ? spotify->message : "Could not allocate playlist tracks.");
    }

    platform_unlock();
    free(page);

    if (more) {
        app_load_more();
    } else {
        platform_trim_memory();
    }

    app_wake();
}

static void handle_browse(Spotify *spotify, const Job *job) {
    if (job->browse.kind == BROWSE_PLAYLIST_TRACKS || job->browse.kind == BROWSE_SAVED_TRACKS) {
        handle_tracks(spotify, job);
        return;
    }

    platform_lock();
    BrowseRequest request = job->browse;
    bool append = job->action == ACTION_MORE;

    if (request.kind == BROWSE_PROFILE_PLAYLISTS && job->action == ACTION_REFRESH) {
        request.owner_id[0] = 0;
    }

    bool use_sorted_library = !append && job->action != ACTION_REFRESH && request.kind == BROWSE_PLAYLISTS &&
                              job->session == app_state.view.session_revision &&
                              job->browse_seq == app_state.browse_seq && app_state.view.playlists.loaded &&
                              !app_state.view.playlists.more && app_state.playlists &&
                              app_state.view.playlists.count > 0;

    if (use_sorted_library) {
        unsigned count = app_state.view.playlists.count;

        if (count > LIST_LIMIT) {
            count = LIST_LIMIT;
        }

        if (count <= 128 && app_state.collection_capacity > 256) {
            CatalogItem *shrunk = realloc(app_state.collection, 128 * sizeof(*shrunk));

            if (shrunk) {
                app_state.collection = shrunk;
                app_state.collection_capacity = 128;
            }
        }

        if (count > app_state.collection_capacity) {
            unsigned cap = count < 128 ? 128 : count;
            CatalogItem *grown = realloc(app_state.collection, cap * sizeof(*grown));

            if (grown) {
                app_state.collection = grown;
                app_state.collection_capacity = cap;
            }
        }

        if (app_state.collection && app_state.collection_capacity >= count) {
            memcpy(app_state.collection, app_state.playlists, count * sizeof(*app_state.collection));
            app_sort_playlists_locked(app_state.collection, count);
            app_state.view.list.duration_ms = 0;
            app_state.view.list.duration_known = true;
            app_state.view.list.max_disc = 0;
            app_state.view.list.request = request;
            app_state.view.list.request.offset = 0;
            app_state.view.list.selected = -1;
            app_state.view.list.count = count;
            app_state.view.list.total = count;
            app_state.view.list.next_offset = count;
            app_state.view.list.more = false;
            app_state.view.list.loaded = true;
            app_state.view.list.loading = false;
            app_state.view.list.next_after[0] = 0;
            ++app_state.view.list.revision;
            app_state.view.status[0] = 0;
            platform_unlock();
            app_wake();
            return;
        }
    }

    platform_unlock();

    if (request.kind == BROWSE_PROFILE_PLAYLISTS && !append) {
        ProfileInfo profile = {0};

        if (!spotify_profile(spotify, &profile)) {
            platform_lock();

            if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
                app_state.view.list.loading = false;
                snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", spotify->message);
            }

            platform_unlock();
            return;
        }

        copy_utf8(request.owner_id, sizeof(request.owner_id), profile.id);
        BrowsePage following;
        BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};

        if (fetch_page(spotify, &artists, &following, job->action == ACTION_REFRESH, job->session)) {
            memcpy(profile.following, following.items, following.count * sizeof(*profile.following));
            profile.following_count = following.count;
            profile.following_total = following.total;
            profile.following_known = true;
        } else if (spotify->last_status == 403) {
            profile.permission_required = true;
        }

        platform_lock();

        if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
            app_state.view.profile = profile;
        }

        platform_unlock();
        app_wake();
    }

    BrowsePage page;
    bool is_radio = request.kind == BROWSE_SONG_RADIO;
    SongRadio *radio = is_radio ? malloc(sizeof(*radio)) : NULL;
    bool ok = is_radio ? radio && spotify_song_radio(spotify, &job->item, radio)
                       : fetch_page(spotify, &request, &page, job->action == ACTION_REFRESH, job->session);

    platform_lock();

    // Drop results for a list the listener has already left.
    bool current = job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq;

    if (ok && current) {
        if (is_radio) {
            if (!app_collection_store_radio(&request, radio)) {
                snprintf(spotify->message, sizeof(spotify->message), "Could not load the radio playlist.");
                ok = false;
            }
        } else {
            app_collection_store(&request, &page, append);
            app_store_library(&request, &page);
        }

        if (ok) {
            snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                     is_radio && radio->count == 1 ? spotify->message : "");
        }
    }

    if (current) {
        app_state.view.list.loading = false;
    }

    if (!ok && current) {
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s",
                 is_radio && !radio ? "Could not load the radio playlist." : spotify->message);
    }

    bool load_rest = ok && current && list_arranged() && app_state.view.list.more;
    platform_unlock();
    free(radio);

    if (load_rest) {
        app_load_more();
    }
}

void app_run_content(Spotify *spotify, const Job *job) {
    if (job->action == ACTION_LIKES) {
        handle_likes(spotify, job);
        return;
    }

    if (job->action == ACTION_TOGGLE_LIKE) {
        handle_toggle_like(spotify, job);
        return;
    }

    if (job->action == ACTION_REFRESH && job->browse.kind == BROWSE_ARTIST_LIKES) {
        return;
    }

    if (job->action == ACTION_DETAILS) {
        handle_details(spotify, job);
        return;
    }

    if (job->action == ACTION_SAVE_COLLECTION) {
        handle_save_collection(spotify, job);
        return;
    }

    if (job->action == ACTION_HOME) {
        handle_home(spotify, job);
        return;
    }

    if (job->action == ACTION_QUEUE_PANEL) {
        Queue queue;
        bool ok = spotify_queue(spotify, &queue);
        platform_lock();

        if (ok && job->session == app_state.view.session_revision &&
            !(app_state.restored_pending && queue.count == 0 && app_state.view.queue.count > 0)) {
            app_state.view.queue = queue;
            app_save_playback_locked(false);
        }

        platform_unlock();
        return;
    }

    if (job->action == ACTION_LIBRARY) {
        handle_library(spotify, job);
        return;
    }

    if (job->action == ACTION_OPEN_LINK) {
        char url[128];

        if (!item_link(&job->item, url, sizeof(url)) || !platform_open_url(url)) {
            app_set_status(job->session, "Could not open the Spotify link.");
        }

        return;
    }

    if (job->action == ACTION_QUEUE) {
        handle_queue(spotify, job);
        return;
    }

    if (job->action == ACTION_BROWSE || job->action == ACTION_MORE || job->action == ACTION_REFRESH) {
        handle_browse(spotify, job);
    }
}
