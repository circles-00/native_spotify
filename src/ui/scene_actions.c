#include "scene_internal.h"
#include "panel_scene.h"
#include "search_scene.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

static void record_route(Scene *scene, const SceneRoute *route) {
    if (scene->restoring_route) {
        return;
    }

    if (!scene->history_count) {
        scene->history[0] = (SceneRoute){.home = true};
        scene->history_count = 1;
    }

    const SceneRoute *current = &scene->history[scene->history_index];

    if (current->home == route->home &&
        (route->home ||
         (current->kind == route->kind && current->filter == route->filter &&
          !strcmp(current->query, route->query) && !strcmp(current->item.id, route->item.id)))) {
        return;
    }

    if (scene->history_index + 1 == SCENE_HISTORY_LIMIT) {
        memmove(scene->history, scene->history + 1, (SCENE_HISTORY_LIMIT - 1) * sizeof(*scene->history));
        --scene->history_index;
    }

    scene->history[++scene->history_index] = *route;
    scene->history_count = scene->history_index + 1;
}

void scene_open_section(Scene *scene, BrowseKind kind) {
    scene->search_due = 0;
    scene->search_history_open = false;

    if (kind != BROWSE_QUEUE && !app_browse(kind, NULL)) {
        return;
    }

    if (kind == BROWSE_QUEUE) {
        app_action(ACTION_QUEUE);
    }

    record_route(scene, &(SceneRoute){.kind = kind});
    scene->home = false;
    scene->last_click = HIT_NONE;
    scene->section = kind;
    scene->collection_artwork[0] = 0;
    scene->pending_browse = true;
    scene->first_row = 0;
    scene->main_scroll = (Scroll){0};
    scene->popular_expanded = false;
    scene->popular_selected = 0;
    scene->entrance = 1;
    scene->retain_content = false;
    app_viewport(0);
}

void scene_search(Scene *scene) {
    scene->search_due = 0;

    if (!app_search(scene->query, scene->search_filter)) {
        return;
    }

    SceneRoute route = {.kind = BROWSE_SEARCH, .filter = scene->search_filter};
    copy_utf8(route.query, sizeof(route.query), scene->query);
    record_route(scene, &route);
    scene->home = false;
    scene->section = BROWSE_SEARCH;
    scene->collection_artwork[0] = 0;
    scene->pending_browse = true;
    scene->first_row = 0;
    scene->search_scroll = 0;
    scene->main_scroll = (Scroll){0};
    scene->entrance = 1;
    scene->retain_content = false;
    app_viewport(0);
}

void scene_open_collection(Scene *scene, const CatalogItem *item) {
    scene->search_due = 0;
    scene->search_history_open = false;
    BrowseRequest request;

    if (!item_browse_request(item, &request) || !app_open_item(item)) {
        return;
    }

    record_route(scene, &(SceneRoute){.kind = request.kind, .item = *item});
    snprintf(scene->collection, sizeof(scene->collection), "%s", item->title);
    snprintf(scene->collection_artwork, sizeof(scene->collection_artwork), "%s", item->artwork);
    scene->section = request.kind;
    scene->home = false;
    scene->pending_browse = true;
    scene->entrance = 1;
    scene->retain_content = false;
    app_viewport(0);
    scene->first_row = 0;
    scene->main_scroll = (Scroll){0};
    scene->popular_expanded = false;
    scene->popular_selected = 0;
    scene->last_click = HIT_NONE;
}

static void open_artist_likes(Scene *scene, const CatalogItem *artist) {
    if (!app_browse_artist_likes(artist)) {
        return;
    }

    record_route(scene, &(SceneRoute){.kind = BROWSE_ARTIST_LIKES, .item = *artist});
    scene->section = BROWSE_ARTIST_LIKES;
    scene->home = false;
    scene->pending_browse = true;
    scene->first_row = 0;
    scene->main_scroll = (Scroll){0};
    scene->search_due = 0;
    scene->search_history_open = false;
    scene->retain_content = false;
    scene->entrance = 1;
}

static void scene_open_settings(Scene *scene) {
    scene->search_due = 0;
    scene->search_history_open = false;
    record_route(scene, &(SceneRoute){.kind = BROWSE_SETTINGS});
    scene->home = false;
    scene->last_click = HIT_NONE;
    scene->section = BROWSE_SETTINGS;
    scene->collection_artwork[0] = 0;
    scene->pending_browse = false;
    scene->first_row = 0;
    scene->main_scroll = (Scroll){0};
    scene->main_origin = 0;
    scene->main_row_height = 0;
    scene->main_columns = 0;
    scene->popular_expanded = false;
    scene->popular_selected = 0;
    scene->entrance = 1;
    scene->retain_content = false;
}

