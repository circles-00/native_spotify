#include "app.h"
#include "api.h"
#include "browse_cache.h"
#include "platform.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Content, playback control and status reads use independent lanes.
 * Each lane has its own request state; OAuth tokens are shared under platform_token_lock().
 */
typedef enum { LANE_PLAYER, LANE_CONTENT, LANE_STATE, LANE_COUNT } Lane;

typedef struct {
    Action action;
    BrowseRequest browse;
    CatalogItem item;
    Device device;
    Playback target;
    unsigned value;
    unsigned session;
    unsigned seq;
    unsigned browse_seq;
} Job;

#define LANE_CAPACITY 16

typedef struct {
    Job jobs[LANE_CAPACITY];
    unsigned head;
    unsigned count;
    bool running;
    bool active;
    Action action;
    Spotify spotify;
} Worker;

static View view;
static Worker workers[LANE_COUNT];
static Tokens tokens;
static BrowseCache browse_cache;
static PlayerSync sync;
static PlayerSeek seek;
static uint64_t next_paint_ms;
static uint64_t next_devices_ms;
static unsigned command_seq;
static unsigned browse_seq;
static bool shutting_down;
static Job deferred;
static bool has_deferred;
static CatalogItem *collection;
static unsigned collection_capacity;
static unsigned viewport_first;
static CatalogItem *playlists;
static unsigned playlist_capacity;
static unsigned library_first;
static bool queue_visible;
static char radio_guarded_uri[128];

static struct {
    CollectionInfo info;
    uint64_t fetched_ms;
} detail_cache[8];

const CatalogItem *list_row(const ListView *list, unsigned index) {
    if (index < list->first || index - list->first >= list->rows_count) {
        return NULL;
    }

    return &list->rows[index - list->first];
}

static Lane lane_for(Action action) {
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

static bool player_command(Action action) {
    return lane_for(action) == LANE_PLAYER && action != ACTION_SYNC && action != ACTION_DEVICES;
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

        if (!worker->count || shutting_down) {
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
        ui_wake();
    }
}

static bool submit(Job job) {
    Worker *worker = &workers[lane_for(job.action)];

    platform_lock();

    if (shutting_down) {
        platform_unlock();
        return false;
    }

    job.session = view.session_revision;
    job.seq = command_seq;
    job.browse_seq = browse_seq;

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

static bool command_pending(void) {
    Worker *worker = &workers[LANE_PLAYER];

    if (worker->active && player_command(worker->action)) {
        return true;
    }

    for (unsigned i = 0; i < worker->count; ++i) {
        if (player_command(worker->jobs[(worker->head + i) % LANE_CAPACITY].action)) {
            return true;
        }
    }

    return false;
}

/* Lanes copy the shared tokens; refresh is single-flight because it happens under the token lock. */
static void lane_begin(Spotify *spotify) {
    platform_token_lock();
    spotify->tokens = tokens;

    if (spotify->tokens.refresh[0] && spotify->tokens.expires_at <= platform_seconds() + 60) {
        spotify_ensure_token(spotify);
        tokens = spotify->tokens;
    }

    platform_token_unlock();
}

static void lane_end(Spotify *spotify) {
    platform_token_lock();

    // A 401 can refresh mid-request; a rejected refresh token wipes the lane copy (session over).
    bool refreshed = spotify->tokens.refresh[0] && spotify->tokens.expires_at > tokens.expires_at;
    bool revoked = !spotify->tokens.refresh[0] && tokens.refresh[0] && spotify->last_status == 400;

    if (refreshed) {
        tokens = spotify->tokens;
    }

    if (revoked) {
        platform_wipe(&tokens, sizeof(tokens));
    }

    platform_token_unlock();
    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));

    if (revoked) {
        platform_lock();
        view.authenticated = false;
        snprintf(view.status, sizeof(view.status), "Your Spotify session expired. Log in again.");
        platform_unlock();
    }
}

static void replace_tokens(const Tokens *replacement) {
    platform_token_lock();
    tokens = *replacement;
    platform_token_unlock();
}

static bool signed_in(void) {
    platform_token_lock();
    bool ok = tokens.refresh[0] != 0;
    platform_token_unlock();
    return ok;
}

static void set_status(unsigned session, const char *message) {
    platform_lock();

    if (session == view.session_revision) {
        snprintf(view.status, sizeof(view.status), "%s", message);
    }

    platform_unlock();
}

/* Collection: every loaded row of the open list. Called with the view lock held. */
static bool collection_store(const BrowseRequest *request, const BrowsePage *page, bool append) {
    unsigned start = append ? view.list.count : 0;

    if (append && page->offset != view.list.next_offset) {
        return false;
    }

    unsigned needed = start + page->count;

    if (needed > LIST_LIMIT) {
        needed = LIST_LIMIT;
    }

    if (needed > collection_capacity) {
        unsigned capacity = collection_capacity ? collection_capacity * 2 : 256;

        while (capacity < needed) {
            capacity *= 2;
        }

        CatalogItem *grown =
            realloc(collection, (capacity > LIST_LIMIT ? LIST_LIMIT : capacity) * sizeof(*grown));

        if (!grown) {
            return false;
        }

        collection = grown;
        collection_capacity = capacity > LIST_LIMIT ? LIST_LIMIT : capacity;
    }

    if (needed > start) {
        memcpy(collection + start, page->items, (needed - start) * sizeof(*collection));
    }

    if (!append) {
        view.list.duration_ms = 0;
        view.list.duration_known = true;
        view.list.max_disc = 0;
        view.list.request = *request;
        view.list.request.offset = 0;
        view.list.selected = -1;
        ++view.list.revision;

        if (request->kind == BROWSE_SEARCH) {
            memcpy(view.search_artists, page->artists, sizeof(view.search_artists));
            memcpy(view.search_albums, page->albums, sizeof(view.search_albums));
            view.search_artist_count = page->artist_count;
            view.search_album_count = page->album_count;
        }
    }

    view.list.count = needed;
    for (unsigned i = 0; i < page->count; ++i) {
        view.list.duration_ms += page->items[i].duration_ms;
        view.list.duration_known = view.list.duration_known && page->items[i].duration_known;

        if (page->items[i].disc_number > view.list.max_disc) {
            view.list.max_disc = page->items[i].disc_number;
        }
    }

    view.list.total = page->total;
    view.list.next_offset = page->offset + page->limit;
    view.list.more = page->has_next && needed < LIST_LIMIT;
    snprintf(view.list.next_after, sizeof(view.list.next_after), "%s", page->next_after);

    if (request->kind == BROWSE_PROFILE_PLAYLISTS && !view.list.more) {
        view.list.total = view.list.count;
    }
    view.list.loaded = true;
    return true;
}

