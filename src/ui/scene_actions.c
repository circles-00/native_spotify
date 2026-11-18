#include "scene_internal.h"
#include "panel_scene.h"
#include "search_scene.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

void scene_open_section(Scene *scene, BrowseKind kind) {
    scene->search_due = 0;

    if (kind != BROWSE_QUEUE && !app_browse(kind, NULL)) {
        return;
    }

    if (kind == BROWSE_QUEUE) {
        app_action(ACTION_QUEUE);
    }

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
    BrowseRequest request;

    if (!item_browse_request(item, &request) || !app_open_item(item)) {
        return;
    }

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

static void menu_add(ContextMenu *menu, ContextAction action, const char *label) {
    if (menu->count >= 6) {
        return;
    }

    menu->actions[menu->count] = action;
    copy_utf8(menu->labels[menu->count], sizeof(menu->labels[menu->count]), label);
    ++menu->count;
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

        app_open_item(&item);
        return;
    case CTX_RADIO:
        app_start_radio(&item);
        return;
    case CTX_QUEUE:
        app_enqueue_item(&item);
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
    case CTX_LOGOUT:
        scene_activate(scene, view, HIT_ACCOUNT);
        return;
    default:
        return;
    }
}

void scene_activate(Scene *scene, const View *view, HitId id) {
    if (id >= HIT_CTX_FIRST && id <= HIT_CTX_LAST) {
        activate_context_menu(scene, view, id);
        return;
    }

    if (id >= HIT_HOME_QUICK_FIRST && id <= HIT_HOME_QUICK_LAST) {
        unsigned index = (unsigned)(id - HIT_HOME_QUICK_FIRST);

        if (index < view->recent_count) {
            scene_open_collection(scene, &view->recent[index]);
            return;
        }

        if (view->recent_count) {
            return;
        }

        if (index == 3) {
            scene->focus = HIT_SEARCH;
            scene->select_all = true;
            return;
        }

        static const BrowseKind shortcuts[] = {BROWSE_SAVED_TRACKS, BROWSE_SAVED_ALBUMS, BROWSE_PLAYLISTS};

        if (index < 3) {
            scene_open_section(scene, shortcuts[index]);
        }

        return;
    }

    if (id >= HIT_SEARCH_ALL && id <= HIT_SEARCH_ALBUMS) {
        scene->search_filter = (SearchFilter)(id - HIT_SEARCH_ALL);
        scene_search(scene);
        return;
    }

    if (id >= HIT_SEARCH_TOP && id <= HIT_SEARCH_ALBUM_LAST) {
        const CatalogItem *item = search_scene_item(scene, view, id);

        if (item && (item->kind == ITEM_ALBUM || item->kind == ITEM_ARTIST || item->kind == ITEM_PLAYLIST)) {
            scene_open_collection(scene, item);
            return;
        }

        if (item) {
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

        if (item->kind == ITEM_ALBUM || item->kind == ITEM_PLAYLIST || item->kind == ITEM_ARTIST) {
            scene_open_collection(scene, item);
        }

        return;
    }

    if (id >= HIT_CARD_FIRST && id <= HIT_CARD_LAST) {
        const CatalogItem *item = scene_home_item(view, (unsigned)(id - HIT_CARD_FIRST));

        if (item) {
            scene_open_collection(scene, item);
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
        app_select_device((int)(id - HIT_DEVICE_FIRST));
        return;
    }

    switch (id) {
    case HIT_SEARCH:
        return;
    case HIT_HOME_HISTORY:
        app_connect();
        return;
    case HIT_HOME_ALBUMS:
        scene_open_section(scene, BROWSE_SAVED_ALBUMS);
        return;
    case HIT_HOME_PLAYLISTS:
        scene_open_section(scene, BROWSE_PLAYLISTS);
        return;
    case HIT_ALL:
    case HIT_HOME:
        scene->search_due = 0;
        app_action(ACTION_HOME);

        if (scene->home && !scene->pending_browse) {
            return;
        }

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
        menu_add(&scene->menu, CTX_LOGOUT, view->authenticated ? "Log out" : "Log in");
        scene->menu.rect = (Rect){scene->width - 176, 56, 152, 12 + scene->menu.count * 34.0f};
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
    case HIT_MUSIC:
        scene_open_section(scene, BROWSE_SAVED_TRACKS);
        return;
    case HIT_ALBUMS:
        scene_open_section(scene, BROWSE_SAVED_ALBUMS);
        return;
    case HIT_PLAYLISTS:
        scene_open_section(scene, BROWSE_PLAYLISTS);
        return;
    case HIT_QUEUE:
        panel_scene_toggle(scene, PANEL_QUEUE);
        return;
    case HIT_NOW_PLAYING:
        panel_scene_toggle(scene, PANEL_NOW_PLAYING);
        return;
    case HIT_FRIENDS:
        panel_scene_toggle(scene, PANEL_FRIENDS);
        return;
    case HIT_LYRICS:
        panel_scene_toggle(scene, PANEL_LYRICS);
        return;
    case HIT_PANEL_CLOSE:
        panel_scene_toggle(scene, scene->panel);
        return;
    case HIT_PANEL_QUEUE:
        panel_scene_toggle(scene, PANEL_QUEUE);
        scene_open_section(scene, BROWSE_QUEUE);
        return;
    case HIT_PANEL_PLAY:
        app_action(view->playback.playing ? ACTION_PAUSE : ACTION_PLAY);
        return;
    case HIT_PANEL_ARTIST:
        if (spotify_id_valid(view->playback.artist_id)) {
            CatalogItem artist = {.kind = ITEM_ARTIST};
            copy_utf8(artist.id, sizeof(artist.id), view->playback.artist_id);
            copy_utf8(artist.title, sizeof(artist.title), view->playback.artist);
            scene_open_collection(scene, &artist);
            return;
        }

        copy_utf8(scene->query, sizeof(scene->query), view->playback.artist);
        scene->search_filter = SEARCH_ARTISTS;
        scene_search(scene);
        return;
    case HIT_ACCOUNT:
        if (view->authenticated) {
            app_action(ACTION_DISCONNECT);
            return;
        }

        app_connect();
        return;
    case HIT_REFRESH:
        app_action(ACTION_REFRESH);
        return;
    case HIT_PREVIOUS:
        app_action(ACTION_PREVIOUS);
        return;
    case HIT_PLAY:
        app_action(view->playback.playing ? ACTION_PAUSE : ACTION_PLAY);
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
    case HIT_OPEN:
        if (scene->home) {
            if (view->authenticated) {
                app_action(ACTION_REFRESH);
            }

            if (!view->authenticated) {
                scene_activate(scene, view, HIT_ACCOUNT);
            }
        }

        if (!scene->home) {
            app_activate();
        }

        return;
    case HIT_LINK:
        app_open_link();
        return;
    case HIT_DEVICES:
        scene->device_popup = !scene->device_popup;

        if (scene->device_popup) {
            app_action(ACTION_DEVICES);
        }

        return;
    case HIT_CLOSE_DEVICES:
        scene->device_popup = false;
        return;
    case HIT_TRANSFER:
        if (view->engine != ENGINE_READY) {
            scene->device_popup = false;
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
    HitId id = HIT_NONE;

    for (int i = HIT_COUNT - 1; i > 0; --i) {
        if (scene->hits[i].enabled && scene_contains(scene->hits[i].rect, x, y)) {
            id = (HitId)i;
            break;
        }
    }

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

    if (id >= HIT_SEARCH_TOP && id <= HIT_SEARCH_ALBUM_LAST) {
        const CatalogItem *item = search_scene_item(scene, view, id);

        if (item) {
            *out = *item;
            return true;
        }
    }

    if (id >= HIT_HOME_QUICK_FIRST && id <= HIT_HOME_QUICK_LAST) {
        unsigned index = (unsigned)(id - HIT_HOME_QUICK_FIRST);

        if (index < view->recent_count) {
            *out = view->recent[index];
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

    bool in_now_playing = (scene->panel == PANEL_NOW_PLAYING && scene_contains(scene->side_panel, x, y)) ||
                          (y >= scene->player.y && x < 280);

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

    if (scene->panel == PANEL_QUEUE && scene_contains(scene->side_panel, x, y)) {
        for (unsigned i = 0; i < 16 && scene->panel_first + i < view->queue.count; ++i) {
            float row_y = scene->side_panel.y + 164 + i * 56 -
                          (scene->panel_scroll.offset - scene->panel_first * 56.0f);

            if (y >= row_y && y < row_y + 52) {
                *out = view->queue.items[scene->panel_first + i];
                return true;
            }
        }
    }

    return false;
}

void scene_context_press(Scene *scene, const View *view, float x, float y) {
    scene->menu = (ContextMenu){0};

    if (scene->login_screen || scene->setup_popup || scene->device_popup) {
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
            menu_add(&scene->menu, CTX_QUEUE, "Add to queue");
        }

        menu_add(&scene->menu, CTX_RADIO, "Go to song radio");

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

    float w = 196;
    float h = 12 + scene->menu.count * 34.0f;
    float mx = x + w > scene->width - 12 ? scene->width - w - 12 : x;
    float my = y + h > scene->height - 12 ? scene->height - h - 12 : y;
    scene->menu.rect = (Rect){mx < 12 ? 12 : mx, my < 12 ? 12 : my, w, h};
    scene->menu.open = true;
}