static void menu_add(ContextMenu *menu, ContextAction action, const char *label) {
    if (menu->count >= 6) {
        return;
    }

    menu->actions[menu->count] = action;
    copy_utf8(menu->labels[menu->count], sizeof(menu->labels[menu->count]), label);
    ++menu->count;
}

static void open_song_radio(Scene *scene, const CatalogItem *song) {
    if (!app_browse_song_radio(song)) {
        return;
    }

    record_route(scene, &(SceneRoute){.kind = BROWSE_SONG_RADIO, .item = *song});
    scene->section = BROWSE_SONG_RADIO;
    scene->home = false;
    scene->pending_browse = true;
    scene->first_row = 0;
    scene->main_scroll = (Scroll){0};
    scene->search_due = 0;
    scene->search_history_open = false;
    scene->last_click = HIT_NONE;
    scene->retain_content = false;
    scene->entrance = 1;
    if (!strncmp(song->title, "Daily Mix ", 10)) {
        copy_utf8(scene->collection, sizeof(scene->collection), song->title);
    } else {
        copy_utf8(scene->collection, sizeof(scene->collection) - sizeof(" Radio") + 1,
                  song->title[0] ? song->title : "Song");
        strcat(scene->collection, " Radio");
    }
    copy_utf8(scene->collection_artwork, sizeof(scene->collection_artwork), song->artwork);
}

static void open_player_artist(Scene *scene, const View *view) {
    scene->lyrics_fullscreen = false;

    if (spotify_id_valid(view->playback.artist_id)) {
        CatalogItem artist = {.kind = ITEM_ARTIST};
        copy_utf8(artist.id, sizeof(artist.id), view->playback.artist_id);
        copy_utf8(artist.title, sizeof(artist.title), view->playback.artist);
        scene_open_collection(scene, &artist);
        return;
    }

    if (view->playback.artist[0]) {
        copy_utf8(scene->query, sizeof(scene->query), view->playback.artist);
        scene->search_filter = SEARCH_ARTISTS;
        scene_search(scene);
    }
}

static void open_player_album(Scene *scene, const View *view) {
    scene->lyrics_fullscreen = false;

    if (spotify_id_valid(view->playback.album_id)) {
        CatalogItem album = {.kind = ITEM_ALBUM};
        copy_utf8(album.id, sizeof(album.id), view->playback.album_id);
        copy_utf8(album.title, sizeof(album.title),
                  view->playback.album[0] ? view->playback.album : view->playback.title);
        copy_utf8(album.detail, sizeof(album.detail), view->playback.artist);
        copy_utf8(album.artwork, sizeof(album.artwork), view->playback.artwork);
        scene_open_collection(scene, &album);
        return;
    }

    if (view->playback.album[0]) {
        copy_utf8(scene->query, sizeof(scene->query), view->playback.album);
        scene->search_filter = SEARCH_ALBUMS;
        scene_search(scene);
    }
}

