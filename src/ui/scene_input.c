#include "scene_internal.h"
#include "lyrics_scene.h"
#include "collection_scene.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

static Rect scrollbar_track(const Scene *scene, HitId id) {
    if (id == HIT_SCROLL_LIBRARY) {
        return (Rect){scene->sidebar.w - 5, scene->library_rows.y, 4, scene->library_rows.h};
    }

    if (id == HIT_SCROLL_PANEL) {
        if (scene->panel == PANEL_LYRICS) {
            return lyrics_scene_scroll_track(scene);
        }

        Rect panel = scene->side_panel;
        return (Rect){panel.x + panel.w - 5, panel.y + QUEUE_LIST_TOP, 4,
                      panel.h - QUEUE_LIST_TOP - QUEUE_LIST_BOTTOM};
    }

    float top = scene->content.y + (!scene->home && scene->section == BROWSE_SEARCH ? 68 : 8);
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
        scene->library_first = (unsigned)(scroll->offset / LIBRARY_ROW_HEIGHT);
        app_library_viewport(scene->library_first);
        return;
    }

    if (scene->dragging == HIT_SCROLL_PANEL) {
        if (scene->panel == PANEL_LYRICS) {
            scene->lyrics_follow = false;
            return;
        }

        scene->panel_first = (unsigned)(scroll->offset / QUEUE_ROW_HEIGHT);
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

    if (target != HIT_SEARCH && target != HIT_SEARCH_CLEAR &&
        !scene_contains(scene->search_history_panel, x, y)) {
        scene->search_history_open = false;
        scene->search_history_scroll = (Scroll){0};
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
    scene->pressed_song_art = scene_contains(scene_song_art(scene, target), x, y);
    scene->double_click =
        scene->last_click == target && now >= scene->click_ms && now - scene->click_ms < 350;
    scene->last_click = target;
    scene->click_ms = now;
    scene->focus = target;

    if (target == HIT_SEARCH || target == HIT_LIST_FILTER) {
        if (scene->double_click) {
            scene_select_input_all(scene);
            scene->input_click_pending = false;
        } else {
            scene->select_all = false;
            scene->input_click_x = x;
            scene->input_click_pending = true;
        }
    } else {
        scene->select_all = false;
        scene->input_click_pending = false;
    }

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

        bool art = scene->pressed_song_art && scene_contains(scene_song_art(scene, scene->pressed), x, y);

        if ((scene->double_click || art) && scene->pressed >= HIT_ROW_FIRST && scene->pressed <= HIT_ROW_LAST) {
            const CatalogItem *item =
                list_row(&view->list, scene->first_row + (unsigned)(scene->pressed - HIT_ROW_FIRST));

            if (item && item_can_play(item) && scene->section == BROWSE_QUEUE && scene_queued(view, item->id)) {
                app_play_queued(item->id);
            } else if (item && item_can_play(item)) {
                app_open_item(item);
            }
        }

        if (art && !scene->double_click && scene->pressed >= HIT_POPULAR_FIRST &&
            scene->pressed <= HIT_POPULAR_LAST) {
            app_play_popular((unsigned)(scene->pressed - HIT_POPULAR_FIRST));
        }

        if (art && scene->pressed >= HIT_QUEUE_ROW_FIRST && scene->pressed <= HIT_QUEUE_ROW_LAST) {
            unsigned index = scene->panel_first + (unsigned)(scene->pressed - HIT_QUEUE_ROW_FIRST);

            const CatalogItem *item = queue_row(&view->queue, index);

            if (item && index < view->queue.queued) {
                app_play_queued(item->id);
            } else if (item) {
                app_play_upcoming(item);
            }
        }
    }

    scene->dragging = HIT_NONE;
    scene->pressed = HIT_NONE;
    scene->pressed_song_art = false;
}

void scene_query_changed(Scene *scene) {
    scene->search_history_open = !scene->query[0] && scene->focus == HIT_SEARCH;
    scene->search_due = scene->query[0] ? platform_milliseconds() + 180 : 0;
    scene->animating = scene->search_due != 0 || scene->animating;

    if (!scene->query[0] && scene->section == BROWSE_SEARCH) {
        scene->home = true;
        scene->pending_browse = false;
    }
}