static void store_library(const BrowseRequest *request, const BrowsePage *page) {
    int library = request->kind == BROWSE_SAVED_TRACKS   ? 0
                  : request->kind == BROWSE_SAVED_ALBUMS ? 1
                  : request->kind == BROWSE_PLAYLISTS    ? 2
                                                         : -1;

    if (library >= 0 && request->offset == 0) {
        view.library[library] = *page;
        view.library_loaded[library] = true;
    }
}

static bool fetch_page(Spotify *spotify, const BrowseRequest *request, BrowsePage *page, bool fresh) {
    platform_lock();
    bool cached = !fresh && browse_cache_get(&browse_cache, request, platform_milliseconds(), page);
    platform_unlock();

    if (cached) {
        return true;
    }

    if (!spotify_browse(spotify, request, page)) {
        return false;
    }

    platform_lock();
    browse_cache_store(&browse_cache, request, page, platform_milliseconds());
    platform_unlock();
    return true;
}

static bool begin_browse(Job job) {
    BrowsePage page;
    platform_lock();

    if (job.browse.kind == BROWSE_PROFILE_PLAYLISTS && view.profile.loaded) {
        snprintf(job.browse.owner_id, sizeof(job.browse.owner_id), "%s", view.profile.id);
    }
    unsigned expected = ++browse_seq;
    unsigned revision = view.list.revision + 1;
    view.list = (ListView){.request = job.browse, .selected = -1, .revision = revision, .loading = true};
    viewport_first = 0;
    view.search_artist_count = 0;
    view.search_album_count = 0;
    view.status[0] = 0;
    view.details = (CollectionInfo){0};
    bool details = job.browse.kind == BROWSE_ALBUM_TRACKS || job.browse.kind == BROWSE_ARTIST_ALBUMS;
    view.details.item.kind = job.browse.kind == BROWSE_ARTIST_ALBUMS ? ITEM_ARTIST : ITEM_ALBUM;
    snprintf(view.details.item.id, sizeof(view.details.item.id), "%s", job.browse.id);
    copy_utf8(view.details.item.title, sizeof(view.details.item.title), job.browse.title);
    copy_utf8(view.details.item.artwork, sizeof(view.details.item.artwork), job.browse.artwork);

    for (unsigned i = 0; details && i < 8; ++i) {
        if (detail_cache[i].info.loaded && detail_cache[i].info.item.kind == view.details.item.kind &&
            !strcmp(detail_cache[i].info.item.id, job.browse.id) &&
            platform_milliseconds() - detail_cache[i].fetched_ms < 300000) {
            view.details = detail_cache[i].info;
            details = false;
        }
    }
    bool cached = view.authenticated &&
                  (job.browse.kind != BROWSE_PROFILE_PLAYLISTS || view.profile.following_known) &&
                  browse_cache_get(&browse_cache, &job.browse, platform_milliseconds(), &page);

    if (cached) {
        cached = collection_store(&job.browse, &page, false);
        view.list.loading = !cached;
    }

    platform_unlock();

    if (cached) {
        if (details) {
            submit((Job){.action = ACTION_DETAILS, .browse = job.browse});
        }

        ui_wake();
        return true;
    }

    if (submit(job)) {
        if (details) {
            submit((Job){.action = ACTION_DETAILS, .browse = job.browse});
        }

        return true;
    }

    platform_lock();

    if (expected == browse_seq) {
        view.list.loading = false;
        snprintf(view.status, sizeof(view.status), "Could not schedule this collection. Please try again.");
    }

    platform_unlock();
    ui_wake();
    return false;
}

static void reset_session(void) {
    platform_lock();
    ++view.session_revision;
    view.authenticated = false;
    memset(&browse_cache, 0, sizeof(browse_cache));
    memset(detail_cache, 0, sizeof(detail_cache));
    view.details = (CollectionInfo){0};
    view.profile = (ProfileInfo){0};
    sync = (PlayerSync){0};
    seek = (PlayerSeek){0};
    has_deferred = false;
    devices_reset(&view.devices);
    ++view.devices_revision;
    memset(&view.queue, 0, sizeof(view.queue));
    memset(view.library, 0, sizeof(view.library));
    memset(view.library_loaded, 0, sizeof(view.library_loaded));
    memset(view.recent, 0, sizeof(view.recent));
    view.recent_count = 0;
    view.recent_loaded = false;
    view.recent_permission_required = false;
    queue_visible = false;
    memset(&view.list, 0, sizeof(view.list));
    memset(&view.playlists, 0, sizeof(view.playlists));
    ++browse_seq;
    viewport_first = 0;
    library_first = 0;
    view.list.selected = -1;
    view.engine_device_id[0] = 0;
    view.remote_selected = false;
    playback_reset(&view.playback);
    ++view.playback_revision;
    platform_unlock();
}