static void activate_context_menu(Scene *scene, const View *view, HitId id) {
    unsigned idx = (unsigned)(id - HIT_CTX_FIRST);
    scene->menu.open = false;

    if (idx >= scene->menu.count) {
        return;
    }

    CatalogItem item = scene->menu.item;

    switch (scene->menu.actions[idx]) {
    case CTX_PLAY:
        if (item.kind == ITEM_ALBUM || item.kind == ITEM_ARTIST || item.kind == ITEM_PLAYLIST) {
            scene_open_collection(scene, &item);
            return;
        }

        if (!scene->home && scene->section == BROWSE_SEARCH) {
            app_search_history_add(&item);
        }

        app_open_item(&item);
        return;
    case CTX_RADIO:
        if (item.kind == ITEM_TRACK) {
            open_song_radio(scene, &item);
        } else {
            app_start_radio(&item);
        }
        return;
    case CTX_QUEUE:
        app_enqueue_item(&item);
        return;
    case CTX_DEQUEUE:
        app_dequeue(item.id);
        return;
    case CTX_ALBUM:
        if (spotify_id_valid(item.album_id)) {
            CatalogItem album = {.kind = ITEM_ALBUM};
            copy_utf8(album.id, sizeof(album.id), item.album_id);
            copy_utf8(album.title, sizeof(album.title), item.album_title[0] ? item.album_title : "Album");
            copy_utf8(album.artwork, sizeof(album.artwork), item.artwork);
            scene_open_collection(scene, &album);
            return;
        }

        if (item.album_title[0]) {
            copy_utf8(scene->query, sizeof(scene->query), item.album_title);
            scene->search_filter = SEARCH_ALBUMS;
            scene_search(scene);
        }

        return;
    case CTX_ARTIST:
        if (spotify_id_valid(item.artist_id)) {
            CatalogItem artist = {.kind = ITEM_ARTIST};
            copy_utf8(artist.id, sizeof(artist.id), item.artist_id);
            copy_utf8(artist.title, sizeof(artist.title), item.detail[0] ? item.detail : "Artist");
            scene_open_collection(scene, &artist);
            return;
        }

        if (item.detail[0]) {
            copy_utf8(scene->query, sizeof(scene->query), item.detail);
            scene->search_filter = SEARCH_ARTISTS;
            scene_search(scene);
        }

        return;
    case CTX_LINK:
        app_open_item_link(&item);
        return;
    case CTX_PROFILE:
        scene_open_section(scene, BROWSE_PROFILE_PLAYLISTS);
        return;
    case CTX_SETTINGS:
        scene_open_settings(scene);
        return;
    case CTX_LOGOUT:
        scene_activate(scene, view, HIT_ACCOUNT);
        return;
    default:
        return;
    }
}

