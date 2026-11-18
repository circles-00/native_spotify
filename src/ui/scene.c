#include "scene.h"
#include "collection_scene.h"
#include "home_scene.h"
#include "panel_scene.h"
#include "platform.h"
#include "search_scene.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

static void hit(Scene *scene, HitId id, Rect rect, bool enabled) {
    scene->hits[id] = (Hit){rect, enabled};
}

void scene_init(Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    scene->home = true;
    scene->entrance = 1;
    scene->focus = HIT_SEARCH;
    scene->last_selected = -1;
}

unsigned scene_columns(float width) {
    return width >= 850 ? 5 : width >= 660 ? 4 : 3;
}

static bool contains(Rect rect, float x, float y) {
    return rect.w > 0 && rect.h > 0 && x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
           y < rect.y + rect.h;
}

static bool setup_hit(int id) {
    return id == HIT_SETUP_KEY || id == HIT_SETUP_PASTE || id == HIT_SETUP_FORGET || id == HIT_CLOSE_SETUP;
}

HitId scene_hit(const Scene *scene, float x, float y) {
    if (scene->menu.open) {
        for (int i = HIT_CTX_LAST; i >= HIT_CTX_FIRST; --i) {
            if (scene->hits[i].enabled && contains(scene->hits[i].rect, x, y)) {
                return (HitId)i;
            }
        }
        return HIT_NONE;
    }

    for (int i = HIT_COUNT - 1; i > 0; --i) {
        bool home_result = (i >= HIT_HOME_QUICK_FIRST && i <= HIT_HOME_PLAYLISTS) ||
                           (i >= HIT_CARD_FIRST && i <= HIT_CARD_LAST);

        if (scene->home && home_result &&
            (y < scene->content.y + 56 || y >= scene->content.y + scene->content.h || x < scene->content.x ||
             x >= scene->content.x + scene->content.w)) {
            continue;
        }

        if (scene->panel != PANEL_NONE && !scene->device_popup && !scene->setup_popup &&
            contains(scene->side_panel, x, y) && i != HIT_PANEL_CLOSE && i != HIT_PANEL_QUEUE &&
            i != HIT_PANEL_ARTIST && i != HIT_PANEL_PLAY && i != HIT_SCROLL_PANEL) {
            continue;
        }
        bool search_result =
            (i >= HIT_SEARCH_TOP && i <= HIT_SEARCH_ALBUM_LAST) || (i >= HIT_ROW_FIRST && i <= HIT_ROW_LAST);

        if (!scene->home && scene->section == BROWSE_SEARCH && search_result &&
            (y < scene->content.y + 64 || y >= scene->content.y + scene->content.h || x < scene->content.x ||
             x >= scene->content.x + scene->content.w)) {
            continue;
        }

        if (!scene->home && scene->section != BROWSE_SEARCH && i >= HIT_ROW_FIRST && i <= HIT_ROW_LAST &&
            (y < scene->content.y || y >= scene->content.y + scene->content.h || x < scene->content.x ||
             x >= scene->content.x + scene->content.w)) {
            continue;
        }
        if (scene->setup_popup && !setup_hit(i)) {
            continue;
        }

        if (scene->device_popup && contains(scene->device_panel, x, y) && i != HIT_CLOSE_DEVICES &&
            i != HIT_TRANSFER && (i < HIT_DEVICE_FIRST || i > HIT_DEVICE_LAST)) {
            continue;
        }

        if (scene->hits[i].enabled && contains(scene->hits[i].rect, x, y)) {
            return (HitId)i;
        }
    }

    return HIT_NONE;
}