static void load_library(Spotify *spotify, unsigned session) {
    static const BrowseKind kinds[] = {BROWSE_SAVED_TRACKS, BROWSE_SAVED_ALBUMS, BROWSE_PLAYLISTS};

    for (unsigned i = 0; i < 3; ++i) {
        BrowseRequest request = {.kind = kinds[i]};
        BrowsePage page;
        bool loaded = fetch_page(spotify, &request, &page, false);

        platform_lock();

        if (loaded && session == view.session_revision) {
            store_library(&request, &page);

            if (i == 0 && !view.list.loaded && !view.list.loading) {
                collection_store(&request, &page, false);
            }
        }

        platform_unlock();
        ui_wake();
    }

    submit((Job){.action = ACTION_LIBRARY, .browse = {.kind = BROWSE_PLAYLISTS}});
}

static void session_started(Spotify *spotify, unsigned session) {
    replace_tokens(&spotify->tokens);

    platform_lock();
    bool current = session == view.session_revision;

    if (current) {
        view.authenticated = true;
        view.connecting = false;
        view.status[0] = 0;
    }

    platform_unlock();
    ui_wake();

    if (!current) {
        return;
    }

    submit((Job){.action = ACTION_SYNC});
    submit((Job){.action = ACTION_DEVICES});
    submit((Job){.action = ACTION_HOME});
    engine_start();
    load_library(spotify, session);
}

static void run_session(Worker *worker, Job *job) {
    Spotify *spotify = &worker->spotify;
    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));

    if (job->action == ACTION_DISCONNECT) {
        engine_forget();
        spotify_disconnect(spotify);

        Tokens empty = {0};
        replace_tokens(&empty);
        set_status(job->session, spotify->message);
        return;
    }

    bool ok = job->action == ACTION_RESTORE ? spotify_restore(spotify) : spotify_login(spotify);

    if (ok) {
        session_started(spotify, job->session);
    }

    if (!ok) {
        platform_lock();

        if (job->session == view.session_revision) {
            view.connecting = false;

            // A missing stored session on startup is not an error worth showing.
            if (job->action == ACTION_CONNECT) {
                snprintf(view.status, sizeof(view.status), "%s", spotify->message);
            }
        }

        platform_unlock();
    }

    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));
}

static void publish_details(const Job *job, const CollectionInfo *info) {
    platform_lock();

    if (job->session == view.session_revision && job->browse_seq == browse_seq) {
        view.details = *info;
        unsigned slot = 0;

        for (unsigned i = 0; i < 8; ++i) {
            if (detail_cache[i].info.item.kind == info->item.kind &&
                !strcmp(detail_cache[i].info.item.id, info->item.id)) {
                slot = i;
                break;
            }

            if (detail_cache[i].fetched_ms < detail_cache[slot].fetched_ms) {
                slot = i;
            }
        }

        detail_cache[slot].info = *info;
        detail_cache[slot].fetched_ms = platform_milliseconds();
    }

    platform_unlock();
    ui_wake();
}