static bool is_word_byte(unsigned char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           byte == '_' || byte >= 0x80;
}

static unsigned utf8_prev(const char *s, unsigned pos) {
    if (pos == 0) {
        return 0;
    }

    --pos;

    while (pos > 0 && ((unsigned char)s[pos] & 0xc0) == 0x80) {
        --pos;
    }

    return pos;
}

static unsigned utf8_next(const char *s, unsigned pos, unsigned len) {
    if (pos >= len) {
        return len;
    }

    ++pos;

    while (pos < len && ((unsigned char)s[pos] & 0xc0) == 0x80) {
        ++pos;
    }

    return pos;
}

static unsigned word_prev(const char *s, unsigned pos) {
    while (pos > 0 && (s[pos - 1] == ' ' || s[pos - 1] == '\t')) {
        --pos;
    }

    if (pos == 0) {
        return 0;
    }

    bool word = is_word_byte((unsigned char)s[pos - 1]);

    while (pos > 0 && s[pos - 1] != ' ' && s[pos - 1] != '\t' &&
           is_word_byte((unsigned char)s[pos - 1]) == word) {
        --pos;
    }

    return pos;
}

static unsigned word_next(const char *s, unsigned pos, unsigned len) {
    if (pos >= len) {
        return len;
    }

    if (s[pos] != ' ' && s[pos] != '\t') {
        bool word = is_word_byte((unsigned char)s[pos]);

        while (pos < len && s[pos] != ' ' && s[pos] != '\t' && is_word_byte((unsigned char)s[pos]) == word) {
            ++pos;
        }
    }

    while (pos < len && (s[pos] == ' ' || s[pos] == '\t')) {
        ++pos;
    }

    return pos;
}

static bool active_input(Scene *scene, char **buffer, InputState **state) {
    if (scene->focus == HIT_LIST_FILTER) {
        *buffer = scene->list_filter;
        *state = &scene->filter_input;
    } else if (scene->focus == HIT_SEARCH) {
        *buffer = scene->query;
        *state = &scene->search_input;
    } else {
        return false;
    }

    unsigned len = (unsigned)strlen(*buffer);

    if ((*state)->cursor > len) {
        (*state)->cursor = len;
    }

    if ((*state)->anchor > len) {
        (*state)->anchor = len;
    }

    if (scene->select_all && len > 0 && (*state)->cursor == (*state)->anchor) {
        (*state)->anchor = 0;
        (*state)->cursor = len;
    }

    return true;
}

static void sync_select_all(Scene *scene, const char *buffer, const InputState *state) {
    unsigned len = (unsigned)strlen(buffer);
    unsigned start = state->cursor < state->anchor ? state->cursor : state->anchor;
    unsigned end = state->cursor < state->anchor ? state->anchor : state->cursor;
    scene->select_all = len > 0 && start == 0 && end == len;
}

static void input_notify_changed(Scene *scene) {
    if (scene->focus == HIT_LIST_FILTER) {
        app_list_filter(scene->list_filter);
        return;
    }

    if (scene->focus == HIT_SEARCH) {
        scene_query_changed(scene);
    }
}

static bool edit_input_replace(Scene *scene, char *buffer, InputState *state, const char *text, size_t size) {
    if (size > 0) {
        char check[SEARCH_CAPACITY] = {0};

        if (!text || !search_append(check, text, size)) {
            return false;
        }
    }

    unsigned len = (unsigned)strlen(buffer);
    unsigned start = state->cursor < state->anchor ? state->cursor : state->anchor;
    unsigned end = state->cursor < state->anchor ? state->anchor : state->cursor;
    size_t after_len = (size_t)len - (end - start) + size;

    if (after_len >= SEARCH_CAPACITY) {
        return false;
    }

    char next[SEARCH_CAPACITY] = {0};

    if (start > 0) {
        memcpy(next, buffer, start);
    }

    if (size > 0) {
        memcpy(next + start, text, size);
    }

    memcpy(next + start + size, buffer + end, (size_t)(len - end) + 1);
    memcpy(buffer, next, after_len + 1);
    state->cursor = (unsigned)(start + size);
    state->anchor = state->cursor;
    scene->select_all = false;
    scene->input_click_pending = false;
    input_notify_changed(scene);
    return true;
}