void scene_layout(Scene *scene, const View *view, float width, float height) {
    if (scene->session_revision != view->session_revision) {
        scene->session_revision = view->session_revision;
        scene->query[0] = 0;
        scene->home_scroll = 0;
        scene->search_scroll = 0;
        scene->main_scroll = (Scroll){0};
        scene->library_scroll = (Scroll){0};
        scene->panel_scroll = (Scroll){0};
        scene->popular_expanded = false;
        scene->popular_selected = 0;
        scene->panel = PANEL_NONE;
        scene->search_due = 0;
        scene->library_first = 0;
        scene->home = true;
        scene->device_popup = false;
        scene->setup_popup = false;
        scene->menu.open = false;
        scene->pressed = HIT_NONE;
        scene->dragging = HIT_NONE;
        scene->collection_artwork[0] = 0;
        scene->pending_browse = false;
        scene->retain_content = false;
        scene->entrance = 1;
    }

    if (scene->width != width || scene->height != height) {
        scene->retain_content = false;
        scene->entrance = 1;
    }

    scene->width = width;
    scene->height = height;
    scene->sidebar = (Rect){8, 64, width < 1100 ? 256 : 288, height - 160};
    scene->content = (Rect){scene->sidebar.w + 16, 64, width - scene->sidebar.w - 24, height - 160};
    scene->side_panel = (Rect){width - 328, 64, 320, height - 160};

    if (scene->panel != PANEL_NONE && width >= 1200) {
        scene->content.w -= 328;
    }
    scene->player = (Rect){0, height - 88, width, 88};
    scene->search = (Rect){width * .5f - 200, 8, 400, 48};
    bool was_login = scene->login_screen;
    scene->login_screen = !view->authenticated;

    if (was_login && !scene->login_screen) {
        scene->focus = HIT_SEARCH;
    }

    // The app asks for the setup panel when playback needs Soloist and Soloist needs the listener.
    if (scene->setup_revision != view->setup_revision) {
        scene->setup_revision = view->setup_revision;
        scene->setup_popup = true;
        scene->device_popup = false;
    }

    if (scene->pending_browse && !view->list.loading) {
        if (view->list.loaded) {
            scene->section = view->list.request.kind;
            snprintf(scene->collection, sizeof(scene->collection), "%s", view->list.request.title);
            snprintf(scene->collection_artwork, sizeof(scene->collection_artwork), "%s",
                     view->list.request.artwork);
        }
        scene->pending_browse = false;
        if (scene->entrance < 1) {
            scene->last_ms = platform_milliseconds();
            scene->animating = true;
        }
    }

    if (scene->dragging == HIT_NONE) {
        player_draft_sync(&scene->draft, &view->playback, view->playback_revision);
    }

    memset(scene->hits, 0, sizeof(scene->hits));

    if (scene->login_screen) {
        scene->login_panel = (Rect){width / 2 - 240, height / 2 - 224, 480, 448};
        scene->focus = HIT_ACCOUNT;
        hit(scene, HIT_ACCOUNT, (Rect){width / 2 - 160, scene->login_panel.y + 266, 320, 48},
            !view->connecting);
        return;
    }

    hit(scene, HIT_HOME, (Rect){scene->search.x - 56, 8, 48, 48}, true);
    hit(scene, HIT_SEARCH, scene->search, true);
    hit(scene, HIT_PROFILE, (Rect){width - 60, 14, 36, 36}, true);

    hit(scene, HIT_PLAYLISTS, (Rect){20, 114, 88, 30}, true);
    hit(scene, HIT_ALBUMS, (Rect){116, 114, 74, 30}, true);
    hit(scene, HIT_TRACKS, (Rect){16, 184, scene->sidebar.w - 16, 56}, true);
    hit(scene, HIT_QUEUE, (Rect){width - 238, height - 58, 28, 28}, true);
    hit(scene, HIT_NOW_PLAYING, (Rect){width - 274, height - 58, 28, 28}, true);
    hit(scene, HIT_LYRICS, (Rect){width - 310, height - 58, 28, 28}, true);

    float library_viewport = scene->sidebar.h > 202 ? scene->sidebar.h - 202 : 48;
    scroll_layout(&scene->library_scroll, view->playlists.count * 48.0f, library_viewport);
    scene->library_first = (unsigned)(scene->library_scroll.offset / 48.0f);

    if (scene->library_first > view->playlists.count) {
        scene->library_first = view->playlists.count;
    }

    float library_sub = scene->library_scroll.offset - scene->library_first * 48.0f;

    for (unsigned i = 0; i < 24 && scene->library_first + i < view->playlists.count; ++i) {
        Rect row = {16, 252 + i * 48 - library_sub, scene->sidebar.w - 16, 48};

        if (row.y + row.h > 252 && row.y < scene->sidebar.y + scene->sidebar.h - 14) {
            hit(scene, (HitId)(HIT_LIBRARY_FIRST + i), row, true);
        }
    }

    Rect library_thumb = scroll_thumb(
        &scene->library_scroll, (Rect){scene->sidebar.x + scene->sidebar.w - 10, 252, 6, library_viewport});
    hit(scene, HIT_SCROLL_LIBRARY, library_thumb, library_thumb.h > 0);
    app_library_viewport(scene->library_first);

    float x = scene->content.x + 24;
    float y = scene->content.y;

    if (scene->home) {
        home_scene_layout(scene, view);
    }

    if (!scene->home && scene->section == BROWSE_SEARCH) {
        search_scene_layout(scene, view);
        app_viewport(scene->first_row);
    }

    if (!scene->home && collection_page(scene->section)) {
        collection_scene_layout(scene, view);
    }

    if (!scene->home && scene->section != BROWSE_SEARCH && !collection_page(scene->section)) {
        bool grid = scene->section == BROWSE_SAVED_ALBUMS || scene->section == BROWSE_PLAYLISTS ||
                    scene->section == BROWSE_FOLLOWED_ARTISTS;
        unsigned count = view->list.count;
        bool selection_moved = view->list.selected != scene->last_selected;
        scene->last_selected = view->list.selected;
        unsigned shown = 0;

        if (grid) {
            unsigned columns = scene_columns(scene->content.w);
            float card_width = (scene->content.w - 48 - (columns - 1) * 8) / columns;
            float card_height = card_width + 72;
            unsigned rows = (count + columns - 1) / columns;
            scroll_layout(&scene->main_scroll, 100 + rows * card_height + 24, scene->content.h);
            scene->main_origin = 100;
            scene->main_row_height = card_height;
            scene->main_columns = columns;
            unsigned visible_rows =
                scene->content.h > 152 ? (unsigned)((scene->content.h - 112) / card_height) : 0;

            float row_offset = scene->main_scroll.offset > 100 ? scene->main_scroll.offset - 100 : 0;
            scene->first_row = (unsigned)(row_offset / card_height) * columns;

            if (scene->first_row > count) {
                scene->first_row = count;
            }

            if (selection_moved && view->list.selected >= 0 && visible_rows) {
                unsigned row = (unsigned)view->list.selected / columns;

                if ((unsigned)view->list.selected < scene->first_row) {
                    scene->first_row = row * columns;
                    scroll_to(&scene->main_scroll, 100 + row * card_height, 0, false);
                }
                if ((unsigned)view->list.selected >= scene->first_row + visible_rows * columns) {
                    scene->first_row = (row - visible_rows + 1) * columns;
                    scroll_to(&scene->main_scroll, 100 + (row - visible_rows + 1) * card_height, 0, false);
                }
            }

            float grid_y = y + 100 - scene->main_scroll.offset;

            for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < count; ++slot) {
                unsigned index = scene->first_row + slot;
                Rect rect = {x + (index % columns) * (card_width + 8),
                             grid_y + (index / columns) * card_height, card_width, card_height - 8};

                if (rect.y >= scene->content.y + scene->content.h) {
                    break;
                }

                bool visible = rect.y + rect.h > scene->content.y &&
                               rect.y + rect.h <= scene->content.y + scene->content.h - 8;
                hit(scene, (HitId)(HIT_ROW_FIRST + slot), rect, visible);
                shown = slot + 1;
            }
        }

        if (!grid) {
            scroll_layout(&scene->main_scroll, 280 + count * 56.0f + 24, scene->content.h);
            scene->main_origin = 280;
            scene->main_row_height = 56;
            scene->main_columns = 1;
            unsigned visible = scene->content.h > 296 ? (unsigned)(scene->content.h - 296) / 56 : 0;

            if (visible > ROW_SLOTS) {
                visible = ROW_SLOTS;
            }

            float row_offset = scene->main_scroll.offset > 280 ? scene->main_scroll.offset - 280 : 0;
            scene->first_row = (unsigned)(row_offset / 56.0f);

            if (scene->first_row > count) {
                scene->first_row = count;
            }

            if (selection_moved && view->list.selected >= 0 &&
                (unsigned)view->list.selected < scene->first_row) {
                scene->first_row = (unsigned)view->list.selected;
                scroll_to(&scene->main_scroll, 280 + scene->first_row * 56.0f, 0, false);
            }

            if (selection_moved && visible && view->list.selected >= 0 &&
                (unsigned)view->list.selected >= scene->first_row + visible) {
                scene->first_row = (unsigned)view->list.selected - visible + 1;
                scroll_to(&scene->main_scroll, 280 + scene->first_row * 56.0f, 0, false);
            }

            float list_y = y + 280 - scene->main_scroll.offset;

            for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < count; ++slot) {
                unsigned index = scene->first_row + slot;
                Rect rect = {x - 8, list_y + index * 56, scene->content.w - 32, 56};

                if (rect.y >= scene->content.y + scene->content.h) {
                    break;
                }

                bool row_visible = rect.y + rect.h > scene->content.y &&
                                   rect.y + rect.h <= scene->content.y + scene->content.h - 8;
                hit(scene, (HitId)(HIT_ROW_FIRST + slot), rect, row_visible);
                shown = slot + 1;
            }

            bool selected = view->list.selected >= 0 && (unsigned)view->list.selected < count;
            float controls_y = y + 192 - scene->main_scroll.offset;
            hit(scene, HIT_OPEN, (Rect){x, controls_y, 56, 56},
                selected && controls_y + 56 > scene->content.y &&
                    controls_y < scene->content.y + scene->content.h);
            hit(scene, HIT_LINK, (Rect){x + 76, controls_y + 12, 32, 32},
                selected && controls_y + 44 > scene->content.y &&
                    controls_y + 12 < scene->content.y + scene->content.h);
        }

        // Infinite scroll: fetch the next page well before the listener reaches the end.
        if (view->list.more && !view->list.loading && scene->first_row + shown + 2 * ROW_SLOTS >= count) {
            app_load_more();
        }

        app_viewport(scene->first_row);
    }

    float main_top = scene->home || (!scene->home && scene->section == BROWSE_SEARCH) ? scene->content.y + 68
                                                                                      : scene->content.y + 8;
    float main_track_h = scene->content.y + scene->content.h - 8 - main_top;
    Rect main_thumb = scroll_thumb(
        &scene->main_scroll, (Rect){scene->content.x + scene->content.w - 10, main_top, 6, main_track_h});
    hit(scene, HIT_SCROLL_MAIN, main_thumb, main_thumb.h > 0);

    hit(scene, HIT_REFRESH, (Rect){scene->sidebar.x + scene->sidebar.w - 48, 84, 24, 24}, true);
    hit(scene, HIT_PREVIOUS, (Rect){width / 2 - 76, height - 76, 28, 28},
        playback_can_command(&view->playback, "previous"));
    hit(scene, HIT_PLAY, (Rect){width / 2 - 18, height - 80, 36, 36}, true);
    hit(scene, HIT_SHUFFLE, (Rect){width / 2 - 126, height - 74, 24, 24},
        view->playback.has_device && !view->playback.restricted);
    hit(scene, HIT_REPEAT, (Rect){width / 2 + 102, height - 74, 24, 24},
        view->playback.has_device && !view->playback.restricted);
    hit(scene, HIT_NEXT, (Rect){width / 2 + 48, height - 76, 28, 28},
        playback_can_command(&view->playback, "next"));
    hit(scene, HIT_SEEK, (Rect){width / 2 - 180, height - 34, 360, 14}, playback_can_seek(&view->playback));
    hit(scene, HIT_DEVICES, (Rect){width - 202, height - 58, 28, 28}, true);
    hit(scene, HIT_VOLUME, (Rect){width - 130, height - 52, 104, 16}, playback_can_volume(&view->playback));

    panel_scene_layout(scene, view);

    if (scene->device_popup) {
        unsigned capacity = height > 318 ? (unsigned)((height - 276) / 42) : 1;
        if (capacity > 8) {
            capacity = 8;
        }
        scene->device_visible = capacity;

        if (scene->device_revision != view->devices_revision) {
            scene->device_revision = view->devices_revision;
            scene->device_first =
                view->devices.selected >= (int)capacity ? (unsigned)view->devices.selected - capacity + 1 : 0;
        }

        unsigned visible = view->devices.count < capacity ? view->devices.count : capacity;
        float list_height = visible ? visible * 42.0f : 72;
        scene->device_panel = (Rect){width - 346, height - 212 - list_height, 320, 112 + list_height};
        float popup_y = scene->device_panel.y;

        hit(scene, HIT_CLOSE_DEVICES, (Rect){width - 54, popup_y + 10, 24, 24}, true);

        for (unsigned i = scene->device_first; i < view->devices.count && i - scene->device_first < capacity;
             ++i) {
            hit(scene, (HitId)(HIT_DEVICE_FIRST + i),
                (Rect){width - 330, popup_y + 44 + (i - scene->device_first) * 42, 288, 40}, true);
        }

        // Until Soloist is ready this button opens its setup instead of transferring.
        hit(scene, HIT_TRANSFER, (Rect){width - 314, height - 152, 256, 36},
            view->engine != ENGINE_READY || device_can_transfer(devices_selected(&view->devices)));
    }

    if (scene->setup_popup) {
        Rect panel = {width / 2 - 230, height / 2 - 170, 460, 340};
        bool keyed = view->engine != ENGINE_NEEDS_KEY && view->engine != ENGINE_OFF;
        scene->setup_panel = panel;

        hit(scene, HIT_CLOSE_SETUP, (Rect){panel.x + panel.w - 44, panel.y + 16, 28, 28}, true);
        hit(scene, HIT_SETUP_KEY, (Rect){panel.x + 32, panel.y + 248, 186, 44}, true);
        hit(scene, HIT_SETUP_PASTE, (Rect){panel.x + 242, panel.y + 248, 186, 44}, true);
        hit(scene, HIT_SETUP_FORGET, (Rect){panel.x + panel.w / 2 - 70, panel.y + 300, 140, 28}, keyed);
    }

    if (scene->menu.open) {
        for (unsigned i = 0; i < scene->menu.count && i < 6; ++i) {
            hit(scene, (HitId)(HIT_CTX_FIRST + i),
                (Rect){scene->menu.rect.x + 4, scene->menu.rect.y + 6 + i * 34, scene->menu.rect.w - 8, 32},
                true);
        }
    }
}