static void run_content(Worker *worker, Job *job) {
    Spotify *spotify = &worker->spotify;

    if (job->action == ACTION_DETAILS) {
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
        return;
    }

    if (job->action == ACTION_SAVE_COLLECTION) {
        bool ok = spotify_save_collection(spotify, &job->item, job->value != 0);
        platform_lock();
        bool current = job->session == view.session_revision && job->browse_seq == browse_seq;

        if (current) {
            view.details.saved = ok ? job->value != 0 : job->value == 0;
            view.details.saved_known = ok;
            view.details.saving = false;
            view.details.permission_required = !ok && spotify->last_status == 403;

            if (ok && job->item.kind == ITEM_ARTIST && view.details.followers_known) {
                if (job->value != 0) {
                    ++view.details.followers;
                } else if (view.details.followers > 0) {
                    --view.details.followers;
                }
            }

            if (!ok && !view.details.permission_required) {
                snprintf(view.status, sizeof(view.status), "%s", spotify->message);
            }
        }

        CollectionInfo info = view.details;
        platform_unlock();

        if (current) {
            publish_details(job, &info);
        }

        if (ok && job->item.kind == ITEM_ARTIST) {
            BrowsePage following;
            BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};
            bool followed_ok = fetch_page(spotify, &artists, &following, true);
            platform_lock();

            if (followed_ok && job->session == view.session_revision) {
                memcpy(view.profile.following, following.items,
                       following.count * sizeof(*view.profile.following));
                view.profile.following_count = following.count;
                view.profile.following_total = following.total;
                view.profile.following_known = true;
                view.profile.permission_required = false;
            }

            platform_unlock();
            ui_wake();
        }

        return;
    }

    if (job->action == ACTION_HOME) {
        BrowsePage page;
        bool ok = spotify_recent_albums(spotify, &page);
        platform_lock();
        bool need_profile = !view.profile.loaded;

        if (job->session == view.session_revision) {
            view.recent_loaded = true;
            view.recent_permission_required = !ok && spotify->last_status == 403;

            if (ok) {
                view.recent_count = page.count;
                memcpy(view.recent, page.items, page.count * sizeof(*view.recent));
            }
        }

        platform_unlock();

        if (need_profile) {
            ProfileInfo profile = {0};

            if (spotify_profile(spotify, &profile)) {
                BrowsePage following;
                BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};

                if (fetch_page(spotify, &artists, &following, false)) {
                    memcpy(profile.following, following.items, following.count * sizeof(*profile.following));
                    profile.following_count = following.count;
                    profile.following_total = following.total;
                    profile.following_known = true;
                } else if (spotify->last_status == 403) {
                    profile.permission_required = true;
                }

                platform_lock();

                if (job->session == view.session_revision && !view.profile.loaded) {
                    view.profile = profile;
                }

                platform_unlock();
            }
        }

        return;
    }

    if (job->action == ACTION_QUEUE_PANEL) {
        Queue queue;
        bool ok = spotify_queue(spotify, &queue);
        platform_lock();

        if (ok && job->session == view.session_revision) {
            view.queue = queue;
        }

        platform_unlock();
        return;
    }

    if (job->action == ACTION_LIBRARY) {
        BrowsePage page;
        bool ok = fetch_page(spotify, &job->browse, &page, false);
        platform_lock();
        bool current = job->session == view.session_revision;
        unsigned start = job->browse.offset ? view.playlists.count : 0;
        unsigned needed = ok ? start + page.count : start;

        if (current && ok && needed <= LIST_LIMIT && needed > playlist_capacity) {
            unsigned capacity = needed + 128 < LIST_LIMIT ? needed + 128 : LIST_LIMIT;
            CatalogItem *grown = realloc(playlists, capacity * sizeof(*playlists));
            ok = grown != NULL;

            if (ok) {
                playlists = grown;
                playlist_capacity = capacity;
            }
        }

        if (current && ok && needed <= LIST_LIMIT) {
            if (page.count) {
                memcpy(playlists + start, page.items, page.count * sizeof(*playlists));
            }

            view.playlists.count = needed;
            view.playlists.total = page.total;
            view.playlists.loaded = true;
            view.playlists.more = page.has_next && needed < LIST_LIMIT;
            view.playlists.next_offset = page.offset + page.limit;
        }

        bool more = current && ok && view.playlists.more;
        BrowseRequest next = {.kind = BROWSE_PLAYLISTS, .offset = view.playlists.next_offset};
        view.playlists.loading = more;
        platform_unlock();

        if (more) {
            submit((Job){.action = ACTION_LIBRARY, .browse = next});
        }

        return;
    }

    if (job->action == ACTION_OPEN_LINK) {
        char url[128];

        if (!item_link(&job->item, url, sizeof(url)) || !platform_open_url(url)) {
            set_status(job->session, "Could not open the Spotify link.");
        }

        return;
    }

    if (job->action == ACTION_QUEUE) {
        Queue queue;
        bool ok = spotify_queue(spotify, &queue);

        platform_lock();

        if (ok && job->session == view.session_revision && job->browse_seq == browse_seq) {
            BrowseRequest request = {.kind = BROWSE_QUEUE};
            BrowsePage page = {.total = queue.count};

            view.queue = queue;
            collection_store(&request, &page, false);

            for (unsigned offset = 0; offset < queue.count; offset += BROWSE_PAGE_SIZE) {
                queue_page(&queue, offset, &page);
                page.has_next = false;
                collection_store(&request, &page, offset > 0);
            }
        }

        if (job->session == view.session_revision && job->browse_seq == browse_seq) {
            view.list.loading = false;

            if (!ok) {
                snprintf(view.status, sizeof(view.status), "%s", spotify->message);
            }
        }

        platform_unlock();
        return;
    }

    if (job->action == ACTION_BROWSE || job->action == ACTION_MORE || job->action == ACTION_REFRESH) {
        platform_lock();
        BrowseRequest request = job->browse;
        bool append = job->action == ACTION_MORE;

        if (request.kind == BROWSE_PROFILE_PLAYLISTS && job->action == ACTION_REFRESH) {
            request.owner_id[0] = 0;
        }

        platform_unlock();

        if (request.kind == BROWSE_PROFILE_PLAYLISTS && !append) {
            ProfileInfo profile = {0};

            if (!spotify_profile(spotify, &profile)) {
                platform_lock();

                if (job->session == view.session_revision && job->browse_seq == browse_seq) {
                    view.list.loading = false;
                    snprintf(view.status, sizeof(view.status), "%s", spotify->message);
                }

                platform_unlock();
                return;
            }

            snprintf(request.owner_id, sizeof(request.owner_id), "%s", profile.id);
            BrowsePage following;
            BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};

            if (fetch_page(spotify, &artists, &following, job->action == ACTION_REFRESH)) {
                memcpy(profile.following, following.items, following.count * sizeof(*profile.following));
                profile.following_count = following.count;
                profile.following_total = following.total;
                profile.following_known = true;
            } else if (spotify->last_status == 403) {
                profile.permission_required = true;
            }

            platform_lock();

            if (job->session == view.session_revision && job->browse_seq == browse_seq) {
                view.profile = profile;
            }

            platform_unlock();
            ui_wake();
        }

        BrowsePage page;
        bool ok = fetch_page(spotify, &request, &page, job->action == ACTION_REFRESH);

        platform_lock();

        // Drop results for a list the listener has already left.
        bool current = job->session == view.session_revision && job->browse_seq == browse_seq;

        if (ok && current) {
            collection_store(&request, &page, append);
            store_library(&request, &page);
            view.status[0] = 0;
        }

        if (current) {
            view.list.loading = false;
        }

        if (!ok && current) {
            snprintf(view.status, sizeof(view.status), "%s", spotify->message);
        }

        platform_unlock();
    }
}

static Playback engine_target(void) {
    Playback target;
    playback_reset(&target);
    target.has_device = true;
    snprintf(target.device_id, sizeof(target.device_id), "%s", view.engine_device_id);
    return target;
}

static bool engine_ready(void) {
    return view.engine == ENGINE_READY && view.engine_device_id[0];
}

