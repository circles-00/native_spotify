#include "controller.h"
#include "artwork_cache.h"
#include "ui.h"

static bool needs_resnapshot(const Scene *scene, const View *view) {
    return scene->first_row < view->list.first ||
           (scene->first_row + ROW_SLOTS > view->list.first + view->list.rows_count &&
            view->list.first + view->list.rows_count < view->list.count) ||
           scene->library_first < view->playlists.first ||
           (scene->library_first + 24 > view->playlists.first + view->playlists.rows_count &&
            view->playlists.first + view->playlists.rows_count < view->playlists.count);
}

void ui_controller_init(UiController *controller, uint64_t now) {
    scene_init(&controller->scene);
    controller->scene.last_ms = now;
}

bool ui_controller_is_editing(const UiController *controller) {
    return !controller->scene.login_screen && controller->scene.focus == HIT_SEARCH;
}

UiCursor ui_controller_cursor(const UiController *controller) {
    if (controller->scene.hot == HIT_SEARCH) {
        return UI_CURSOR_IBEAM;
    }

    if (controller->scene.hot != HIT_NONE) {
        return UI_CURSOR_HAND;
    }

    return UI_CURSOR_ARROW;
}

void ui_controller_layout(UiController *controller, float width, float height) {
    View view = app_snapshot();
    artwork_reset(view.session_revision);
    scene_layout(&controller->scene, &view, width, height);
}

void ui_controller_draw(UiController *controller, float width, float height, Canvas *canvas) {
    View view = app_snapshot();
    artwork_reset(view.session_revision);
    scene_layout(&controller->scene, &view, width, height);

    if (needs_resnapshot(&controller->scene, &view)) {
        view = app_snapshot();
    }

    scene_draw(&controller->scene, &view, canvas);
}

bool ui_controller_tick(UiController *controller, uint64_t now) {
    if (!controller->scene.animating) {
        return false;
    }

    return scene_tick(&controller->scene, now);
}

void ui_controller_scroll(UiController *controller, float x, float y, int direction, uint64_t now) {
    View view = app_snapshot();
    Scene *scene = &controller->scene;

    if (scene->animating) {
        scene_tick(scene, now);
    }

    scene->last_ms = now;
    scene->hot = scene_hit(scene, x, y);
    scene->over_panel = scene->panel != PANEL_NONE && x >= scene->side_panel.x && y >= scene->side_panel.y &&
                        y < scene->side_panel.y + scene->side_panel.h;
    scene->over_library =
        x < scene->sidebar.x + scene->sidebar.w && y >= scene->sidebar.y && y < scene->player.y;
    scene_scroll(scene, &view, direction);
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
    View view = app_snapshot();
    scene_press(&controller->scene, &view, x, y, now);
    return controller->scene.dragging != HIT_NONE;
}

void ui_controller_release(UiController *controller, float x, float y, uint64_t now) {
    View view = app_snapshot();
    scene_pointer_motion(&controller->scene, x, y);
    scene_release(&controller->scene, &view, x, y);

    if (controller->scene.entrance == 0) {
        controller->scene.last_ms = now;
    }
}

void ui_controller_context_press(UiController *controller, float x, float y) {
    View view = app_snapshot();
    scene_context_press(&controller->scene, &view, x, y);
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
        if (!scene->login_screen) {
            scene->focus = HIT_SEARCH;
            scene->select_all = true;
        }

        return UI_SHORTCUT_HANDLED;
    }

    if (scene->setup_popup && key == 'v') {
        ui_paste_engine_key();
        return UI_SHORTCUT_HANDLED;
    }

    if (ui_controller_is_editing(controller)) {
        if (key == 'a') {
            scene->select_all = true;
            return UI_SHORTCUT_HANDLED;
        }

        if (key == 'u') {
            scene->query[0] = 0;
            scene_query_changed(scene);
            return UI_SHORTCUT_HANDLED;
        }

        if (key == 'v') {
            return UI_SHORTCUT_PASTE_SEARCH;
        }
    }

    return UI_SHORTCUT_NONE;
}

bool ui_controller_key(UiController *controller, SceneKey key, bool shift, uint64_t now) {
    View view = app_snapshot();
    bool handled = scene_key(&controller->scene, &view, key, shift);

    if (controller->scene.entrance == 0) {
        controller->scene.last_ms = now;
    }

    return handled;
}

bool ui_controller_text(UiController *controller, const char *utf8, size_t size) {
    if (!ui_controller_is_editing(controller)) {
        return false;
    }

    return scene_input(&controller->scene, utf8, size);
}