void scene_hover(Scene *scene, HitId target, uint64_t now) {
    if (scene->hot == target) {
        return;
    }

    if (scene->hot != HIT_NONE && scene->hover[scene->hot] > .25f) {
        scene->hover[scene->hot] = .25f;
    }

    scene->hot = target;

    if (target != HIT_NONE && scene->hover[target] < .65f) {
        scene->hover[target] = .65f;
    }

    if (!scene->animating) {
        scene->last_ms = now;
    }
    scene->animating = true;
}

bool scene_tick(Scene *scene, uint64_t now) {
    uint64_t elapsed = now > scene->last_ms ? now - scene->last_ms : 0;
    scene->last_ms = now;
    if (elapsed > 64) {
        elapsed = 64;
    }

    if (scene->search_due && now >= scene->search_due) {
        scene->search_due = 0;

        if (!scene->login_screen && !scene->setup_popup && scene->query[0]) {
            scene_search(scene);
        }
    }

    bool main_moving = scroll_tick(&scene->main_scroll, now);
    bool library_moving = scroll_tick(&scene->library_scroll, now);
    bool panel_moving = scroll_tick(&scene->panel_scroll, now);
    bool active = scene->search_due != 0 || main_moving || library_moving || panel_moving;

    if (library_moving) {
        scene->library_first = (unsigned)(scene->library_scroll.offset / 48.0f);
        app_library_viewport(scene->library_first);
    }

    if (panel_moving) {
        scene->panel_first = (unsigned)(scene->panel_scroll.offset / 56.0f);
    }

    if (main_moving && !scene->home &&
        !(scene->section == BROWSE_SEARCH && scene->search_filter == SEARCH_ALL) &&
        scene->main_row_height > 0 && scene->main_columns > 0) {
        float row_offset = scene->main_scroll.offset > scene->main_origin
                               ? scene->main_scroll.offset - scene->main_origin
                               : 0;
        scene->first_row = (unsigned)(row_offset / scene->main_row_height) * scene->main_columns;
        app_viewport(scene->first_row);
    }

    if (scene->home) {
        scene->home_scroll = scene->main_scroll.offset;
    }

    if (!scene->home && scene->section == BROWSE_SEARCH) {
        scene->search_scroll = scene->main_scroll.offset;
    }

    for (unsigned i = 1; i < HIT_COUNT; ++i) {
        float target = scene->hot == (HitId)i ? 1 : 0;
        float difference = target - scene->hover[i];

        if (difference > -.01f && difference < .01f) {
            scene->hover[i] = target;
            continue;
        }

        float step = target > scene->hover[i] ? (float)elapsed / 55.0f : (float)elapsed / 24.0f;
        scene->hover[i] = difference > 0 ? (difference <= step ? target : scene->hover[i] + step)
                                         : (-difference <= step ? target : scene->hover[i] - step);
        active = scene->hover[i] != target || active;
    }

    scene->animating = active;

    return active;
}