static void run_player(Worker *worker, Job *job) {
    Spotify *spotify = &worker->spotify;
    bool accepted = false;

    if (job->action == ACTION_SYNC) {
        Playback playback;
        bool ok = spotify_playback(spotify, &playback);
        uint64_t now = platform_milliseconds();
        bool check_radio = false;

        platform_lock();

        // A command issued after this read started makes the read stale; keep the optimistic state.
        bool fresh = job->session == view.session_revision && job->seq == command_seq && !command_pending();

        if (ok && fresh &&
            player_seek_reconcile(&seek, &view.playback, view.playback_received_ms, &playback, now)) {
            view.playback = playback;
            view.playback_received_ms = now;
            ++view.playback_revision;
            check_radio = playback.playing && playback.repeat == REPEAT_OFF && playback.duration_ms > 0 &&
                          playback.position_known && playback.duration_ms - playback.position_ms < 20000 &&
                          !strncmp(playback.uri, "spotify:track:", 14) &&
                          strcmp(playback.uri, radio_guarded_uri) != 0;
        }

        player_sync_received(&sync, now, ok, view.playback.playing, spotify->retry_at * 1000);

        if (ok && (!fresh || seek.pending)) {
            sync.next_ms = now + 300;
        }
        platform_unlock();

        if (check_radio) {
            Queue upcoming = {0};

            if (spotify_queue(spotify, &upcoming) && upcoming.count == 0) {
                CatalogItem seed = {.kind = ITEM_TRACK, .playable = true};
                copy_utf8(seed.id, sizeof(seed.id), playback.uri + 14);
                copy_utf8(seed.title, sizeof(seed.title), playback.title);
                copy_utf8(seed.detail, sizeof(seed.detail), playback.artist);

                if (spotify_play_radio(spotify, &playback, &seed, false)) {
                    platform_lock();
                    copy_utf8(radio_guarded_uri, sizeof(radio_guarded_uri), playback.uri);
                    platform_unlock();
                }
            }
        }

        return;
    }

    if (job->action == ACTION_DEVICES) {
        Devices devices;
        devices_reset(&devices);

        platform_lock();
        Devices previous = view.devices;
        platform_unlock();

        devices.selected = previous.selected;
        bool ok = spotify_devices(spotify, &devices);
        int engine = ok ? engine_find_device(&devices) : -1;

        platform_lock();

        if (ok && job->session == view.session_revision) {
            view.devices = devices;
            ++view.devices_revision;
            snprintf(view.engine_device_id, sizeof(view.engine_device_id), "%s",
                     engine >= 0 ? devices.items[engine].id : "");
        }

        platform_unlock();

        if (ok) {
            engine_device_seen(engine >= 0);
        }

        return;
    }

    platform_lock();
    Playback target = job->target;
    bool local = !view.remote_selected && engine_ready();

    // New playback goes to this computer unless the listener chose a remote device.
    if ((job->action == ACTION_PLAY_ITEM || job->action == ACTION_RADIO) && local) {
        target = engine_target();
    }

    Device engine = {.supports_volume = true};
    snprintf(engine.id, sizeof(engine.id), "%s", view.engine_device_id);
    platform_unlock();

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
            view.remote_selected = strcmp(job->device.id, view.engine_device_id) != 0;
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
    player_sync_requested(&sync, platform_milliseconds());

    if (job->action == ACTION_SEEK) {
        player_seek_completed(&seek, job->seq, platform_milliseconds(), accepted);
    }

    if (job->session == view.session_revision) {
        snprintf(view.status, sizeof(view.status), "%s", accepted ? "" : spotify->message);
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
    bool current = job->session == view.session_revision &&
                   ((job->action != ACTION_DETAILS && job->action != ACTION_SAVE_COLLECTION) ||
                    job->browse_seq == browse_seq);
    platform_unlock();

    if (!current || !signed_in()) {
        platform_lock();

        if (current && lane_for(job->action) == LANE_CONTENT) {
            view.list.loading = false;
            snprintf(view.status, sizeof(view.status), "Connect your Spotify account first.");
        }

        platform_unlock();
        return;
    }

    lane_begin(spotify);

    if (lane_for(job->action) != LANE_CONTENT) {
        run_player(worker, job);
    }

    if (lane_for(job->action) == LANE_CONTENT) {
        run_content(worker, job);
    }

    lane_end(spotify);
}

static void login_progress(const char *message) {
    platform_lock();
    snprintf(view.status, sizeof(view.status), "%s", message);
    platform_unlock();
    ui_wake();
}

static void engine_changed(EngineState state, const char *message) {
    platform_lock();
    view.engine = state;
    snprintf(view.engine_message, sizeof(view.engine_message), "%s", message);

    if (state == ENGINE_STARTING || state == ENGINE_PAIRING) {
        next_devices_ms = 0;
    }

    bool resume = state == ENGINE_READY && has_deferred;
    Job job = deferred;

    if (resume) {
        has_deferred = false;
        view.status[0] = 0;
    }

    // Playback was waiting on Soloist but it needs the listener: show the setup panel.
    if ((state == ENGINE_FAILED || state == ENGINE_NEEDS_KEY) && has_deferred) {
        has_deferred = false;
        view.status[0] = 0;
        ++view.setup_revision;
    }

    platform_unlock();

    if (resume) {
        submit(job);
    }

    ui_wake();
}

void app_initialize(void) {
    for (unsigned i = 0; i < LANE_COUNT; ++i) {
        snprintf(workers[i].spotify.client_id, sizeof(workers[i].spotify.client_id), "%s",
                 NATIVE_SPOTIFY_CLIENT_ID);
        workers[i].spotify.notify = login_progress;
    }

    snprintf(view.status, sizeof(view.status), "Connect to Spotify to get started.");
    view.playback.volume_percent = -1;
    view.playback_revision = 1;
    view.devices_revision = 1;
    view.list.selected = -1;
    devices_reset(&view.devices);
    engine_configure(engine_changed);
}

void app_shutdown(void) {
    platform_lock();
    shutting_down = true;
    platform_unlock();
    engine_stop();
}

/* Applies a command's expected result immediately; the confirming read reconciles it. */
static void optimistic(const Job *job) {
    uint64_t now = platform_milliseconds();
    Playback *playback = &view.playback;
    playback->position_ms = player_position(playback, now - view.playback_received_ms);

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

    view.playback_received_ms = now;
    ++view.playback_revision;
    ++command_seq;

    if (job->action == ACTION_SEEK) {
        player_seek_requested(&seek, command_seq, now);
    }

    if (job->action == ACTION_NEXT || job->action == ACTION_PREVIOUS || job->action == ACTION_PLAY_ITEM) {
        seek.pending = false;
    }
}

static bool command(Job job) {
    platform_lock();
    bool ready = view.authenticated;

    if (ready) {
        optimistic(&job);
    }

    platform_unlock();

    if (!ready) {
        platform_lock();
        snprintf(view.status, sizeof(view.status), "Connect your Spotify account first.");
        platform_unlock();
        ui_wake();
        return false;
    }

    ui_wake();

    if (submit(job)) {
        return true;
    }

    platform_lock();
    seek.pending = false;
    player_sync_requested(&sync, platform_milliseconds());
    snprintf(view.status, sizeof(view.status), "Could not schedule playback control. Please try again.");
    platform_unlock();
    ui_wake();
    return false;
}

void app_action(Action action) {
    if (action == ACTION_RESTORE && getenv("NATIVE_CONTROLLER_OFFLINE")) {
        return;
    }

    if (action == ACTION_RESTORE || action == ACTION_CONNECT || action == ACTION_DISCONNECT) {
        reset_session();
        platform_lock();
        view.connecting = action != ACTION_DISCONNECT;
        snprintf(view.status, sizeof(view.status), "%s",
                 action == ACTION_CONNECT ? "Finish login in your browser. Waiting up to 120 seconds…" : "");
        platform_unlock();
        ui_wake();
        submit((Job){.action = action});
        return;
    }

    platform_lock();
    Job job = {.action = action, .target = view.playback, .browse = view.list.request};
    job.value = action == ACTION_SHUFFLE  ? !view.playback.shuffle
                : action == ACTION_REPEAT ? ((unsigned)view.playback.repeat + 1) % 3
                                          : 0;
    bool listed = view.list.loaded && view.list.request.kind == BROWSE_QUEUE;
    bool loaded = view.list.loaded;
    platform_unlock();

    platform_lock();
    bool needs_setup = action == ACTION_PLAY && !job.target.has_device &&
                       (view.engine == ENGINE_NEEDS_KEY || view.engine == ENGINE_FAILED);

    if (needs_setup) {
        ++view.setup_revision;
    }

    platform_unlock();

    if (needs_setup) {
        ui_wake();
        return;
    }

    if (player_command(action)) {
        command(job);
        return;
    }

    if (action == ACTION_QUEUE) {
        platform_lock();
        ++browse_seq;
        view.list = (ListView){.request = {.kind = BROWSE_QUEUE}, .selected = -1};
        viewport_first = 0;
        view.list.loading = true;
        platform_unlock();
    }

    if (action == ACTION_REFRESH) {
        submit((Job){.action = ACTION_SYNC});
        submit((Job){.action = ACTION_DEVICES});

        if (job.browse.kind == BROWSE_ALBUM_TRACKS || job.browse.kind == BROWSE_ARTIST_ALBUMS) {
            submit((Job){.action = ACTION_DETAILS, .browse = job.browse});
        }

        if (listed) {
            submit((Job){.action = ACTION_QUEUE});
            return;
        }

        if (!loaded) {
            return;
        }
    }

    submit(job);
}

bool app_connect(void) {
    app_action(ACTION_CONNECT);
    return true;
}

int app_tick(bool visible) {
    platform_lock();

    if (!visible || !view.authenticated) {
        platform_unlock();
        return -1;
    }

    uint64_t now = platform_milliseconds();
    bool poll = now >= sync.next_ms;
    bool queue = poll && view.list.loaded && view.list.request.kind == BROWSE_QUEUE && !view.list.loading;
    bool panel_queue = poll && queue_visible;
    bool paint = view.playback.playing && now >= next_paint_ms;
    bool waiting = view.engine == ENGINE_STARTING || view.engine == ENGINE_PAIRING;
    bool devices = waiting && now >= next_devices_ms;

    if (poll) {
        // Hold off further polls until this one completes and reschedules.
        sync.next_ms = now + 30000;
    }

    if (paint) {
        next_paint_ms = now + 1000;
    }

    if (devices) {
        next_devices_ms = now + 3000;
    }

    uint64_t deadline = sync.next_ms;

    if (view.playback.playing && next_paint_ms < deadline) {
        deadline = next_paint_ms;
    }

    if (waiting && next_devices_ms < deadline) {
        deadline = next_devices_ms;
    }

    platform_unlock();

    if (poll) {
        submit((Job){.action = ACTION_SYNC});
    }

    if (devices) {
        submit((Job){.action = ACTION_DEVICES});
    }

    if (queue) {
        submit((Job){.action = ACTION_QUEUE});
    }

    if (panel_queue) {
        submit((Job){.action = ACTION_QUEUE_PANEL});
    }

    if (paint) {
        ui_wake();
    }

    return deadline > now ? (int)(deadline - now) : 1;
}

static bool browse(BrowseKind kind, const char *query, SearchFilter filter) {
    Job job = {.action = ACTION_BROWSE, .browse = {.kind = kind, .filter = filter}};
    char url[1200];

    bool valid = (!query || search_append(job.browse.query, query, strlen(query))) &&
                 browse_url(&job.browse, url, sizeof(url));

    if (!valid) {
        platform_lock();
        snprintf(view.status, sizeof(view.status), "Enter a search of at most 255 characters.");
        platform_unlock();
        ui_wake();
        return false;
    }

    return begin_browse(job);
}

bool app_browse(BrowseKind kind, const char *query) {
    return browse(kind, query, SEARCH_ALL);
}

bool app_search(const char *query, SearchFilter filter) {
    return browse(BROWSE_SEARCH, query, filter);
}

bool app_load_more(void) {
    platform_lock();
    Job job = {.action = ACTION_MORE, .browse = view.list.request};
    job.browse.offset = view.list.next_offset;
    snprintf(job.browse.after, sizeof(job.browse.after), "%s", view.list.next_after);
    bool ok = view.authenticated && view.list.loaded && view.list.more && !view.list.loading &&
              view.list.request.kind != BROWSE_QUEUE;

    if (ok) {
        view.list.loading = true;
    }

    platform_unlock();

    if (ok && !submit(job)) {
        platform_lock();
        view.list.loading = false;
        platform_unlock();
        return false;
    }

    return ok;
}

void app_viewport(unsigned first) {
    platform_lock();
    viewport_first = first;
    platform_unlock();
}

void app_library_viewport(unsigned first) {
    platform_lock();
    library_first = first;
    platform_unlock();
}

void app_queue_visible(bool visible) {
    platform_lock();
    queue_visible = visible;
    platform_unlock();
}

bool app_select(int index) {
    platform_lock();
    bool ok = index >= -1 && (index < 0 || (unsigned)index < view.list.count);

    if (ok) {
        view.list.selected = index;

        if (index >= 0 && ((unsigned)index < viewport_first || (unsigned)index >= viewport_first + 24)) {
            viewport_first = (unsigned)index;
        }
    }

    platform_unlock();

    if (ok) {
        ui_wake();
    }

    return ok;
}

static bool selected_item(CatalogItem *item) {
    platform_lock();
    bool ok = view.list.selected >= 0 && (unsigned)view.list.selected < view.list.count;

    if (ok) {
        *item = collection[view.list.selected];
    }

    platform_unlock();
    return ok;
}

bool app_activate(void) {
    CatalogItem item;
    return selected_item(&item) && app_open_item(&item);
}

static bool play_item(const CatalogItem *item) {
    platform_lock();
    Job job = {
        .action = ACTION_PLAY_ITEM, .item = *item, .target = view.playback, .browse = view.list.request};
    bool local = !view.remote_selected && engine_ready();
    bool remote = view.remote_selected && view.playback.has_device;
    bool preparing = view.engine == ENGINE_DOWNLOADING || view.engine == ENGINE_STARTING ||
                     view.engine == ENGINE_PAIRING || (view.engine == ENGINE_OFF && view.authenticated);

    // Soloist is still getting ready: remember the choice and play it the moment it is.
    if (!local && !remote && preparing) {
        deferred = job;
        has_deferred = true;
        optimistic(&job);
        view.playback.playing = false;
        snprintf(view.status, sizeof(view.status), "%s",
                 view.engine_message[0] ? view.engine_message : "Getting playback ready on this computer…");
    }

    platform_unlock();

    if (!local && !remote && preparing) {
        ui_wake();
        return true;
    }

    if (!local && !remote && !job.target.has_device) {
        platform_lock();
        ++view.setup_revision;
        platform_unlock();
        ui_wake();
        return false;
    }

    return command(job);
}

bool app_open_item(const CatalogItem *item) {
    Job job = {.action = ACTION_BROWSE, .item = *item};

    if (item_browse_request(item, &job.browse)) {
        return begin_browse(job);
    }

    return item_can_play(item) && play_item(item);
}

bool app_play_collection(void) {
    platform_lock();
    CatalogItem item = view.details.item;
    bool valid =
        (view.list.request.kind == BROWSE_ALBUM_TRACKS || view.list.request.kind == BROWSE_ARTIST_ALBUMS) &&
        spotify_id_valid(item.id);
    platform_unlock();
    return valid && play_item(&item);
}

bool app_play_popular(unsigned index) {
    platform_lock();
    bool valid = index < view.details.popular_count;
    CatalogItem item = valid ? view.details.popular[index] : (CatalogItem){0};
    platform_unlock();
    return valid && item_can_play(&item) && play_item(&item);
}

bool app_toggle_collection_saved(void) {
    platform_lock();
    bool valid = view.authenticated && view.details.loaded && !view.details.saving;
    bool authorize = valid && view.details.permission_required;
    Job job = {.action = ACTION_SAVE_COLLECTION, .item = view.details.item, .value = !view.details.saved};

    if (valid && !authorize) {
        view.details.saving = true;
        view.details.saved = job.value != 0;
    }

    platform_unlock();

    if (authorize) {
        return app_connect();
    }

    if (!valid) {
        return false;
    }

    if (submit(job)) {
        ui_wake();
        return true;
    }

    platform_lock();
    view.details.saving = false;
    view.details.saved = job.value == 0;
    platform_unlock();
    return false;
}

bool app_open_link(void) {
    CatalogItem item;
    return selected_item(&item) && submit((Job){.action = ACTION_OPEN_LINK, .item = item});
}

bool app_open_item_link(const CatalogItem *item) {
    return item && spotify_id_valid(item->id) && submit((Job){.action = ACTION_OPEN_LINK, .item = *item});
}

bool app_enqueue_item(const CatalogItem *item) {
    if (!item || !item_can_play(item)) {
        return false;
    }

    platform_lock();
    Job job = {.action = ACTION_ENQUEUE, .item = *item, .target = view.playback};
    platform_unlock();
    return command(job);
}

bool app_start_radio(const CatalogItem *item) {
    if (!item || (!spotify_id_valid(item->id) && !item->title[0] && !item->detail[0])) {
        return false;
    }

    platform_lock();
    Job job = {.action = ACTION_RADIO, .item = *item, .target = view.playback};
    bool local = !view.remote_selected && engine_ready();
    bool remote = view.remote_selected && view.playback.has_device;
    platform_unlock();

    if (!local && !remote && !job.target.has_device) {
        platform_lock();
        ++view.setup_revision;
        platform_unlock();
        ui_wake();
        return false;
    }

    return command(job);
}

bool app_open_collection_link(void) {
    platform_lock();
    CatalogItem item = view.details.item;
    bool valid = spotify_id_valid(item.id);
    platform_unlock();
    return valid && submit((Job){.action = ACTION_OPEN_LINK, .item = item});
}

bool app_filter_discography(const char *group) {
    platform_lock();
    Job job = {.action = ACTION_BROWSE, .browse = view.list.request};
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

    return begin_browse(job);
}

bool app_select_device(int index) {
    platform_lock();
    bool ok = devices_select(&view.devices, index);
    platform_unlock();

    if (ok) {
        ui_wake();
    }

    return ok;
}

bool app_transfer(void) {
    platform_lock();
    const Device *device = devices_selected(&view.devices);
    bool ok = device_can_transfer(device);
    Job job = {.action = ACTION_TRANSFER, .target = view.playback};

    if (ok) {
        job.device = *device;
    }

    platform_unlock();
    return ok && submit(job);
}

bool app_seek(unsigned fraction) {
    platform_lock();
    Playback playback = view.playback;
    platform_unlock();

    unsigned position;

    if (!player_seek_position(&playback, fraction, &position)) {
        return false;
    }

    return command((Job){.action = ACTION_SEEK, .target = playback, .value = position});
}

bool app_volume(unsigned percent) {
    platform_lock();
    Playback playback = view.playback;
    platform_unlock();

    if (percent > 100 || !playback_can_volume(&playback)) {
        return false;
    }

    return command((Job){.action = ACTION_VOLUME, .target = playback, .value = percent});
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
        snprintf(view.engine_message, sizeof(view.engine_message),
                 "Could not save the key in your system credential store. Unlock it and try again.");
        view.engine = ENGINE_FAILED;
        platform_unlock();
        ui_wake();
    }
}

