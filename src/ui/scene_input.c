#include "scene_internal.h"
#include "collection_scene.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

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

    if (scene->menu.open && !scene_contains(scene->menu.rect, x, y)) {
        scene->menu.open = false;
        return;
    }

    HitId target = scene_hit(scene, x, y);

    if (scene->device_popup && !scene_contains(scene->device_panel, x, y) && target != HIT_DEVICES) {
        scene->device_popup = false;
        return;
    }

    if (scene->setup_popup && !scene_contains(scene->setup_panel, x, y)) {
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