void scene_select_input_all(Scene *scene) {
    char *buffer = NULL;
    InputState *state = NULL;

    if (!active_input(scene, &buffer, &state)) {
        scene->select_all = false;
        return;
    }

    state->anchor = 0;
    state->cursor = (unsigned)strlen(buffer);
    scene->select_all = true;
    scene->input_click_pending = false;
}

bool scene_copy(Scene *scene, char *out, size_t capacity, bool cut) {
    char *buffer = NULL;
    InputState *state = NULL;

    if (!out || !capacity || !active_input(scene, &buffer, &state)) {
        return false;
    }

    unsigned start = state->cursor < state->anchor ? state->cursor : state->anchor;
    unsigned end = state->cursor < state->anchor ? state->anchor : state->cursor;

    if (start == end) {
        out[0] = 0;
        return false;
    }

    size_t count = end - start;

    if (count >= capacity) {
        count = capacity - 1;
    }

    memcpy(out, buffer + start, count);
    out[count] = 0;

    if (cut) {
        edit_input_replace(scene, buffer, state, NULL, 0);
    }

    return true;
}

bool scene_input(Scene *scene, const char *text, size_t size) {
    char *buffer = NULL;
    InputState *state = NULL;

    if (!active_input(scene, &buffer, &state)) {
        return false;
    }

    return edit_input_replace(scene, buffer, state, text, size);
}