void scene_activate(Scene *scene, const View *view, HitId id) {
    if (id >= HIT_LYRIC_FIRST && id <= HIT_LYRIC_LAST) {
        unsigned index = (unsigned)(id - HIT_LYRIC_FIRST);

        if (scene->panel == PANEL_LYRICS && scene->hits[id].enabled && view->lyrics.state == LYRICS_SYNCED &&
            index < view->lyrics.count && !strcmp(scene->lyric_uri, view->playback.uri) &&
            lyrics_match(&view->lyrics, &view->playback)) {
            if (app_seek_lyric(view->lyrics.uri, view->lyrics.lines[index].time_ms)) {
                scene->lyrics_follow = true;
                scene->lyric_active = -2;
            }
        }

        return;
    }

    if (id >= HIT_HISTORY_REMOVE_FIRST && id <= HIT_HISTORY_REMOVE_LAST) {
        unsigned index = (unsigned)(id - HIT_HISTORY_REMOVE_FIRST);

        if (index < view->search_history_count) {
            app_search_history_remove(view->search_history[index].id);
            scene->focus = HIT_SEARCH;
        }

        return;
    }

    if (id >= HIT_HISTORY_FIRST && id <= HIT_HISTORY_LAST) {
        unsigned index = (unsigned)(id - HIT_HISTORY_FIRST);

        if (index < view->search_history_count) {
            scene->search_history_open = false;
            scene->search_due = 0;
            scene->focus = HIT_NONE;
            app_play_search_history(&view->search_history[index]);
        }

        return;
    }

    const CatalogItem *song = scene_like_item(scene, view, id);

    if (song && song->kind == ITEM_TRACK) {
        app_toggle_song_like(song);
        return;
    }

    if (id >= HIT_CTX_FIRST && id <= HIT_CTX_LAST) {
        activate_context_menu(scene, view, id);
        return;
    }

    if (id >= HIT_HOME_QUICK_FIRST && id <= HIT_HOME_QUICK_LAST) {
        unsigned index = (unsigned)(id - HIT_HOME_QUICK_FIRST);

        const CatalogItem *item = scene_home_quick_item(view, index);

        if (item) {
            scene_open_collection(scene, item);
            return;
        }

        if (index > 0 && (view->library[2].count || index > 3)) {
            return;
        }

        if (index == 3) {
            scene->focus = HIT_SEARCH;
            scene_select_input_all(scene);
            scene->search_history_open = true;
            return;
        }

        static const BrowseKind shortcuts[] = {BROWSE_SAVED_TRACKS, BROWSE_SAVED_ALBUMS, BROWSE_PLAYLISTS};

        if (index < 3) {
            scene_open_section(scene, shortcuts[index]);
        }

        return;
    }

    if (id >= HIT_SEARCH_ALL && id <= HIT_SEARCH_PLAYLISTS) {
        scene->search_filter = (SearchFilter)(id - HIT_SEARCH_ALL);
        scene_search(scene);
        return;
    }

    if (id >= HIT_SEARCH_TOP && id <= HIT_SEARCH_PLAYLIST_LAST) {
        const CatalogItem *item = search_scene_item(scene, view, id);

        if (item && (item->kind == ITEM_ALBUM || item->kind == ITEM_ARTIST || item->kind == ITEM_PLAYLIST)) {
            scene_open_collection(scene, item);
            return;
        }

        if (item) {
            app_search_history_add(item);
            app_open_item(item);
        }

        return;
    }

    if (id >= HIT_LIBRARY_FIRST && id <= HIT_LIBRARY_LAST) {
        unsigned index = scene->library_first + (unsigned)(id - HIT_LIBRARY_FIRST);
        const CatalogItem *item = list_row(&view->playlists, index);

        if (item) {
            scene_open_collection(scene, item);
        }

        return;
    }

    if (id >= HIT_ROW_FIRST && id <= HIT_ROW_LAST) {
        unsigned index = scene->first_row + (unsigned)(id - HIT_ROW_FIRST);
        const CatalogItem *item = list_row(&view->list, index);

        if (!item || !app_select((int)index)) {
            return;
        }

        if (scene->section == BROWSE_SEARCH && view->list.request.kind == BROWSE_SEARCH) {
            app_search_history_add(item);
        }

        if (item->kind == ITEM_ALBUM || item->kind == ITEM_PLAYLIST || item->kind == ITEM_ARTIST) {
            scene_open_collection(scene, item);
        }

        return;
    }

    if (id >= HIT_CARD_FIRST && id <= HIT_CARD_LAST) {
        unsigned card_index = (unsigned)(id - HIT_CARD_FIRST);
        const CatalogItem *item = scene_home_item(view, card_index);

        if (item) {
            if (card_index / HOME_SHELF_COLUMNS == 2) {
                open_song_radio(scene, item);
            } else if (item->kind == ITEM_TRACK) {
                app_play_track(item);
            } else {
                scene_open_collection(scene, item);
            }
        }

        return;
    }

    if (id >= HIT_PANEL_RECENT_FIRST && id <= HIT_PANEL_RECENT_LAST) {
        unsigned index = (unsigned)(id - HIT_PANEL_RECENT_FIRST);

        if (index < view->recent_count) {
            if (view->recent[index].kind == ITEM_TRACK) {
                app_play_track(&view->recent[index]);
            } else {
                scene_open_collection(scene, &view->recent[index]);
            }
        }

        return;
    }

    if (id >= HIT_POPULAR_FIRST && id <= HIT_POPULAR_LAST) {
        unsigned index = (unsigned)(id - HIT_POPULAR_FIRST);
        scene->popular_selected = index;

        if (scene->double_click) {
            app_play_popular(index);
        }

        return;
    }

    if (id >= HIT_PROFILE_ARTIST_FIRST && id <= HIT_PROFILE_ARTIST_LAST) {
        unsigned index = (unsigned)(id - HIT_PROFILE_ARTIST_FIRST);

        if (index < view->profile.following_count) {
            scene_open_collection(scene, &view->profile.following[index]);
        }

        return;
    }

    if (id >= HIT_DISC_ALL && id <= HIT_DISC_SINGLES) {
        static const char *groups[] = {NULL, "album", "single"};
        app_filter_discography(groups[id - HIT_DISC_ALL]);
        return;
    }

    if (id >= HIT_DEVICE_FIRST && id <= HIT_DEVICE_LAST) {
        app_transfer_to((int)(id - HIT_DEVICE_FIRST));
        return;
    }

    switch (id) {
    case HIT_BACK:
    case HIT_FORWARD: {
        if ((id == HIT_BACK && !scene->history_index) ||
            (id == HIT_FORWARD && scene->history_index + 1 >= scene->history_count)) {
            return;
        }

        unsigned next = id == HIT_BACK ? scene->history_index - 1 : scene->history_index + 1;
        const SceneRoute *route = &scene->history[next];
        scene->restoring_route = true;

        if (route->home) {
            scene_activate(scene, view, HIT_HOME);
        } else if (route->kind == BROWSE_SEARCH) {
            copy_utf8(scene->query, sizeof(scene->query), route->query);
            scene->search_filter = route->filter;
            scene_search(scene);
        } else if (route->kind == BROWSE_ARTIST_LIKES) {
            open_artist_likes(scene, &route->item);
        } else if (route->kind == BROWSE_SONG_RADIO) {
            open_song_radio(scene, &route->item);
        } else if (route->kind == BROWSE_SETTINGS) {
            scene_open_settings(scene);
        } else if (route->item.id[0]) {
            scene_open_collection(scene, &route->item);
        } else {
            scene_open_section(scene, route->kind);
        }

        scene->restoring_route = false;
        scene->history_index = next;
        return;
    }
    case HIT_SEARCH:
        scene->search_history_open = true;
        return;
    case HIT_SEARCH_NAV:
        scene->focus = HIT_SEARCH;
        scene_select_input_all(scene);
        scene->search_history_open = true;
        return;

    case HIT_SEARCH_CLEAR:
        scene->query[0] = 0;
        scene->search_input = (InputState){0};
        scene->select_all = false;
        scene->input_click_pending = false;
        scene->focus = HIT_SEARCH;
        scene_query_changed(scene);
        scene->search_history_open = true;
        return;
    case HIT_PLAYER_LIKE:
        if (!strncmp(view->playback.uri, "spotify:track:", 14)) {
            CatalogItem song = {.kind = ITEM_TRACK, .liked = view->playback_liked, .playable = true};
            copy_utf8(song.id, sizeof(song.id), view->playback.uri + 14);
            copy_utf8(song.title, sizeof(song.title), view->playback.title);
            copy_utf8(song.detail, sizeof(song.detail), view->playback.artist);
            copy_utf8(song.artwork, sizeof(song.artwork), view->playback.artwork);
            app_toggle_song_like(&song);
        }

        return;
    case HIT_ARTIST_LIKES: {
        CatalogItem artist = view->details.item;

        if (!spotify_id_valid(artist.id)) {
            artist.kind = ITEM_ARTIST;
            copy_utf8(artist.id, sizeof(artist.id), view->list.request.id);
            copy_utf8(artist.title, sizeof(artist.title), view->list.request.title);
            copy_utf8(artist.artwork, sizeof(artist.artwork), view->list.request.artwork);
        }

        open_artist_likes(scene, &artist);
        return;
    }
    case HIT_HOME_HISTORY:
        app_connect();
        return;
    case HIT_HOME_ALBUMS:
        scene_open_section(scene, BROWSE_SAVED_ALBUMS);
        return;
    case HIT_HOME_PLAYLISTS:
        scene_open_section(scene, BROWSE_PLAYLISTS);
        return;
    case HIT_HOME:
    case HIT_TOP_HOME:
        scene->search_due = 0;
        app_action(ACTION_HOME);

        if (scene->home && !scene->pending_browse) {
            return;
        }

        record_route(scene, &(SceneRoute){.home = true});
        scene->home = true;
        scene->pending_browse = false;
        scene->main_scroll = (Scroll){0};
        scene->home_scroll = 0;
        scene->entrance = 1;
        scene->retain_content = false;
        return;
    case HIT_PROFILE:
        scene->menu = (ContextMenu){0};
        menu_add(&scene->menu, CTX_PROFILE, "Profile");
        menu_add(&scene->menu, CTX_SETTINGS, "Settings");
        menu_add(&scene->menu, CTX_LOGOUT, view->authenticated ? "Log out" : "Log in");
        scene->menu.rect =
            (Rect){scene->hits[HIT_PROFILE].rect.x - 120, 42, 152, 12 + scene->menu.count * 34.0f};
        scene->menu.open = true;
        return;
    case HIT_PROFILE_FOLLOWING:
        if (view->profile.permission_required) {
            app_connect();
            return;
        }

        scene_open_section(scene, BROWSE_PROFILE_PLAYLISTS == scene->section ? BROWSE_FOLLOWED_ARTISTS
                                                                             : BROWSE_PROFILE_PLAYLISTS);
        return;
    case HIT_DETAIL_PLAY:
        app_play_collection();
        return;
    case HIT_DETAIL_SHUFFLE:
        app_action(ACTION_SHUFFLE);
        return;
    case HIT_DETAIL_SAVE:
        if (view->details.permission_required) {
            app_connect();
            return;
        }

        app_toggle_collection_saved();
        return;
    case HIT_DETAIL_LINK:
        app_open_collection_link();
        return;
    case HIT_DETAIL_ARTIST:
        scene_open_collection(scene, &view->details.artist);
        return;
    case HIT_DETAIL_ALL:
        scene->popular_expanded = !scene->popular_expanded;
        return;
    case HIT_TRACKS:
        scene_open_section(scene, BROWSE_SAVED_TRACKS);
        return;
    case HIT_ARTISTS:
        scene_open_section(scene, BROWSE_FOLLOWED_ARTISTS);
        return;
    case HIT_ALBUMS:
        scene_open_section(scene, BROWSE_SAVED_ALBUMS);
        return;
    case HIT_PLAYLISTS:
        scene_open_section(scene, BROWSE_PLAYLISTS);
        return;
    case HIT_LIBRARY_COLLAPSE:
        scene->library_collapsed = !scene->library_collapsed;
        scene_save_layout(scene);
        return;
    case HIT_QUEUE:
        panel_scene_toggle(scene, PANEL_QUEUE);
        return;
    case HIT_DEVICES:
        panel_scene_toggle(scene, PANEL_DEVICES);
        return;
    case HIT_PLAYING_ON:
        if (scene->panel != PANEL_DEVICES) {
            panel_scene_toggle(scene, PANEL_DEVICES);
        }

        return;
    case HIT_MUTE: {
        unsigned restore = scene->unmute_volume ? scene->unmute_volume : 50;

        if (scene->draft.volume > 0) {
            scene->unmute_volume = scene->draft.volume;
        }

        scene->draft.volume = scene->draft.volume > 0 ? 0 : restore;
        app_volume(scene->draft.volume);
        return;
    }
    case HIT_FRIENDS:
        panel_scene_toggle(scene, PANEL_FRIENDS);
        return;
    case HIT_LYRICS:
        panel_scene_toggle(scene, PANEL_LYRICS);
        return;
    case HIT_LYRICS_FULLSCREEN:
        scene->lyrics_fullscreen = !scene->lyrics_fullscreen;
        scene->lyric_layout_width = 0;
        scene->search_history_open = false;
        scene->search_due = 0;
        return;
    case HIT_LYRICS_FOLLOW:
        scene->lyrics_follow = true;
        scene->lyric_active = -2;
        return;
    case HIT_PANEL_CLOSE:
        panel_scene_toggle(scene, scene->panel);
        return;
    case HIT_PANEL_QUEUE:
        panel_scene_toggle(scene, PANEL_QUEUE);
        scene_open_section(scene, BROWSE_QUEUE);
        return;
    case HIT_PANEL_QUEUE_TAB:
    case HIT_PANEL_RECENT_TAB:
        scene->panel_recent = id == HIT_PANEL_RECENT_TAB;
        return;
    case HIT_PANEL_PLAY:
        app_action(ACTION_TOGGLE_PLAYBACK);
        return;
    case HIT_QUEUE_CLEAR:
        app_clear_queue();
        return;
    case HIT_PANEL_PREVIOUS:
        app_action(ACTION_PREVIOUS);
        return;
    case HIT_PANEL_NEXT:
        app_action(ACTION_NEXT);
        return;
    case HIT_PLAYER_ARTIST:
        open_player_artist(scene, view);
        return;
    case HIT_PLAYER_SONG:
        open_player_album(scene, view);
        return;
    case HIT_ACCOUNT:
        if (view->authenticated) {
            app_action(ACTION_DISCONNECT);
            return;
        }

        if (app_set_client_id(scene->client_id)) {
            app_connect();
        }

        return;
    case HIT_DEV_DASHBOARD:
        app_open_developer_dashboard();
        return;
    case HIT_DEV_GUIDE:
        app_open_setup_guide();
        return;
    case HIT_REFRESH:
        app_action(ACTION_REFRESH);
        return;
    case HIT_PREVIOUS:
        app_action(ACTION_PREVIOUS);
        return;
    case HIT_PLAY:
        app_action(ACTION_TOGGLE_PLAYBACK);
        return;
    case HIT_SHUFFLE:
        app_action(ACTION_SHUFFLE);
        return;
    case HIT_REPEAT:
        app_action(ACTION_REPEAT);
        return;
    case HIT_NEXT:
        app_action(ACTION_NEXT);
        return;
    case HIT_LIST_FILTER_CLEAR:
        scene->list_filter[0] = 0;
        scene->filter_input = (InputState){0};
        scene->select_all = false;
        scene->input_click_pending = false;
        scene->focus = HIT_LIST_FILTER;
        app_list_filter("");
        return;
    case HIT_SORT_INDEX:
    case HIT_SORT_TITLE:
    case HIT_SORT_ALBUM:
    case HIT_SORT_DURATION: {
        static const ListSort columns[] = {LIST_SORT_NONE, LIST_SORT_TITLE, LIST_SORT_ALBUM, LIST_SORT_DURATION};
        ListSort sort = columns[id - HIT_SORT_INDEX];

        // Each click cycles ascending, descending, then back to the playlist's own order.
        if (sort != view->list.sort) {
            app_list_sort(sort, false);
            return;
        }

        app_list_sort(view->list.sort_descending ? LIST_SORT_NONE : sort, true);
        return;
    }
    case HIT_OPEN:
        if (!scene->home && app_list_arrangeable(scene->section)) {
            bool arranged = view->list.sort != LIST_SORT_NONE || view->list.filter[0];

            // Unsorted playlists play as a Spotify context; everything else plays the rows as shown.
            if (scene->section == BROWSE_PLAYLIST_TRACKS && !arranged) {
                app_play_collection();
                return;
            }

            app_play_list();
            return;
        }

        if (scene->home) {
            if (view->authenticated) {
                app_action(ACTION_REFRESH);
            }

            if (!view->authenticated) {
                scene_activate(scene, view, HIT_ACCOUNT);
            }
        }

        if (!scene->home && scene->section == BROWSE_PLAYLIST_TRACKS && view->list.selected < 0) {
            app_play_collection();
            return;
        }

        if (!scene->home) {
            app_activate();
        }

        return;
    case HIT_SETTING_SEARCH_RADIO:
        app_set_search_radio(!view->settings.search_radio);
        return;
    case HIT_SETTING_REGEN_MIXES:
        app_regenerate_home_suggestions(HOME_CACHE_MIXES);
        return;
    case HIT_SETTING_REGEN_PLAYLISTS:
        app_regenerate_home_suggestions(HOME_CACHE_PLAYLISTS);
        return;
    case HIT_SETTING_REGEN_TRACKS:
        app_regenerate_home_suggestions(HOME_CACHE_TRACKS);
        return;
    case HIT_SETTING_REGEN_ALL:
        app_regenerate_home_suggestions(HOME_CACHE_ALL);
        return;
    case HIT_LINK:
        app_open_link();
        return;
    case HIT_TRANSFER:
        if (view->engine != ENGINE_READY) {
            scene->setup_popup = true;
            return;
        }

        app_transfer();
        return;
    case HIT_SETUP_KEY:
        app_engine_dashboard();
        return;
    case HIT_SETUP_PASTE:
        ui_paste_engine_key();
        return;
    case HIT_SETUP_FORGET:
        app_engine_forget();
        return;
    case HIT_CLOSE_SETUP:
        scene->setup_popup = false;
        return;
    default:
        return;
    }
}

