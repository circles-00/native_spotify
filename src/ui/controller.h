#ifndef NATIVE_UI_CONTROLLER_H
#define NATIVE_UI_CONTROLLER_H

#include "scene.h"

typedef enum { UI_CURSOR_ARROW, UI_CURSOR_HAND, UI_CURSOR_IBEAM, UI_CURSOR_RESIZE } UiCursor;

typedef enum {
    UI_SHORTCUT_NONE,
    UI_SHORTCUT_HANDLED,
    UI_SHORTCUT_PASTE_SEARCH,
    UI_SHORTCUT_COPY_SEARCH,
    UI_SHORTCUT_CUT_SEARCH
} UiShortcutResult;

typedef struct {
    Scene scene;
    View *snapshot;
    unsigned list_revision;
    bool last_home;
} UiController;

void ui_controller_init(UiController *controller, uint64_t now);
void ui_controller_destroy(UiController *controller);
bool ui_controller_is_editing(const UiController *controller);
UiCursor ui_controller_cursor(const UiController *controller);
void ui_controller_layout(UiController *controller, float width, float height);
void ui_controller_draw(UiController *controller, float width, float height, Canvas *canvas);
bool ui_controller_tick(UiController *controller, uint64_t now);
void ui_controller_scroll(UiController *controller, float x, float y, int direction, uint64_t now);
void ui_controller_wheel(UiController *controller, float x, float y, float notches, unsigned lines, bool page,
                         uint64_t now);
bool ui_controller_motion(UiController *controller, float x, float y, uint64_t now, bool *cursor_changed);
void ui_controller_leave(UiController *controller, uint64_t now);
bool ui_controller_press(UiController *controller, float x, float y, uint64_t now);
void ui_controller_release(UiController *controller, float x, float y, uint64_t now);
void ui_controller_context_press(UiController *controller, float x, float y);
UiShortcutResult ui_controller_shortcut(UiController *controller, char key, bool ctrl);
bool ui_controller_copy(UiController *controller, char *out, size_t capacity, bool cut);
bool ui_controller_key(UiController *controller, SceneKey key, bool shift, uint64_t now);
bool ui_controller_key_mods(UiController *controller, SceneKey key, bool shift, bool ctrl, uint64_t now);
bool ui_controller_text(UiController *controller, const char *utf8, size_t size);

#endif