bool app_engine_key(const char *text, size_t size) {
    char key[ENGINE_KEY_CAPACITY];
    bool valid = engine_key_normalize(text, size, key, sizeof(key));
    PendingKey *pending = valid ? calloc(1, sizeof(*pending)) : NULL;

    if (pending) {
        memcpy(pending->key, key, strlen(key));
        pending->size = strlen(key);
    }

    platform_wipe(key, sizeof(key));

    platform_lock();

    if (!valid) {
        snprintf(view.engine_message, sizeof(view.engine_message),
                 "The clipboard does not contain a Soloist key. Copy the key, then press Paste key again.");
    }

    if (pending) {
        view.engine = ENGINE_STARTING;
        snprintf(view.engine_message, sizeof(view.engine_message), "Saving your key…");
    }

    platform_unlock();
    ui_wake();

    // The credential store can block (e.g. an unlock prompt), so save off the UI thread.
    if (pending && !platform_start_thread(save_key, pending)) {
        platform_wipe(pending, sizeof(*pending));
        free(pending);
        return false;
    }

    return pending != NULL;
}

void app_engine_dashboard(void) {
    platform_open_url(ENGINE_DASHBOARD_URL);
}

static void forget_key(void *argument) {
    (void)argument;
    engine_forget();
    engine_start();
}