static bool item_at(const Scene *scene, const View *view, float x, float y, CatalogItem *out) {
    HitId id = scene_hit(scene, x, y);

    if (id >= HIT_ROW_FIRST && id <= HIT_ROW_LAST) {
        const CatalogItem *item =
            (!scene->home && scene->section == BROWSE_SEARCH)
                ? search_scene_item(scene, view, id)
                : list_row(&view->list, scene->first_row + (unsigned)(id - HIT_ROW_FIRST));

        if (item) {
            *out = *item;
            return true;
        }
    }

    if (id >= HIT_POPULAR_FIRST && id <= HIT_POPULAR_LAST) {
        unsigned index = (unsigned)(id - HIT_POPULAR_FIRST);

        if (index < view->details.popular_count) {
            *out = view->details.popular[index];
            return true;
        }
    }

    if (id >= HIT_SEARCH_TOP && id <= HIT_SEARCH_PLAYLIST_LAST) {
        const CatalogItem *item = search_scene_item(scene, view, id);

        if (item) {
            *out = *item;
            return true;
        }
    }

    if (id >= HIT_HOME_QUICK_FIRST && id <= HIT_HOME_QUICK_LAST) {
        const CatalogItem *item = scene_home_quick_item(view, (unsigned)(id - HIT_HOME_QUICK_FIRST));

        if (item) {
            *out = *item;
            return true;
        }
    }

    if (id >= HIT_CARD_FIRST && id <= HIT_CARD_LAST) {
        const CatalogItem *item = scene_home_item(view, (unsigned)(id - HIT_CARD_FIRST));

        if (item) {
            *out = *item;
            return true;
        }
    }

    if (id >= HIT_LIBRARY_FIRST && id <= HIT_LIBRARY_LAST) {
        const CatalogItem *item =
            list_row(&view->playlists, scene->library_first + (unsigned)(id - HIT_LIBRARY_FIRST));

        if (item) {
            *out = *item;
            return true;
        }
    }

    if (id >= HIT_PROFILE_ARTIST_FIRST && id <= HIT_PROFILE_ARTIST_LAST) {
        unsigned index = (unsigned)(id - HIT_PROFILE_ARTIST_FIRST);

        if (index < view->profile.following_count) {
            *out = view->profile.following[index];
            return true;
        }
    }

    bool in_now_playing = y >= scene->player.y && x < 280;

    if (in_now_playing && !strncmp(view->playback.uri, "spotify:track:", 14) &&
        spotify_id_valid(view->playback.uri + 14)) {
        *out = (CatalogItem){.kind = ITEM_TRACK, .playable = true};
        copy_utf8(out->id, sizeof(out->id), view->playback.uri + 14);
        copy_utf8(out->title, sizeof(out->title), view->playback.title);
        copy_utf8(out->detail, sizeof(out->detail), view->playback.artist);
        copy_utf8(out->artist_id, sizeof(out->artist_id), view->playback.artist_id);
        copy_utf8(out->album_id, sizeof(out->album_id), view->playback.album_id);
        copy_utf8(out->album_title, sizeof(out->album_title), view->playback.album);
        copy_utf8(out->artwork, sizeof(out->artwork), view->playback.artwork);
        return true;
    }

    if (scene->panel == PANEL_QUEUE && !scene->panel_recent && scene_contains(scene->side_panel, x, y)) {
        float top = scene->side_panel.y + QUEUE_LIST_TOP;

        if (y >= top && y < scene->side_panel.y + scene->side_panel.h - QUEUE_LIST_BOTTOM) {
            unsigned index = (unsigned)((y - top + scene->panel_scroll.offset) / QUEUE_ROW_HEIGHT);

            const CatalogItem *item = queue_row(&view->queue, index);

            if (item) {
                *out = *item;
                return true;
            }
        }
    }

    return false;
}