static void open_section(Scene *scene, BrowseKind kind) {
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

const CatalogItem *scene_home_item(const View *view, unsigned index) {
    if (index < 5 && index < view->library[1].count) {
        return &view->library[1].items[index];
    }
    if (index >= 5 && index < 10 && index - 5 < view->library[2].count) {
        return &view->library[2].items[index - 5];
    }

    return NULL;
}

static void open_collection(Scene *scene, const CatalogItem *item) {
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

void scene_activate(Scene *scene, const View *view, HitId id) {
    if (id >= HIT_CTX_FIRST && id <= HIT_CTX_LAST) {
        unsigned idx = (unsigned)(id - HIT_CTX_FIRST);
        scene->menu.open = false;

        if (idx >= scene->menu.count) {
            return;
        }

        CatalogItem item = scene->menu.item;

        switch (scene->menu.actions[idx]) {
        case CTX_PLAY:
            if (item.kind == ITEM_ALBUM || item.kind == ITEM_ARTIST || item.kind == ITEM_PLAYLIST) {
                open_collection(scene, &item);
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
                open_collection(scene, &album);
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
                open_collection(scene, &artist);
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
            open_section(scene, BROWSE_PROFILE_PLAYLISTS);
            return;
        case CTX_LOGOUT:
            scene_activate(scene, view, HIT_ACCOUNT);
            return;
        default:
            return;
        }
    }

    if (id >= HIT_HOME_QUICK_FIRST && id <= HIT_HOME_QUICK_LAST) {
        unsigned index = (unsigned)(id - HIT_HOME_QUICK_FIRST);

        if (index < view->recent_count) {
            open_collection(scene, &view->recent[index]);
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
            open_section(scene, shortcuts[index]);
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
            open_collection(scene, item);
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
            open_collection(scene, item);
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
            open_collection(scene, item);
        }

        return;
    }

    if (id >= HIT_CARD_FIRST && id <= HIT_CARD_LAST) {
        const CatalogItem *item = scene_home_item(view, (unsigned)(id - HIT_CARD_FIRST));

        if (item) {
            open_collection(scene, item);
            return;
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
            open_collection(scene, &view->profile.following[index]);
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
        open_section(scene, BROWSE_SAVED_ALBUMS);
        return;
    case HIT_HOME_PLAYLISTS:
        open_section(scene, BROWSE_PLAYLISTS);
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
        open_section(scene, BROWSE_PROFILE_PLAYLISTS == scene->section ? BROWSE_FOLLOWED_ARTISTS
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
        open_collection(scene, &view->details.artist);
        return;
    case HIT_DETAIL_ALL:
        scene->popular_expanded = !scene->popular_expanded;
        return;
    case HIT_TRACKS:
    case HIT_MUSIC:
        open_section(scene, BROWSE_SAVED_TRACKS);
        return;
    case HIT_ALBUMS:
        open_section(scene, BROWSE_SAVED_ALBUMS);
        return;
    case HIT_PLAYLISTS:
        open_section(scene, BROWSE_PLAYLISTS);
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
        open_section(scene, BROWSE_QUEUE);
        return;
    case HIT_PANEL_PLAY:
        app_action(view->playback.playing ? ACTION_PAUSE : ACTION_PLAY);
        return;
    case HIT_PANEL_ARTIST:
        if (spotify_id_valid(view->playback.artist_id)) {
            CatalogItem artist = {.kind = ITEM_ARTIST};
            copy_utf8(artist.id, sizeof(artist.id), view->playback.artist_id);
            copy_utf8(artist.title, sizeof(artist.title), view->playback.artist);
            open_collection(scene, &artist);
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

static Rect scrollbar_track(const Scene *scene, HitId id) {
    if (id == HIT_SCROLL_LIBRARY) {
        float h = scene->sidebar.h > 202 ? scene->sidebar.h - 202 : 48;
        return (Rect){scene->sidebar.x + scene->sidebar.w - 10, 252, 6, h};
    }

    if (id == HIT_SCROLL_PANEL) {
        float h = scene->side_panel.h > 224 ? scene->side_panel.h - 224 : 56;
        return (Rect){scene->side_panel.x + scene->side_panel.w - 10, scene->side_panel.y + 164, 6, h};
    }

    float top = scene->home                                         ? scene->content.y + 60
                : (!scene->home && scene->section == BROWSE_SEARCH) ? scene->content.y + 68
                                                                    : scene->content.y + 8;
    return (Rect){scene->content.x + scene->content.w - 10, top, 6,
                  scene->content.y + scene->content.h - 8 - top};
}

static Scroll *scrollbar_state(Scene *scene, HitId id) {
    return id == HIT_SCROLL_LIBRARY ? &scene->library_scroll
           : id == HIT_SCROLL_PANEL ? &scene->panel_scroll
                                    : &scene->main_scroll;
}

void scene_motion(Scene *scene, float x) {
    if (scene->dragging != HIT_SEEK && scene->dragging != HIT_VOLUME) {
        return;
    }

    Rect rect = scene->hits[scene->dragging].rect;
    float fraction = (x - rect.x) / rect.w;
    if (fraction < 0) {
        fraction = 0;
    }
    if (fraction > 1) {
        fraction = 1;
    }

    if (scene->dragging == HIT_SEEK) {
        scene->draft.seek = (unsigned)(fraction * 1000);
    }
    if (scene->dragging == HIT_VOLUME) {
        scene->draft.volume = (unsigned)(fraction * 100);
    }
}

void scene_pointer_motion(Scene *scene, float x, float y) {
    scene_motion(scene, x);

    if (scene->dragging < HIT_SCROLL_MAIN || scene->dragging > HIT_SCROLL_PANEL) {
        return;
    }

    Scroll *scroll = scrollbar_state(scene, scene->dragging);
    scroll_drag(scroll, scrollbar_track(scene, scene->dragging), y, scene->scroll_grab, 0);

    if (scene->dragging == HIT_SCROLL_LIBRARY) {
        scene->library_first = (unsigned)(scroll->offset / 48.0f);
        app_library_viewport(scene->library_first);
        return;
    }

    if (scene->dragging == HIT_SCROLL_PANEL) {
        scene->panel_first = (unsigned)(scroll->offset / 56.0f);
        return;
    }

    if (scene->home) {
        scene->home_scroll = scroll->offset;
        return;
    }

    if (scene->section == BROWSE_SEARCH) {
        scene->search_scroll = scroll->offset;
    }

    if (scene->main_row_height > 0 && scene->main_columns > 0) {
        float origin = scene->main_origin;
        float row_offset = scroll->offset > origin ? scroll->offset - origin : 0;
        scene->first_row = (unsigned)(row_offset / scene->main_row_height) * scene->main_columns;
        app_viewport(scene->first_row);
    }
}

void scene_press(Scene *scene, const View *view, float x, float y, uint64_t now) {
    (void)view;

    if (scene->menu.open && !contains(scene->menu.rect, x, y)) {
        scene->menu.open = false;
        return;
    }

    HitId target = scene_hit(scene, x, y);

    if (scene->device_popup && !contains(scene->device_panel, x, y) && target != HIT_DEVICES) {
        scene->device_popup = false;
        return;
    }

    if (scene->setup_popup && !contains(scene->setup_panel, x, y)) {
        scene->setup_popup = false;
        return;
    }

    if (target < HIT_SCROLL_MAIN || target > HIT_SCROLL_PANEL) {
        HitId tracks[] = {HIT_SCROLL_MAIN, HIT_SCROLL_LIBRARY, HIT_SCROLL_PANEL};

        for (unsigned i = 0; i < 3; ++i) {
            HitId id = tracks[i];
            Rect track = scrollbar_track(scene, id);

            if (scene->hits[id].enabled && x >= track.x - 6 && x <= track.x + track.w + 4 && y >= track.y &&
                y <= track.y + track.h) {
                target = id;
                break;
            }
        }
    }

    scene->pressed = target;
    scene->double_click =
        scene->last_click == target && now >= scene->click_ms && now - scene->click_ms < 350;
    scene->last_click = target;
    scene->click_ms = now;
    scene->focus = target;
    scene_hover(scene, target, now);

    if (target == HIT_SEEK || target == HIT_VOLUME) {
        scene->dragging = target;
        scene_motion(scene, x);
        return;
    }

    if (target >= HIT_SCROLL_MAIN && target <= HIT_SCROLL_PANEL) {
        Rect thumb = scene->hits[target].rect;
        scene->dragging = target;
        scene->scroll_grab = y >= thumb.y && y <= thumb.y + thumb.h ? y - thumb.y : thumb.h * .5f;
        scene_pointer_motion(scene, x, y);
        return;
    }
}

static bool item_at(const Scene *scene, const View *view, float x, float y, CatalogItem *out) {
    HitId id = HIT_NONE;

    for (int i = HIT_COUNT - 1; i > 0; --i) {
        if (scene->hits[i].enabled && contains(scene->hits[i].rect, x, y)) {
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

    bool in_now_playing = (scene->panel == PANEL_NOW_PLAYING && contains(scene->side_panel, x, y)) ||
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

    if (scene->panel == PANEL_QUEUE && contains(scene->side_panel, x, y)) {
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

void scene_release(Scene *scene, const View *view, float x, float y) {
    if (scene->dragging == HIT_SEEK) {
        app_seek(scene->draft.seek);
    }
    if (scene->dragging == HIT_VOLUME) {
        app_volume(scene->draft.volume);
    }

    if (scene->dragging == HIT_NONE && scene->pressed == scene_hit(scene, x, y)) {
        scene_activate(scene, view, scene->pressed);

        if (scene->double_click && scene->pressed >= HIT_ROW_FIRST && scene->pressed <= HIT_ROW_LAST) {
            const CatalogItem *item =
                list_row(&view->list, scene->first_row + (unsigned)(scene->pressed - HIT_ROW_FIRST));

            if (item && item_can_play(item)) {
                app_open_item(item);
            }
        }
    }

    scene->dragging = HIT_NONE;
    scene->pressed = HIT_NONE;
}

void scene_query_changed(Scene *scene) {
    scene->search_due = scene->query[0] ? platform_milliseconds() + 180 : 0;
    scene->animating = scene->search_due != 0 || scene->animating;

    if (!scene->query[0] && scene->section == BROWSE_SEARCH) {
        scene->home = true;
        scene->pending_browse = false;
    }
}

bool scene_input(Scene *scene, const char *text, size_t size) {
    char query[SEARCH_CAPACITY] = {0};
    if (!scene->select_all) {
        snprintf(query, sizeof(query), "%s", scene->query);
    }

    if (!search_append(query, text, size)) {
        return false;
    }

    memcpy(scene->query, query, sizeof(query));
    scene->select_all = false;
    scene_query_changed(scene);

    return true;
}

bool scene_key(Scene *scene, const View *view, SceneKey key, bool shift) {
    if (key == KEY_ESCAPE) {
        if (scene->menu.open) {
            scene->menu.open = false;
            return true;
        }
        scene->panel = PANEL_NONE;
        app_queue_visible(false);
        scene->device_popup = false;
        scene->setup_popup = false;
        scene->focus = HIT_NONE;
        return true;
    }

    if (key == KEY_TAB) {
        for (unsigned i = 1; i < HIT_COUNT; ++i) {
            int next = ((int)scene->focus + (shift ? -(int)i : (int)i) + HIT_COUNT) % HIT_COUNT;

            if (scene->hits[next].enabled) {
                scene->focus = (HitId)next;
                return true;
            }
        }
    }

    if (scene->focus == HIT_SEARCH) {
        if (key == KEY_ENTER) {
            scene_search(scene);
        }

        if (key == KEY_BACKSPACE) {
            if (scene->select_all) {
                scene->query[0] = 0;
            }
            if (!scene->select_all) {
                search_backspace(scene->query);
            }
            scene->select_all = false;
            scene_query_changed(scene);
        }

        return true;
    }

    if ((scene->focus == HIT_SEEK || scene->focus == HIT_VOLUME) && (key == KEY_LEFT || key == KEY_RIGHT)) {
        unsigned *value = scene->focus == HIT_SEEK ? &scene->draft.seek : &scene->draft.volume;
        unsigned maximum = scene->focus == HIT_SEEK ? 1000 : 100;
        int next = (int)*value + (key == KEY_LEFT ? -5 : 5);
        *value = next < 0 ? 0 : (unsigned)next > maximum ? maximum : (unsigned)next;
        return true;
    }

    if ((key == KEY_UP || key == KEY_DOWN) && !scene->home && view->list.count) {
        unsigned count =
            scene->section == BROWSE_SEARCH && scene->search_filter == SEARCH_ALL && view->list.count > 4
                ? 4
                : view->list.count;
        int next = view->list.selected + (key == KEY_UP ? -1 : 1);

        if (next < 0) {
            next = 0;
        }
        if ((unsigned)next >= count) {
            next = (int)count - 1;
        }

        app_select(next);

        // Layout scrolls the selection into view; focus follows its slot.
        unsigned slot = (unsigned)next >= scene->first_row ? (unsigned)next - scene->first_row : 0;
        scene->focus = (HitId)(HIT_ROW_FIRST + (slot < ROW_SLOTS ? slot : ROW_SLOTS - 1));
        return true;
    }

    if (key == KEY_ENTER || key == KEY_SPACE) {
        if (scene->focus == HIT_SEEK) {
            app_seek(scene->draft.seek);
        }
        if (scene->focus == HIT_VOLUME) {
            app_volume(scene->draft.volume);
        }

        if (scene->focus >= HIT_ROW_FIRST && scene->focus <= HIT_ROW_LAST) {
            const CatalogItem *item =
                list_row(&view->list, scene->first_row + (unsigned)(scene->focus - HIT_ROW_FIRST));

            if (item &&
                (item->kind == ITEM_ALBUM || item->kind == ITEM_PLAYLIST || item->kind == ITEM_ARTIST)) {
                scene_activate(scene, view, scene->focus);
                return true;
            }

            app_activate();
        }
        if (scene->focus < HIT_ROW_FIRST || scene->focus > HIT_ROW_LAST) {
            scene_activate(scene, view, scene->focus);
        }
        return true;
    }

    return false;
}

void scene_scroll(Scene *scene, const View *view, int direction) {
    if (scene->setup_popup) {
        return;
    }

    if ((scene->hot == HIT_VOLUME || scene->focus == HIT_VOLUME) && scene->hits[HIT_VOLUME].enabled) {
        int next = (int)scene->draft.volume + (direction < 0 ? 5 : -5);
        scene->draft.volume = next < 0 ? 0 : next > 100 ? 100 : (unsigned)next;
        app_volume(scene->draft.volume);
        return;
    }

    uint64_t now = scene->last_ms ? scene->last_ms : platform_milliseconds();
    bool smooth = scene->last_ms != 0;

    if (scene->over_panel && scene->panel != PANEL_NONE && !scene->device_popup) {
        if (scene->panel == PANEL_QUEUE) {
            float target =
                (scene->panel_scroll.active ? scene->panel_scroll.target : scene->panel_scroll.offset) +
                (direction < 0 ? -112.0f : 112.0f);
            scroll_to(&scene->panel_scroll, target, now, smooth);
            scene->panel_first = (unsigned)(scene->panel_scroll.offset / 56.0f);

            if (scene->panel_scroll.active) {
                scene->last_ms = now;
                scene->animating = true;
            }
        }

        return;
    }

    if (scene->over_library && !scene->device_popup) {
        float target =
            (scene->library_scroll.active ? scene->library_scroll.target : scene->library_scroll.offset) +
            (direction < 0 ? -144.0f : 144.0f);
        scroll_to(&scene->library_scroll, target, now, smooth);
        scene->library_first = (unsigned)(scene->library_scroll.offset / 48.0f);
        app_library_viewport(scene->library_first);

        if (scene->library_scroll.active) {
            scene->last_ms = now;
            scene->animating = true;
        }

        return;
    }

    if (scene->device_popup) {
        if (direction < 0 && scene->device_first) {
            --scene->device_first;
        }
        if (direction > 0 && scene->device_first + scene->device_visible < view->devices.count) {
            ++scene->device_first;
        }
        return;
    }

    if (scene->home) {
        float target = (scene->main_scroll.active ? scene->main_scroll.target : scene->main_scroll.offset) +
                       (direction < 0 ? -96.0f : 96.0f);
        scroll_to(&scene->main_scroll, target, now, smooth);
        scene->home_scroll = scene->main_scroll.offset;

        if (scene->main_scroll.active) {
            scene->last_ms = now;
            scene->animating = true;
        }

        return;
    }

    if (scene->section == BROWSE_SEARCH && scene->search_filter == SEARCH_ALL) {
        float target = (scene->main_scroll.active ? scene->main_scroll.target : scene->main_scroll.offset) +
                       (direction < 0 ? -96.0f : 96.0f);
        scroll_to(&scene->main_scroll, target, now, smooth);
        scene->search_scroll = scene->main_scroll.offset;

        if (scene->main_scroll.active) {
            scene->last_ms = now;
            scene->animating = true;
        }

        return;
    }

    if (!view->list.count && !collection_page(scene->section)) {
        return;
    }

    float delta = scene->main_columns > 1 ? scene->main_row_height
                                          : 3 * (scene->main_row_height > 0 ? scene->main_row_height : 56.0f);
    float target = (scene->main_scroll.active ? scene->main_scroll.target : scene->main_scroll.offset) +
                   (direction < 0 ? -delta : delta);
    scroll_to(&scene->main_scroll, target, now, smooth);

    if (scene->main_row_height > 0 && scene->main_columns > 0) {
        float origin = scene->main_origin;
        float row_offset = scene->main_scroll.offset > origin ? scene->main_scroll.offset - origin : 0;
        scene->first_row = (unsigned)(row_offset / scene->main_row_height) * scene->main_columns;

        if (scene->first_row > view->list.count) {
            scene->first_row = view->list.count;
        }

        app_viewport(scene->first_row);
    }

    if (scene->main_scroll.active) {
        scene->last_ms = now;
        scene->animating = true;
    }
}

void scene_scrollbars(Scene *scene, Canvas *canvas) {
    HitId bars[] = {HIT_SCROLL_MAIN, HIT_SCROLL_LIBRARY, HIT_SCROLL_PANEL};

    for (unsigned i = 0; i < 3; ++i) {
        HitId id = bars[i];

        if (!scene->hits[id].enabled) {
            continue;
        }

        Rect thumb = scene->hits[id].rect;
        bool active = scene->dragging == id || scene->hot == id;
        canvas->box(canvas->context, thumb, thumb.w / 2,
                    active ? 0xa0a0a0 : color_mix(0x585858, 0x888888, scene->hover[id]));
    }
}
