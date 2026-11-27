#include "controller.h"
#include "artwork_cache.h"
#include "scene_internal.h"
#include "ui.h"
#include <stdlib.h>

static bool needs_resnapshot(const Scene *scene, const View *view) {
    return scene->first_row < view->list.first ||
           (scene->first_row + ROW_SLOTS > view->list.first + view->list.rows_count &&
            view->list.first + view->list.rows_count < view->list.count) ||
           scene->library_first < view->playlists.first ||
           (scene->library_first + 24 > view->playlists.first + view->playlists.rows_count &&
            view->playlists.first + view->playlists.rows_count < view->playlists.count);
}

static bool queue_needs_resnapshot(const Scene *scene, const View *view) {
    if (scene->panel != PANEL_QUEUE || scene->panel_recent || scene->panel_first >= view->queue.total) {
        return false;
    }

    unsigned last = scene->panel_first + QUEUE_VISIBLE_SLOTS - 1;

    if (last >= view->queue.total) {
        last = view->queue.total - 1;
    }

    return !queue_row(&view->queue, scene->panel_first) || !queue_row(&view->queue, last);
}

static View *controller_snapshot(UiController *controller) {
    if (!controller->snapshot) {
        controller->snapshot = calloc(1, sizeof(*controller->snapshot));

        if (!controller->snapshot) {
            return NULL;
        }
    }

    app_snapshot_into(controller->snapshot);
    return controller->snapshot;
}

void ui_controller_init(UiController *controller, uint64_t now) {
    scene_init(&controller->scene);
    controller->snapshot = NULL;
    controller->list_revision = 0;
    controller->last_home = controller->scene.home;
    controller->scene.last_ms = now;
}

void ui_controller_destroy(UiController *controller) {
    if (!controller) {
        return;
    }

    free(controller->snapshot);
    controller->snapshot = NULL;
}

bool ui_controller_is_editing(const UiController *controller) {
    if (controller->scene.intro_screen || controller->scene.lyrics_fullscreen) {
        return false;
    }

    if (controller->scene.login_screen) {
        return controller->scene.focus == HIT_CLIENT_ID;
    }

    return controller->scene.focus == HIT_SEARCH || controller->scene.focus == HIT_LIST_FILTER;
}

UiCursor ui_controller_cursor(const UiController *controller) {
    // A resize keeps its cursor while the pointer runs ahead of the edge.
    if (scene_resize_hit(controller->scene.dragging) || scene_resize_hit(controller->scene.hot)) {
        return UI_CURSOR_RESIZE;
    }

    if (controller->scene.hot == HIT_SEARCH || controller->scene.hot == HIT_LIST_FILTER ||
        controller->scene.hot == HIT_CLIENT_ID) {
        return UI_CURSOR_IBEAM;
    }

    if (controller->scene.hot != HIT_NONE) {
        return UI_CURSOR_HAND;
    }

    return UI_CURSOR_ARROW;
}

void ui_controller_layout(UiController *controller, float width, float height) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return;
    }

    artwork_reset(view->session_revision);
    scene_layout(&controller->scene, view, width, height);

    if (queue_needs_resnapshot(&controller->scene, view)) {
        app_snapshot_into(view);
        scene_layout(&controller->scene, view, width, height);
    }
}

void ui_controller_draw(UiController *controller, float width, float height, Canvas *canvas) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return;
    }

    artwork_reset(view->session_revision);
    scene_layout(&controller->scene, view, width, height);

    bool queue_changed = queue_needs_resnapshot(&controller->scene, view);

    if (queue_changed || needs_resnapshot(&controller->scene, view)) {
        app_snapshot_into(view);
    }

    if (queue_changed) {
        scene_layout(&controller->scene, view, width, height);
    }

    scene_draw(&controller->scene, view, canvas);
    bool view_changed =
        controller->list_revision != view->list.revision || controller->last_home != controller->scene.home;
    controller->list_revision = view->list.revision;
    controller->last_home = controller->scene.home;
    artwork_prune(view_changed);
}

bool ui_controller_tick(UiController *controller, uint64_t now) {
    if (!controller->scene.animating) {
        return false;
    }

    return scene_tick(&controller->scene, now);
}

void ui_controller_scroll(UiController *controller, float x, float y, int direction, uint64_t now) {
    ui_controller_wheel(controller, x, y, (float)direction, 3, false, now);
}