void scene_context_press(Scene *scene, const View *view, float x, float y) {
    scene->menu = (ContextMenu){0};

    if (scene->login_screen || scene->setup_popup) {
        return;
    }

    CatalogItem item = {0};

    if (!item_at(scene, view, x, y, &item)) {
        return;
    }

    scene->menu.item = item;

    if (item.kind == ITEM_TRACK || item.kind == ITEM_EPISODE) {
        if (item.playable) {
            menu_add(&scene->menu, CTX_PLAY, "Play");
            bool queued = scene_queued(view, item.id);
            menu_add(&scene->menu, queued ? CTX_DEQUEUE : CTX_QUEUE, queued ? "Remove from queue" : "Add to queue");
        }

        if (item.kind == ITEM_TRACK && spotify_id_valid(item.id)) {
            menu_add(&scene->menu, CTX_RADIO, "Go to song radio");
        }

        if (item.album_id[0] || item.album_title[0]) {
            menu_add(&scene->menu, CTX_ALBUM, "Go to album");
        }

        if (item.artist_id[0] || item.detail[0]) {
            menu_add(&scene->menu, CTX_ARTIST, "Go to artist");
        }

        menu_add(&scene->menu, CTX_LINK, "Open in Spotify");
    } else if (item.kind == ITEM_ALBUM) {
        menu_add(&scene->menu, CTX_PLAY, "Open album");

        if (item.artist_id[0] || item.detail[0]) {
            menu_add(&scene->menu, CTX_ARTIST, "Go to artist");
        }

        menu_add(&scene->menu, CTX_RADIO, "Go to artist radio");
        menu_add(&scene->menu, CTX_LINK, "Open in Spotify");
    } else if (item.kind == ITEM_ARTIST) {
        menu_add(&scene->menu, CTX_PLAY, "Open artist");
        menu_add(&scene->menu, CTX_RADIO, "Go to artist radio");
        menu_add(&scene->menu, CTX_LINK, "Open in Spotify");
    } else if (item.kind == ITEM_PLAYLIST) {
        menu_add(&scene->menu, CTX_PLAY, "Open playlist");
        menu_add(&scene->menu, CTX_LINK, "Open in Spotify");
    }

    if (!scene->menu.count) {
        return;
    }

    float w = 208;
    float h = 12 + scene->menu.count * 34.0f;
    float mx = x + w > scene->width - 12 ? scene->width - w - 12 : x;
    float my = y + h > scene->height - 12 ? scene->height - h - 12 : y;
    scene->menu.rect = (Rect){mx < 12 ? 12 : mx, my < 12 ? 12 : my, w, h};
    scene->menu.open = true;
}