void app_engine_forget(void) {
    platform_start_thread(forget_key, NULL);
}

View app_snapshot(void) {
    platform_lock();
    View snapshot = view;
    unsigned first = viewport_first > 16 ? viewport_first - 16 : 0;

    if (first > snapshot.list.count) {
        first = snapshot.list.count;
    }

    unsigned rows = snapshot.list.count - first < LIST_WINDOW ? snapshot.list.count - first : LIST_WINDOW;

    if (rows) {
        memcpy(snapshot.list.rows, collection + first, rows * sizeof(*collection));
    }

    snapshot.list.first = first;
    snapshot.list.rows_count = rows;

    unsigned library_start = library_first > 16 ? library_first - 16 : 0;
    if (library_start > view.playlists.count) {
        library_start = view.playlists.count;
    }
    unsigned library_rows = view.playlists.count - library_start;

    if (library_rows > LIST_WINDOW) {
        library_rows = LIST_WINDOW;
    }

    if (library_rows) {
        memcpy(snapshot.playlists.rows, playlists + library_start, library_rows * sizeof(*playlists));
    }

    snapshot.playlists.first = library_start;
    snapshot.playlists.rows_count = library_rows;
    platform_unlock();

    uint64_t now = platform_milliseconds();
    snapshot.playback.position_ms = player_position(
        &snapshot.playback, now >= snapshot.playback_received_ms ? now - snapshot.playback_received_ms : 0);

    return snapshot;
}