bool scene_key_mods(Scene *scene, const View *view, SceneKey key, bool shift, bool ctrl) {
    if (key == KEY_ESCAPE) {
        scene->select_all = false;
        scene->input_click_pending = false;

        if (scene->menu.open) {
            scene->menu.open = false;
            return true;
        }

        if (scene->search_history_open) {
            scene->search_history_open = false;
            scene->focus = HIT_NONE;
            return true;
        }

        if (scene->lyrics_fullscreen && !scene->setup_popup) {
            scene_activate(scene, view, HIT_LYRICS_FULLSCREEN);
            scene->focus = HIT_LYRICS_FULLSCREEN;
            return true;
        }

        if (scene->panel == PANEL_LYRICS && scene->setup_popup) {
            scene->setup_popup = false;
            return true;
        }

        scene->panel = PANEL_NONE;
        app_queue_visible(false);
        app_lyrics_visible(false);
        app_devices_visible(false);
        scene->setup_popup = false;
        scene->focus = HIT_NONE;
        return true;
    }

    if (key == KEY_TAB) {
        for (unsigned i = 1; i < HIT_COUNT; ++i) {
            int next = ((int)scene->focus + (shift ? -(int)i : (int)i) + HIT_COUNT) % HIT_COUNT;

            if (scene->hits[next].enabled && (next < HIT_QUEUE_ROW_FIRST || next > HIT_QUEUE_ROW_LAST)) {
                scene->focus = (HitId)next;
                scene->search_history_open =
                    next == HIT_SEARCH || next == HIT_SEARCH_CLEAR || next >= HIT_HISTORY_FIRST;

                if (next == HIT_SEARCH || next == HIT_LIST_FILTER) {
                    scene_select_input_all(scene);
                } else {
                    scene->select_all = false;
                }

                return true;
            }
        }
    }

    char *buffer = NULL;
    InputState *state = NULL;

    if (active_input(scene, &buffer, &state)) {
        unsigned len = (unsigned)strlen(buffer);
        unsigned start = state->cursor < state->anchor ? state->cursor : state->anchor;
        unsigned end = state->cursor < state->anchor ? state->anchor : state->cursor;

        if (key == KEY_ENTER) {
            if (scene->focus == HIT_LIST_FILTER) {
                app_list_filter(scene->list_filter);
                return true;
            }

            scene->search_history_open = false;
            scene_search(scene);
            return true;
        }

        if (key == KEY_BACKSPACE) {
            if (start < end) {
                edit_input_replace(scene, buffer, state, NULL, 0);
            } else if (state->cursor > 0) {
                state->anchor = ctrl ? word_prev(buffer, state->cursor) : utf8_prev(buffer, state->cursor);
                edit_input_replace(scene, buffer, state, NULL, 0);
            } else {
                scene->select_all = false;
            }

            return true;
        }

        if (key == KEY_DELETE) {
            if (start < end) {
                edit_input_replace(scene, buffer, state, NULL, 0);
            } else if (state->cursor < len) {
                state->anchor =
                    ctrl ? word_next(buffer, state->cursor, len) : utf8_next(buffer, state->cursor, len);
                edit_input_replace(scene, buffer, state, NULL, 0);
            } else {
                scene->select_all = false;
            }

            return true;
        }

        if (key == KEY_LEFT) {
            unsigned next = (!shift && !ctrl && start < end) ? start
                            : ctrl                           ? word_prev(buffer, state->cursor)
                                                             : utf8_prev(buffer, state->cursor);
            state->cursor = next;

            if (!shift) {
                state->anchor = next;
            }

            scene->input_click_pending = false;
            sync_select_all(scene, buffer, state);
            return true;
        }

        if (key == KEY_RIGHT) {
            unsigned next = (!shift && !ctrl && start < end) ? end
                            : ctrl                           ? word_next(buffer, state->cursor, len)
                                                             : utf8_next(buffer, state->cursor, len);
            state->cursor = next;

            if (!shift) {
                state->anchor = next;
            }

            scene->input_click_pending = false;
            sync_select_all(scene, buffer, state);
            return true;
        }

        if (key == KEY_HOME) {
            state->cursor = 0;

            if (!shift) {
                state->anchor = 0;
            }

            scene->input_click_pending = false;
            sync_select_all(scene, buffer, state);
            return true;
        }

        if (key == KEY_END) {
            state->cursor = len;

            if (!shift) {
                state->anchor = len;
            }

            scene->input_click_pending = false;
            sync_select_all(scene, buffer, state);
            return true;
        }

        if (scene->focus == HIT_SEARCH) {
            return true;
        }
    }

    if ((scene->focus == HIT_SEEK || scene->focus == HIT_VOLUME) && (key == KEY_LEFT || key == KEY_RIGHT)) {
        unsigned *value = scene->focus == HIT_SEEK ? &scene->draft.seek : &scene->draft.volume;
        unsigned maximum = scene->focus == HIT_SEEK ? 1000 : 100;
        int next = (int)*value + (key == KEY_LEFT ? -5 : 5);
        *value = next < 0 ? 0 : (unsigned)next > maximum ? maximum : (unsigned)next;
        return true;
    }

    if ((key == KEY_UP || key == KEY_DOWN) && scene->panel == PANEL_LYRICS &&
        (scene->lyrics_fullscreen || scene->focus == HIT_LYRICS || scene->focus == HIT_LYRICS_FULLSCREEN ||
         (scene->focus >= HIT_LYRICS_FOLLOW && scene->focus <= HIT_LYRIC_LAST))) {
        scene->over_panel = true;
        scene_wheel(scene, view, key == KEY_UP ? 1 : -1, 1, false);
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

            if (item && scene->section == BROWSE_SEARCH && view->list.request.kind == BROWSE_SEARCH) {
                app_search_history_add(item);
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

bool scene_key(Scene *scene, const View *view, SceneKey key, bool shift) {
    return scene_key_mods(scene, view, key, shift, false);
}

void scene_scroll(Scene *scene, const View *view, int direction) {
    scene_wheel(scene, view, (float)direction, 3, false);
}

static float wheel_distance(float notches, unsigned lines, bool page, float row, float viewport) {
    return notches * (page ? viewport : lines * row);
}

void scene_wheel(Scene *scene, const View *view, float notches, unsigned lines, bool page) {
    if (scene->setup_popup || !notches || (!page && !lines)) {
        return;
    }

    uint64_t now = scene->last_ms ? scene->last_ms : platform_milliseconds();
    bool smooth = scene->last_ms != 0;

    if (scene->over_search_history && scene->search_history_open) {
        Scroll *scroll = &scene->search_history_scroll;
        float target = scroll->active ? scroll->target : scroll->offset;
        scroll_to(scroll, target + wheel_distance(notches, lines, page, HISTORY_ROW_HEIGHT, scroll->viewport),
                  now, smooth);
        scene->animating = scene->animating || scroll->active;
        return;
    }

    if (scene->hot == HIT_VOLUME && scene->hits[HIT_VOLUME].enabled) {
        int change = (int)(-notches * 5 + (notches < 0 ? .5f : -.5f));
        int next = (int)scene->draft.volume + change;
        scene->draft.volume = next < 0 ? 0 : next > 100 ? 100 : (unsigned)next;

        if (change) {
            app_volume(scene->draft.volume);
        }

        return;
    }

    if (scene->over_panel && scene->panel == PANEL_DEVICES) {
        int count = (int)view->devices.count + (view->engine != ENGINE_READY);
        int max = count > (int)scene->device_visible ? count - (int)scene->device_visible : 0;
        float distance = wheel_distance(notches, lines, page, 1, (float)scene->device_visible);
        int next = (int)scene->device_first + (int)(distance + (distance < 0 ? -.5f : .5f));
        scene->device_first = next < 0 ? 0 : next > max ? (unsigned)max : (unsigned)next;
        return;
    }

    if (scene->over_panel && scene->panel != PANEL_NONE) {
        if (scene->panel == PANEL_LYRICS && scene->panel_scroll.extent > scene->panel_scroll.viewport) {
            Scroll *scroll = &scene->panel_scroll;
            scene->lyrics_follow = false;
            float target = scroll->active ? scroll->target : scroll->offset;
            scroll_to(scroll, target + wheel_distance(notches, lines, page, 36, scroll->viewport), now, smooth);
            scene->animating = scene->animating || scroll->active;
        } else if (scene->panel == PANEL_QUEUE && !scene->panel_recent) {
            float target =
                (scene->panel_scroll.active ? scene->panel_scroll.target : scene->panel_scroll.offset) +
                wheel_distance(notches, lines, page, QUEUE_ROW_HEIGHT, scene->panel_scroll.viewport);
            scroll_to(&scene->panel_scroll, target, now, smooth);
            scene->panel_first = (unsigned)(scene->panel_scroll.offset / QUEUE_ROW_HEIGHT);

            if (scene->panel_scroll.active) {
                scene->last_ms = now;
                scene->animating = true;
            }
        }

        return;
    }

    if (scene->over_library) {
        float target =
            (scene->library_scroll.active ? scene->library_scroll.target : scene->library_scroll.offset) +
            wheel_distance(notches, lines, page, LIBRARY_ROW_HEIGHT, scene->library_scroll.viewport);
        scroll_to(&scene->library_scroll, target, now, smooth);
        scene->library_first = (unsigned)(scene->library_scroll.offset / LIBRARY_ROW_HEIGHT);
        app_library_viewport(scene->library_first);

        if (scene->library_scroll.active) {
            scene->last_ms = now;
            scene->animating = true;
        }

        return;
    }

    if (scene->home) {
        float target = (scene->main_scroll.active ? scene->main_scroll.target : scene->main_scroll.offset) +
                       wheel_distance(notches, lines, page, 32, scene->main_scroll.viewport);
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
                       wheel_distance(notches, lines, page, 32, scene->main_scroll.viewport);
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

    float row = scene->main_row_height > 0 ? scene->main_row_height : 56.0f;
    float delta = wheel_distance(notches, lines, page, scene->main_columns > 1 ? row / 3 : row,
                                 scene->main_scroll.viewport);
    float target = (scene->main_scroll.active ? scene->main_scroll.target : scene->main_scroll.offset) +
                   delta;
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