void ui_controller_wheel(UiController *controller, float x, float y, float notches, unsigned lines, bool page,
                         uint64_t now) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return;
    }

    Scene *scene = &controller->scene;

    if (scene->animating) {
        scene_tick(scene, now);
    }

    scene->last_ms = now;
    scene_hover(scene, scene_hit(scene, x, y), now);
    scene->over_search_history = scene_contains(scene->search_history_panel, x, y);
    scene->over_panel = scene->panel != PANEL_NONE && scene_contains(scene->side_panel, x, y);
    scene->over_library =
        x < scene->sidebar.x + scene->sidebar.w && y >= scene->sidebar.y && y < scene->player.y;
    scene_wheel(scene, view, notches, lines, page);
}

bool ui_controller_motion(UiController *controller, float x, float y, uint64_t now, bool *cursor_changed) {
    Scene *scene = &controller->scene;
    HitId target = scene_hit(scene, x, y);
    bool hot_changed = scene->hot != target;
    bool changed = hot_changed || scene->dragging != HIT_NONE;

    if (scene->dragging != HIT_NONE) {
        scene_pointer_motion(scene, x, y);
    }

    scene_hover(scene, target, now);

    if (cursor_changed) {
        *cursor_changed = hot_changed;
    }

    return changed;
}

void ui_controller_leave(UiController *controller, uint64_t now) {
    scene_hover(&controller->scene, HIT_NONE, now);
}

bool ui_controller_press(UiController *controller, float x, float y, uint64_t now) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return false;
    }

    scene_press(&controller->scene, view, x, y, now);
    return controller->scene.dragging != HIT_NONE;
}

void ui_controller_release(UiController *controller, float x, float y, uint64_t now) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return;
    }

    scene_pointer_motion(&controller->scene, x, y);
    scene_release(&controller->scene, view, x, y);

    if (controller->scene.entrance == 0) {
        controller->scene.last_ms = now;
    }
}

void ui_controller_context_press(UiController *controller, float x, float y) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return;
    }

    scene_context_press(&controller->scene, view, x, y);
}

UiShortcutResult ui_controller_shortcut(UiController *controller, char key, bool ctrl) {
    if (!ctrl) {
        return UI_SHORTCUT_NONE;
    }

    if (key >= 'A' && key <= 'Z') {
        key = (char)(key - 'A' + 'a');
    }

    Scene *scene = &controller->scene;

    if (key == 'l') {
        if (!scene->login_screen && !scene->lyrics_fullscreen) {
            scene->focus = HIT_SEARCH;
            scene_select_input_all(scene);
            scene->search_history_open = true;
        }

        return UI_SHORTCUT_HANDLED;
    }

    if (scene->setup_popup && key == 'v') {
        ui_paste_engine_key();
        return UI_SHORTCUT_HANDLED;
    }

    if (ui_controller_is_editing(controller)) {
        if (key == 'a') {
            scene_select_input_all(scene);
            return UI_SHORTCUT_HANDLED;
        }

        if (key == 'c') {
            return UI_SHORTCUT_COPY_SEARCH;
        }

        if (key == 'x') {
            return UI_SHORTCUT_CUT_SEARCH;
        }

        if (key == 'u' && scene->focus == HIT_CLIENT_ID) {
            scene->client_id[0] = 0;
            scene->client_id_input = (InputState){0};
            scene->select_all = false;
            return UI_SHORTCUT_HANDLED;
        }

        if (key == 'u' && scene->focus == HIT_LIST_FILTER) {
            scene->list_filter[0] = 0;
            scene->filter_input = (InputState){0};
            scene->select_all = false;
            app_list_filter("");
            return UI_SHORTCUT_HANDLED;
        }

        if (key == 'u') {
            scene->query[0] = 0;
            scene->search_input = (InputState){0};
            scene->select_all = false;
            scene_query_changed(scene);
            return UI_SHORTCUT_HANDLED;
        }

        if (key == 'v') {
            return UI_SHORTCUT_PASTE_SEARCH;
        }
    }

    return UI_SHORTCUT_NONE;
}

bool ui_controller_copy(UiController *controller, char *out, size_t capacity, bool cut) {
    if (!ui_controller_is_editing(controller)) {
        return false;
    }

    return scene_copy(&controller->scene, out, capacity, cut);
}

bool ui_controller_key_mods(UiController *controller, SceneKey key, bool shift, bool ctrl, uint64_t now) {
    View *view = controller_snapshot(controller);

    if (!view) {
        return false;
    }

    bool handled = scene_key_mods(&controller->scene, view, key, shift, ctrl);

    if (controller->scene.entrance == 0) {
        controller->scene.last_ms = now;
    }

    return handled;
}

bool ui_controller_key(UiController *controller, SceneKey key, bool shift, uint64_t now) {
    return ui_controller_key_mods(controller, key, shift, false, now);
}

bool ui_controller_text(UiController *controller, const char *utf8, size_t size) {
    if (!ui_controller_is_editing(controller)) {
        return false;
    }

    return scene_input(&controller->scene, utf8, size);
}
