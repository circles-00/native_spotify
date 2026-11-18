#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Collection: every loaded row of the open list. Called with the view lock held. */
bool app_collection_store(const BrowseRequest *request, const BrowsePage *page, bool append) {
    unsigned start = append ? app_state.view.list.count : 0;

    if (append && page->offset != app_state.view.list.next_offset) {
        return false;
    }

    unsigned needed = start + page->count;

    if (needed > LIST_LIMIT) {
        needed = LIST_LIMIT;
    }

    if (needed > app_state.collection_capacity) {
        unsigned capacity = app_state.collection_capacity ? app_state.collection_capacity * 2 : 256;

        while (capacity < needed) {
            capacity *= 2;
        }

        CatalogItem *grown =
            realloc(app_state.collection, (capacity > LIST_LIMIT ? LIST_LIMIT : capacity) * sizeof(*grown));

        if (!grown) {
            return false;
        }

        app_state.collection = grown;
        app_state.collection_capacity = capacity > LIST_LIMIT ? LIST_LIMIT : capacity;
    }

    if (needed > start) {
        memcpy(app_state.collection + start, page->items, (needed - start) * sizeof(*app_state.collection));
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
            app_state.view.search_artist_count = page->artist_count;
            app_state.view.search_album_count = page->album_count;
        }
    }

    app_state.view.list.count = needed;

    for (unsigned i = 0; i < page->count; ++i) {
        app_state.view.list.duration_ms += page->items[i].duration_ms;
        app_state.view.list.duration_known =
            app_state.view.list.duration_known && page->items[i].duration_known;

        if (page->items[i].disc_number > app_state.view.list.max_disc) {
            app_state.view.list.max_disc = page->items[i].disc_number;
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
    return true;
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

bool app_fetch_page(Spotify *spotify, const BrowseRequest *request, BrowsePage *page, bool fresh) {
    platform_lock();
    bool cached = !fresh && browse_cache_get(&app_state.browse_cache, request, platform_milliseconds(), page);
    platform_unlock();

    if (cached) {
        return true;
    }

    if (!spotify_browse(spotify, request, page)) {
        return false;
    }

    platform_lock();
    browse_cache_store(&app_state.browse_cache, request, page, platform_milliseconds());
    platform_unlock();
    return true;
}

bool app_begin_browse(Job job) {
    BrowsePage page;
    platform_lock();

    if (job.browse.kind == BROWSE_PROFILE_PLAYLISTS && app_state.view.profile.loaded) {
        snprintf(job.browse.owner_id, sizeof(job.browse.owner_id), "%s", app_state.view.profile.id);
    }

    unsigned expected = ++app_state.browse_seq;
    unsigned revision = app_state.view.list.revision + 1;
    app_state.view.list =
        (ListView){.request = job.browse, .selected = -1, .revision = revision, .loading = true};
    app_state.viewport_first = 0;
    app_state.view.search_artist_count = 0;
    app_state.view.search_album_count = 0;
    app_state.view.status[0] = 0;
    app_state.view.details = (CollectionInfo){0};
    bool details = job.browse.kind == BROWSE_ALBUM_TRACKS || job.browse.kind == BROWSE_ARTIST_ALBUMS;
    app_state.view.details.item.kind = job.browse.kind == BROWSE_ARTIST_ALBUMS ? ITEM_ARTIST : ITEM_ALBUM;
    snprintf(app_state.view.details.item.id, sizeof(app_state.view.details.item.id), "%s", job.browse.id);
    copy_utf8(app_state.view.details.item.title, sizeof(app_state.view.details.item.title), job.browse.title);
    copy_utf8(app_state.view.details.item.artwork, sizeof(app_state.view.details.item.artwork),
              job.browse.artwork);

    for (unsigned i = 0; details && i < 8; ++i) {
        if (app_state.detail_cache[i].info.loaded &&
            app_state.detail_cache[i].info.item.kind == app_state.view.details.item.kind &&
            !strcmp(app_state.detail_cache[i].info.item.id, job.browse.id) &&
            platform_milliseconds() - app_state.detail_cache[i].fetched_ms < 300000) {
            app_state.view.details = app_state.detail_cache[i].info;
            details = false;
        }
    }

    bool cached = app_state.view.authenticated &&
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

bool app_search(const char *query, SearchFilter filter) {
    return browse(BROWSE_SEARCH, query, filter);
}

bool app_load_more(void) {
    platform_lock();
    Job job = {.action = ACTION_MORE, .browse = app_state.view.list.request};
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
        app_state.view.list.loading = false;
        platform_unlock();
        return false;
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

void app_load_library(Spotify *spotify, unsigned session) {
    static const BrowseKind kinds[] = {BROWSE_SAVED_TRACKS, BROWSE_SAVED_ALBUMS, BROWSE_PLAYLISTS};

    for (unsigned i = 0; i < 3; ++i) {
        BrowseRequest request = {.kind = kinds[i]};
        BrowsePage page;
        bool loaded = app_fetch_page(spotify, &request, &page, false);

        platform_lock();

        if (loaded && session == app_state.view.session_revision) {
            app_store_library(&request, &page);

            if (i == 0 && !app_state.view.list.loaded && !app_state.view.list.loading) {
                app_collection_store(&request, &page, false);
            }
        }

        platform_unlock();
        app_wake();
    }

    app_submit((Job){.action = ACTION_LIBRARY, .browse = {.kind = BROWSE_PLAYLISTS}});
}

static void publish_details(const Job *job, const CollectionInfo *info) {
    platform_lock();

    if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
        app_state.view.details = *info;
        unsigned slot = 0;

        for (unsigned i = 0; i < 8; ++i) {
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
        bool followed_ok = app_fetch_page(spotify, &artists, &following, true);
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
        return;
    }

    ProfileInfo profile = {0};

    if (!spotify_profile(spotify, &profile)) {
        return;
    }

    BrowsePage following;
    BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};

    if (app_fetch_page(spotify, &artists, &following, false)) {
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

    platform_unlock();
}

static void handle_library(Spotify *spotify, const Job *job) {
    BrowsePage page;
    bool ok = app_fetch_page(spotify, &job->browse, &page, false);
    platform_lock();
    bool current = job->session == app_state.view.session_revision;
    unsigned start = job->browse.offset ? app_state.view.playlists.count : 0;
    unsigned needed = ok ? start + page.count : start;

    if (current && ok && needed <= LIST_LIMIT && needed > app_state.playlist_capacity) {
        unsigned capacity = needed + 128 < LIST_LIMIT ? needed + 128 : LIST_LIMIT;
        CatalogItem *grown = realloc(app_state.playlists, capacity * sizeof(*app_state.playlists));
        ok = grown != NULL;

        if (ok) {
            app_state.playlists = grown;
            app_state.playlist_capacity = capacity;
        }
    }

    if (current && ok && needed <= LIST_LIMIT) {
        if (page.count) {
            memcpy(app_state.playlists + start, page.items, page.count * sizeof(*app_state.playlists));
        }

        app_state.view.playlists.count = needed;
        app_state.view.playlists.total = page.total;
        app_state.view.playlists.loaded = true;
        app_state.view.playlists.more = page.has_next && needed < LIST_LIMIT;
        app_state.view.playlists.next_offset = page.offset + page.limit;
    }

    bool more = current && ok && app_state.view.playlists.more;
    BrowseRequest next = {.kind = BROWSE_PLAYLISTS, .offset = app_state.view.playlists.next_offset};
    app_state.view.playlists.loading = more;
    platform_unlock();

    if (more) {
        app_submit((Job){.action = ACTION_LIBRARY, .browse = next});
    }
}

static void handle_queue(Spotify *spotify, const Job *job) {
    Queue queue;
    bool ok = spotify_queue(spotify, &queue);

    platform_lock();

    if (ok && job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
        BrowseRequest request = {.kind = BROWSE_QUEUE};
        BrowsePage page = {.total = queue.count};

        app_state.view.queue = queue;
        app_collection_store(&request, &page, false);

        for (unsigned offset = 0; offset < queue.count; offset += BROWSE_PAGE_SIZE) {
            queue_page(&queue, offset, &page);
            page.has_next = false;
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

static void handle_browse(Spotify *spotify, const Job *job) {
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

            if (job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq) {
                app_state.view.list.loading = false;
                snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", spotify->message);
            }

            platform_unlock();
            return;
        }

        snprintf(request.owner_id, sizeof(request.owner_id), "%s", profile.id);
        BrowsePage following;
        BrowseRequest artists = {.kind = BROWSE_FOLLOWED_ARTISTS};

        if (app_fetch_page(spotify, &artists, &following, job->action == ACTION_REFRESH)) {
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
    bool ok = app_fetch_page(spotify, &request, &page, job->action == ACTION_REFRESH);

    platform_lock();

    // Drop results for a list the listener has already left.
    bool current = job->session == app_state.view.session_revision && job->browse_seq == app_state.browse_seq;

    if (ok && current) {
        app_collection_store(&request, &page, append);
        app_store_library(&request, &page);
        app_state.view.status[0] = 0;
    }

    if (current) {
        app_state.view.list.loading = false;
    }

    if (!ok && current) {
        snprintf(app_state.view.status, sizeof(app_state.view.status), "%s", spotify->message);
    }

    platform_unlock();
}

void app_run_content(Spotify *spotify, const Job *job) {
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

        if (ok && job->session == app_state.view.session_revision) {
            app_state.view.queue = queue;
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
