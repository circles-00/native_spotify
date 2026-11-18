#include "scene_internal.h"
#include "collection_scene.h"
#include "home_scene.h"
#include "panel_scene.h"
#include "platform.h"
#include "search_scene.h"

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

bool scene_contains(Rect rect, float x, float y) {
    return rect.w > 0 && rect.h > 0 && x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
           y < rect.y + rect.h;
}

static bool setup_hit(int id) {
    return id == HIT_SETUP_KEY || id == HIT_SETUP_PASTE || id == HIT_SETUP_FORGET || id == HIT_CLOSE_SETUP;
}

HitId scene_hit(const Scene *scene, float x, float y) {
    if (scene->menu.open) {
        for (int i = HIT_CTX_LAST; i >= HIT_CTX_FIRST; --i) {
            if (scene->hits[i].enabled && scene_contains(scene->hits[i].rect, x, y)) {
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
            scene_contains(scene->side_panel, x, y) && i != HIT_PANEL_CLOSE && i != HIT_PANEL_QUEUE &&
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

        if (scene->device_popup && scene_contains(scene->device_panel, x, y) && i != HIT_CLOSE_DEVICES &&
            i != HIT_TRANSFER && (i < HIT_DEVICE_FIRST || i > HIT_DEVICE_LAST)) {
            continue;
        }

        if (scene->hits[i].enabled && scene_contains(scene->hits[i].rect, x, y)) {
            return (HitId)i;
        }
    }

    return HIT_NONE;
}

static void layout_main_list(Scene *scene, const View *view) {
    float x = scene->content.x + 24;
    float y = scene->content.y;
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
            Rect rect = {x + (index % columns) * (card_width + 8), grid_y + (index / columns) * card_height,
                         card_width, card_height - 8};

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

        if (selection_moved && view->list.selected >= 0 && (unsigned)view->list.selected < scene->first_row) {
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

static void layout_popups(Scene *scene, const View *view, float width, float height) {
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
        layout_main_list(scene, view);
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
    layout_popups(scene, view, width, height);
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

const CatalogItem *scene_home_item(const View *view, unsigned index) {
    if (index < 5 && index < view->library[1].count) {
        return &view->library[1].items[index];
    }

    if (index >= 5 && index < 10 && index - 5 < view->library[2].count) {
        return &view->library[2].items[index - 5];
    }

    return NULL;
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
