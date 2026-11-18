#include "scene_internal.h"
#include "dark_palette.h"
#include "skeleton.h"
#include "collection_scene.h"
#include "home_scene.h"
#include "panel_scene.h"
#include "platform.h"
#include "search_scene.h"
#include "settings_scene.h"

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

float scene_view_scale(float width, float height, float minimum) {
    float scale = width / 1280.0f;
    float vertical = height / 800.0f;

    if (scale > vertical) {
        scale = vertical;
    }

    if (scale > 1.5f) {
        scale = 1.5f;
    }

    return scale > minimum ? scale : minimum;
}

unsigned scene_columns(float width) {
    return width >= 850 ? 5 : width >= 660 ? 4 : 3;
}

bool scene_contains(Rect rect, float x, float y) {
    return rect.w > 0 && rect.h > 0 && x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
           y < rect.y + rect.h;
}

float scene_player_text_limit(const Scene *scene, bool has_like) {
    float max_width = scene->width < 1000 ? 142.0f : 215.0f;

    if (!has_like) {
        return max_width;
    }

    float seek_w = scene->width >= 1200 ? 400.0f : scene->width >= 1000 ? 352.0f : 260.0f;
    float seek_x = (scene->width - seek_w) * 0.5f;
    float available = seek_x - 70.0f - 77.0f;

    if (available < 0.0f) {
        available = 0.0f;
    }

    return max_width < available ? max_width : available;
}

Rect scene_player_like_rect(const Scene *scene, float text_width) {
    float limit = scene_player_text_limit(scene, true);
    float used = text_width < limit ? text_width : limit;

    if (used < 0.0f) {
        used = 0.0f;
    }

    return (Rect){77.0f + used + 4.0f, scene->player.y + 24.0f, 28.0f, 28.0f};
}

void scene_player_update_text_hits(Scene *scene, const View *view, float title_w, float artist_w) {
    bool has_like =
        !strncmp(view->playback.uri, "spotify:track:", 14) && spotify_id_valid(view->playback.uri + 14);
    float limit = scene_player_text_limit(scene, has_like);
    float song_w = title_w < limit ? title_w : limit;
    float art_w = artist_w < limit ? artist_w : limit;

    if (song_w < 0.0f) {
        song_w = 0.0f;
    }

    if (art_w < 0.0f) {
        art_w = 0.0f;
    }

    bool active_track = view->playback.title[0] != 0;
    bool has_song = active_track && (spotify_id_valid(view->playback.album_id) || view->playback.album[0]);
    bool has_artist =
        active_track && (spotify_id_valid(view->playback.artist_id) || view->playback.artist[0]);

    hit(scene, HIT_PLAYER_SONG, (Rect){77.0f, scene->player.y + 20.0f, song_w, 19.0f}, has_song);
    hit(scene, HIT_PLAYER_ARTIST, (Rect){77.0f, scene->player.y + 39.0f, art_w, 18.0f}, has_artist);

    if (has_like) {
        float measured = title_w > artist_w ? title_w : artist_w;
        hit(scene, HIT_PLAYER_LIKE, scene_player_like_rect(scene, measured), true);
    } else {
        hit(scene, HIT_PLAYER_LIKE, (Rect){0}, false);
    }
}

static unsigned utf8_codepoints(const char *text) {
    unsigned count = 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xc0u) != 0x80u) {
            ++count;
        }
    }

    return count;
}

/* Whether a song sits in the listener's own queue (the leading rows of the composed queue). */
bool scene_queued(const View *view, const char *id) {
    for (unsigned i = 0; i < view->queue.count && view->queue.first + i < view->queue.queued; ++i) {
        if (!strcmp(view->queue.items[i].id, id)) {
            return true;
        }
    }

    return false;
}

Rect scene_song_art(const Scene *scene, HitId id) {
    Rect row = scene->hits[id].rect;

    if (id == HIT_SEARCH_TOP) {
        return (Rect){row.x + 20, row.y + 20, 88, 88};
    }

    if (id >= HIT_POPULAR_FIRST && id <= HIT_POPULAR_LAST) {
        return (Rect){row.x + 52, row.y + 6, 36, 36};
    }

    if (id >= HIT_QUEUE_ROW_FIRST && id <= HIT_QUEUE_ROW_LAST) {
        unsigned slot = (unsigned)(id - HIT_QUEUE_ROW_FIRST);
        float sub = scene->panel_scroll.offset - scene->panel_first * QUEUE_ROW_HEIGHT;
        return (Rect){scene->side_panel.x + 19,
                      scene->side_panel.y + QUEUE_LIST_TOP + slot * QUEUE_ROW_HEIGHT + 5 - sub, 32, 32};
    }

    if (id >= HIT_HISTORY_FIRST && id <= HIT_HISTORY_LAST) {
        return (Rect){row.x + 8, row.y + 6, 38, 38};
    }

    if (id >= HIT_ROW_FIRST && id <= HIT_ROW_LAST) {
        if (scene->section == BROWSE_SEARCH) {
            return (Rect){row.x + 8, row.y + 7, 44, 44};
        }

        if (scene->section == BROWSE_ALBUM_TRACKS) {
            return (Rect){row.x + 52, row.y + 6, 36, 36};
        }

        return (Rect){row.x + 48, row.y + 8, 40, 40};
    }

    return (Rect){0};
}

/* "Playing on <device>" badge before the profile picture, shown only for another device. */
static Rect playing_on_rect(const View *view, float right, float left, float center_y) {
    bool other = view->authenticated && view->playback.has_device && view->playback.device[0] &&
                 strcmp(view->playback.device_id, view->engine_device_id) != 0;

    if (!other) {
        return (Rect){0};
    }

    float text_w = (float)utf8_codepoints(view->playback.device) * 7.0f + 64.0f;
    float width = text_w < 96.0f ? 96.0f : text_w > 230.0f ? 230.0f : text_w;

    // Keep room for the home button and a usable search box.
    if (right - left - width - 12.0f < 46.0f + 220.0f) {
        return (Rect){0};
    }

    return (Rect){right - width, center_y - 15.0f, width, 30.0f};
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

    if (scene->search_history_open && !scene->setup_popup &&
        scene_contains(scene->search_history_panel, x, y)) {
        for (int i = HIT_HISTORY_REMOVE_LAST; i >= HIT_HISTORY_FIRST; --i) {
            if (y >= scene->search_history_panel.y + 34 &&
                y < scene->search_history_panel.y + 34 + scene->search_history_scroll.viewport &&
                scene->hits[i].enabled && scene_contains(scene->hits[i].rect, x, y)) {
                return (HitId)i;
            }
        }

        return HIT_NONE;
    }

    for (int i = HIT_HISTORY_FIRST - 1; i > 0; --i) {
        bool home_result = (i >= HIT_HOME_QUICK_FIRST && i <= HIT_HOME_PLAYLISTS) ||
                           (i >= HIT_CARD_FIRST && i <= HIT_CARD_LAST);

        if (scene->home && home_result && !scene_contains(scene->content, x, y)) {
            continue;
        }

        if (i >= HIT_LIBRARY_FIRST && i <= HIT_LIBRARY_LAST && !scene_contains(scene->library_rows, x, y)) {
            continue;
        }

        if (scene->panel != PANEL_NONE && !scene->setup_popup && scene_contains(scene->side_panel, x, y) &&
            i != HIT_PANEL_CLOSE && i != HIT_PANEL_QUEUE && i != HIT_PANEL_PREVIOUS && i != HIT_PANEL_PLAY &&
            i != HIT_PANEL_NEXT && i != HIT_SCROLL_PANEL && i != HIT_TRANSFER && i != HIT_QUEUE_CLEAR &&
            (i < HIT_DEVICE_FIRST || i > HIT_DEVICE_LAST) && (i < HIT_QUEUE_ROW_FIRST || i > HIT_QUEUE_LIKE_LAST) &&
            (i < HIT_LYRICS_FULLSCREEN || i > HIT_LYRIC_LAST) &&
            (i < HIT_PANEL_RECENT_TAB || i > HIT_PANEL_RECENT_LAST)) {
            continue;
        }

        bool search_result = (i >= HIT_SEARCH_TOP && i <= HIT_SEARCH_PLAYLIST_LAST) ||
                             (i >= HIT_ROW_FIRST && i <= HIT_ROW_LAST) ||
                             (i >= HIT_ROW_LIKE_FIRST && i <= HIT_ROW_LIKE_LAST) || i == HIT_TOP_LIKE;

        if (!scene->home && scene->section == BROWSE_SEARCH && search_result &&
            (y < scene->content.y + 64 || y >= scene->content.y + scene->content.h || x < scene->content.x ||
             x >= scene->content.x + scene->content.w)) {
            continue;
        }

        bool content_control = (i >= HIT_ROW_FIRST && i <= HIT_ROW_LAST) ||
                               (i >= HIT_ROW_LIKE_FIRST && i <= HIT_POPULAR_LIKE_LAST) ||
                               i == HIT_ARTIST_LIKES || i == HIT_SETTING_SEARCH_RADIO;

        if (!scene->home && scene->section != BROWSE_SEARCH && content_control &&
            (y < scene->content.y || y >= scene->content.y + scene->content.h || x < scene->content.x ||
             x >= scene->content.x + scene->content.w)) {
            continue;
        }

        if (scene->setup_popup && !setup_hit(i)) {
            continue;
        }

        if (scene->hits[i].enabled && scene_contains(scene->hits[i].rect, x, y)) {
            return (HitId)i;
        }
    }

    return HIT_NONE;
}

const CatalogItem *scene_like_item(const Scene *scene, const View *view, HitId id) {
    if (id >= HIT_ROW_LIKE_FIRST && id <= HIT_ROW_LIKE_LAST) {
        return list_row(&view->list, scene->first_row + (unsigned)(id - HIT_ROW_LIKE_FIRST));
    }

    if (id >= HIT_POPULAR_LIKE_FIRST && id <= HIT_POPULAR_LIKE_LAST) {
        unsigned index = (unsigned)(id - HIT_POPULAR_LIKE_FIRST);
        return index < view->details.popular_count ? &view->details.popular[index] : NULL;
    }

    if (id >= HIT_QUEUE_LIKE_FIRST && id <= HIT_QUEUE_LIKE_LAST) {
        unsigned index = scene->panel_first + (unsigned)(id - HIT_QUEUE_LIKE_FIRST);
        return queue_row(&view->queue, index);
    }

    if (id >= HIT_HISTORY_LIKE_FIRST && id <= HIT_HISTORY_LIKE_LAST) {
        unsigned index = (unsigned)(id - HIT_HISTORY_LIKE_FIRST);
        return index < view->search_history_count ? &view->search_history[index] : NULL;
    }

    return id == HIT_TOP_LIKE ? search_scene_item(scene, view, HIT_SEARCH_TOP) : NULL;
}

static void layout_song_hearts(Scene *scene, const View *view) {
    for (int id = HIT_TOP_LIKE; id <= HIT_POPULAR_LIKE_LAST; ++id) {
        HitId parent = id == HIT_TOP_LIKE ? HIT_SEARCH_TOP
                       : id <= HIT_ROW_LIKE_LAST ? HIT_ROW_FIRST + id - HIT_ROW_LIKE_FIRST
                                                : HIT_POPULAR_FIRST + id - HIT_POPULAR_LIKE_FIRST;
        Rect row = scene->hits[parent].rect;

        if (row.w <= 0 || row.h <= 0) {
            continue;
        }

        const CatalogItem *item = scene_like_item(scene, view, (HitId)id);

        if (!item || item->kind != ITEM_TRACK || !spotify_id_valid(item->id)) {
            continue;
        }

        float right = id == HIT_TOP_LIKE ? 70 : 96;
        float y = id == HIT_TOP_LIKE ? row.y + row.h - 52 : row.y + (row.h - 26) / 2;
        hit(scene, (HitId)id, (Rect){row.x + row.w - right, y, 26, 26},
            y >= scene->content.y && y + 26 <= scene->content.y + scene->content.h);
    }
}

static void layout_search_history(Scene *scene, const View *view) {
    scene->search_history_panel = (Rect){0};

    if (!scene->search_history_open || scene->setup_popup) {
        return;
    }

    float y = scene->search.y + scene->search.h + 6;
    float extent = view->search_history_count ? view->search_history_count * HISTORY_ROW_HEIGHT : 36;
    float available = scene->player.y - y - 48;
    float viewport = extent < available ? extent : available;

    if (viewport < HISTORY_ROW_HEIGHT && view->search_history_count) {
        return;
    }

    scroll_layout(&scene->search_history_scroll, extent, viewport);
    Rect panel = {scene->search.x, y, scene->search.w, viewport + 42};
    scene->search_history_panel = panel;

    for (unsigned i = 0; i < view->search_history_count; ++i) {
        Rect row = {panel.x + 6, panel.y + 34 + i * HISTORY_ROW_HEIGHT - scene->search_history_scroll.offset,
                    panel.w - 17, HISTORY_ROW_HEIGHT};
        float top = panel.y + 34;
        float bottom = top + viewport;
        float visible_y = row.y < top ? top : row.y;
        float visible_end = row.y + row.h > bottom ? bottom : row.y + row.h;

        if (visible_end <= visible_y) {
            continue;
        }

        hit(scene, (HitId)(HIT_HISTORY_FIRST + i), row, true);

        if (row.y + 12 >= top && row.y + 38 <= bottom) {
            hit(scene, (HitId)(HIT_HISTORY_LIKE_FIRST + i), (Rect){row.x + row.w - 60, row.y + 12, 26, 26},
                true);
            hit(scene, (HitId)(HIT_HISTORY_REMOVE_FIRST + i), (Rect){row.x + row.w - 29, row.y + 12, 26, 26},
                true);
        }
    }
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
        unsigned display_count = skeleton_count(&view->list, columns);
        unsigned rows = (display_count + columns - 1) / columns;
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

        for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < display_count; ++slot) {
            unsigned index = scene->first_row + slot;
            Rect rect = {x + (index % columns) * (card_width + 8), grid_y + (index / columns) * card_height,
                         card_width, card_height - 8};

            if (rect.y >= scene->content.y + scene->content.h) {
                break;
            }

            bool visible = rect.y + rect.h > scene->content.y &&
                           rect.y + rect.h <= scene->content.y + scene->content.h - 8;
            hit(scene, (HitId)(HIT_ROW_FIRST + slot), rect, visible && index < count);
            shown = slot + 1;
        }
    }

    if (!grid) {
        unsigned display_count = skeleton_count(&view->list, 5);
        scroll_layout(&scene->main_scroll, 280 + display_count * 56.0f + 24, scene->content.h);
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

        for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < display_count; ++slot) {
            unsigned index = scene->first_row + slot;
            Rect rect = {x - 8, list_y + index * 56, scene->content.w - 32, 56};

            if (rect.y >= scene->content.y + scene->content.h) {
                break;
            }

            bool row_visible = rect.y + rect.h > scene->content.y &&
                               rect.y + rect.h <= scene->content.y + scene->content.h - 8;
            hit(scene, (HitId)(HIT_ROW_FIRST + slot), rect, row_visible && index < count);
            shown = slot + 1;
        }

        bool selected = view->list.selected >= 0 && (unsigned)view->list.selected < count;
        bool arrangeable = app_list_arrangeable(scene->section) && view->list.loaded;
        float controls_y = y + 192 - scene->main_scroll.offset;
        float bottom = scene->content.y + scene->content.h;

        hit(scene, HIT_OPEN, (Rect){x, controls_y, 44, 44},
            (arrangeable ? count > 0 : selected) && controls_y + 44 > scene->content.y && controls_y < bottom);
        hit(scene, HIT_LINK, (Rect){x + 56, controls_y + 6, 32, 32},
            selected && controls_y + 38 > scene->content.y && controls_y + 6 < bottom);

        // Search box and sortable column headers for track lists. The app owns the filter unless it is being typed.
        if (scene->focus != HIT_LIST_FILTER) {
            copy_utf8(scene->list_filter, sizeof(scene->list_filter), view->list.filter);
        }

        float right = scene->content.x + scene->content.w - 24;
        Rect filter = {right - 240, controls_y + 6, 240, 32};
        bool filter_visible = arrangeable && filter.y >= scene->content.y && filter.y + filter.h <= bottom;
        hit(scene, HIT_LIST_FILTER, filter, filter_visible);
        hit(scene, HIT_LIST_FILTER_CLEAR, (Rect){filter.x + filter.w - 30, filter.y + 4, 24, 24},
            filter_visible && scene->list_filter[0]);

        float header_y = y + 250 - scene->main_scroll.offset;
        bool header_visible = arrangeable && header_y >= scene->content.y && header_y + 26 <= bottom;
        hit(scene, HIT_SORT_INDEX, (Rect){x, header_y, 30, 26}, header_visible);
        hit(scene, HIT_SORT_TITLE, (Rect){x + 34, header_y, 120, 26}, header_visible);
        hit(scene, HIT_SORT_ALBUM, (Rect){x + scene->content.w * .53f - 6, header_y, 120, 26}, header_visible);
        hit(scene, HIT_SORT_DURATION, (Rect){scene->content.x + scene->content.w - 92, header_y, 60, 26},
            header_visible);
    }

    // Infinite scroll: fetch the next page well before the listener reaches the end.
    if (view->list.more && !view->list.loading && scene->first_row + shown + 2 * ROW_SLOTS >= count) {
        app_load_more();
    }

    app_viewport(scene->first_row);
}

static void layout_popups(Scene *scene, const View *view, float width, float height) {
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
        scene->lyrics_fullscreen = false;
        scene->panel_recent = false;
        scene->search_due = 0;
        scene->search_history_open = false;
        scene->library_first = 0;
        scene->home = true;
        scene->history_count = 0;
        scene->history_index = 0;
        scene->setup_popup = false;
        scene->menu.open = false;
        scene->pressed = HIT_NONE;
        scene->dragging = HIT_NONE;
        scene->collection_artwork[0] = 0;
        scene->pending_browse = false;
        scene->retain_content = false;
        scene->entrance = 1;
        scene->player_text_w = 0;
        scene->player_title_w = 0;
        scene->player_artist_w = 0;
        scene->player_text_measured = false;
        scene->player_measured_title[0] = 0;
        scene->player_measured_artist[0] = 0;
    }

    if (scene->width != width || scene->height != height) {
        scene->retain_content = false;
        scene->entrance = 1;
    }

    scene->width = width;
    scene->height = height;
    float rail = width < 1080 ? 190 : 223;
    float panel = width < 1080 ? 266 : 320;
    scene->sidebar = (Rect){0, 0, rail, height - 78};
    scene->library_rows = (Rect){0, 246, rail, height - 331};
    scene->header = (Rect){0, 0, width, 54};
    scene->content = (Rect){rail, scene->header.h, width - rail, height - 78 - scene->header.h};
    scene->side_panel = (Rect){width - panel, scene->header.h, panel, scene->content.h};

    if (scene->lyrics_fullscreen) {
        scene->side_panel = (Rect){0, 0, width, height - 78};
    }

    if (scene->panel != PANEL_NONE && width >= 1080) {
        scene->content.w -= panel;
    }

    scene->player = (Rect){0, height - 78, width, 78};

    float header_h = 38.0f;
    float center_y = scene->header.h * 0.5f;
    float header_y = center_y - header_h * 0.5f;
    float home_gap = 8.0f;
    float min_left = rail + 97.0f;
    Rect avatar = {width - scene->caption_width - 40.0f, center_y - 16.0f, 32, 32};
    float max_right = avatar.x - 12.0f;
    Rect playing_on = playing_on_rect(view, max_right, min_left, center_y);

    if (playing_on.w > 0) {
        max_right = playing_on.x - 12.0f;
    }

    float available = max_right - min_left;
    float search_w = 380.0f;

    if (header_h + home_gap + search_w > available) {
        search_w = available - header_h - home_gap;
    }

    if (search_w < 140.0f) {
        search_w = 140.0f;
    }

    float search_x = (float)(int)(rail + (width - rail - scene->caption_width - search_w) * 0.5f + 0.5f);

    if (search_x - home_gap - header_h < min_left) {
        search_x = min_left + header_h + home_gap;
    }

    if (search_x + search_w > max_right) {
        search_x = max_right - search_w;
    }

    Rect top_home = {search_x - home_gap - header_h, header_y, header_h, header_h};
    scene->search = (Rect){search_x, header_y, search_w, header_h};

    bool was_login = scene->login_screen;
    scene->intro_screen = view->loading_intro;
    scene->login_screen = !scene->intro_screen && !view->authenticated;

    if (was_login && !scene->login_screen && !scene->intro_screen) {
        scene->focus = HIT_SEARCH;
    }

    // The app asks for the setup panel when playback needs Soloist and Soloist needs the listener.
    if (scene->setup_revision != view->setup_revision) {
        scene->setup_revision = view->setup_revision;
        scene->setup_popup = true;
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

    if (scene->intro_screen) {
        scene->last_ms = platform_milliseconds();
        scene->animating = true;
        return;
    }

    if (scene->login_screen) {
        scene->login_panel = (Rect){width / 2 - 240, height / 2 - 224, 480, 448};
        scene->focus = HIT_ACCOUNT;
        hit(scene, HIT_ACCOUNT, (Rect){width / 2 - 160, scene->login_panel.y + 266, 320, 48},
            !view->connecting);
        return;
    }

    hit(scene, HIT_HOME, (Rect){11, 12, rail - 22, 39}, true);
    hit(scene, HIT_TOP_HOME, top_home, true);
    hit(scene, HIT_BACK, (Rect){rail + 21, 11, 28, 28}, scene->history_index > 0);
    hit(scene, HIT_FORWARD, (Rect){rail + 57, 11, 28, 28}, scene->history_index + 1 < scene->history_count);
    hit(scene, HIT_SEARCH_NAV, (Rect){11, 53, rail - 22, 39}, true);
    hit(scene, HIT_SEARCH, scene->search, true);
    hit(scene, HIT_SEARCH_CLEAR,
        (Rect){scene->search.x + scene->search.w - 30, scene->search.y + (scene->search.h - 24) / 2, 24, 24},
        scene->query[0] != 0);
    hit(scene, HIT_PROFILE, avatar, true);
    hit(scene, HIT_PLAYING_ON, playing_on, playing_on.w > 0);

    hit(scene, HIT_PLAYLISTS, (Rect){11, 149, 66, 27}, true);
    hit(scene, HIT_ALBUMS, (Rect){82, 149, 63, 27}, true);
    hit(scene, HIT_ARTISTS, (Rect){150, 149, rail < 200 ? 38 : 58, 27}, true);
    hit(scene, HIT_TRACKS, (Rect){11, 192, rail - 22, LIBRARY_ROW_HEIGHT}, true);
    hit(scene, HIT_QUEUE, (Rect){width - 195, height - 54, 26, 28}, true);
    hit(scene, HIT_DEVICES, (Rect){width - 163, height - 54, 26, 28}, true);
    hit(scene, HIT_LYRICS, (Rect){width - 227, height - 54, 26, 28}, true);

    scroll_layout(&scene->library_scroll, view->playlists.count * LIBRARY_ROW_HEIGHT, scene->library_rows.h);
    scene->library_first = (unsigned)(scene->library_scroll.offset / LIBRARY_ROW_HEIGHT);

    if (scene->library_first > view->playlists.count) {
        scene->library_first = view->playlists.count;
    }

    float library_sub = scene->library_scroll.offset - scene->library_first * LIBRARY_ROW_HEIGHT;

    for (unsigned i = 0; i < 24 && scene->library_first + i < view->playlists.count; ++i) {
        Rect row = {11, scene->library_rows.y + i * LIBRARY_ROW_HEIGHT - library_sub, rail - 22,
                    LIBRARY_ROW_HEIGHT};

        if (row.y + row.h > scene->library_rows.y && row.y < scene->library_rows.y + scene->library_rows.h) {
            hit(scene, (HitId)(HIT_LIBRARY_FIRST + i), row, true);
        }
    }

    Rect library_thumb = scroll_thumb(&scene->library_scroll,
                                      (Rect){rail - 5, scene->library_rows.y, 4, scene->library_rows.h});
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

    if (!scene->home && scene->section == BROWSE_SETTINGS) {
        scene->main_origin = 0;
        scene->main_row_height = 0;
        scene->main_columns = 0;
        settings_scene_layout(scene, view);
    }

    if (!scene->home && scene->section != BROWSE_SEARCH && scene->section != BROWSE_SETTINGS &&
        !collection_page(scene->section)) {
        layout_main_list(scene, view);
    }

    float main_top = scene->content.y + (!scene->home && scene->section == BROWSE_SEARCH ? 68 : 8);
    float main_track_h = scene->content.y + scene->content.h - 8 - main_top;
    Rect main_thumb = scroll_thumb(
        &scene->main_scroll, (Rect){scene->content.x + scene->content.w - 10, main_top, 6, main_track_h});
    hit(scene, HIT_SCROLL_MAIN, main_thumb, main_thumb.h > 0);

    float seek_w = width >= 1200 ? 400 : width >= 1000 ? 352 : 260;
    hit(scene, HIT_REFRESH, (Rect){rail - 35, 115, 27, 24}, true);
    hit(scene, HIT_PREVIOUS, (Rect){width / 2 - 52, height - 60, 28, 28},
        playback_can_command(&view->playback, "previous"));
    hit(scene, HIT_PLAY, (Rect){width / 2 - 16, height - 62, 32, 32}, true);
    hit(scene, HIT_SHUFFLE, (Rect){width / 2 - 87, height - 60, 28, 28},
        view->playback.has_device && !view->playback.restricted);
    hit(scene, HIT_REPEAT, (Rect){width / 2 + 60, height - 60, 28, 28},
        view->playback.has_device && !view->playback.restricted);
    hit(scene, HIT_NEXT, (Rect){width / 2 + 22, height - 60, 28, 28},
        playback_can_command(&view->playback, "next"));
    hit(scene, HIT_SEEK, (Rect){(width - seek_w) / 2, height - 26, seek_w, 14},
        playback_can_seek(&view->playback));
    hit(scene, HIT_MUTE, (Rect){width - 130, height - 54, 26, 28}, playback_can_volume(&view->playback));
    hit(scene, HIT_VOLUME, (Rect){width - 96, height - 48, 82, 16}, playback_can_volume(&view->playback));

    panel_scene_layout(scene, view);
    layout_song_hearts(scene, view);
    layout_search_history(scene, view);

    const char *title = view->playback.title[0] ? view->playback.title : "Nothing playing yet";
    const char *artist = view->playback.title[0] ? view->playback.artist : "Choose a track from your library";
    float title_w = scene->player_title_w;
    float artist_w = scene->player_artist_w;

    if (!scene->player_text_measured || strcmp(scene->player_measured_title, title) ||
        strcmp(scene->player_measured_artist, artist)) {
        title_w = (float)utf8_codepoints(title) * 7.0f;
        artist_w = (float)utf8_codepoints(artist) * 6.0f;
    }

    scene_player_update_text_hits(scene, view, title_w, artist_w);

    if (scene->lyrics_fullscreen) {
        for (int i = HIT_NONE + 1; i < HIT_COUNT; ++i) {
            if (i != HIT_PANEL_CLOSE && i != HIT_SCROLL_PANEL &&
                (i < HIT_LYRICS_FULLSCREEN || i > HIT_LYRIC_LAST) &&
                scene->hits[i].rect.y < scene->player.y) {
                scene->hits[i].enabled = false;
            }
        }
    }

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
    bool history_moving = scene->search_history_open && scroll_tick(&scene->search_history_scroll, now);
    bool active = scene->intro_screen || scene->search_due != 0 || main_moving || library_moving ||
                  panel_moving || history_moving;

    if (library_moving) {
        scene->library_first = (unsigned)(scene->library_scroll.offset / LIBRARY_ROW_HEIGHT);
        app_library_viewport(scene->library_first);
    }

    if (panel_moving) {
        scene->panel_first = (unsigned)(scene->panel_scroll.offset / QUEUE_ROW_HEIGHT);
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
    unsigned shelf = index / HOME_SHELF_COLUMNS;
    unsigned slot = index % HOME_SHELF_COLUMNS;

    if (shelf == 1) {
        return slot < view->recent_count ? &view->recent[slot] : NULL;
    }

    if (shelf == 0) {
        const BrowsePage *playlists = &view->library[2];
        unsigned featured = 0;

        for (unsigned i = 0; i < playlists->count; ++i) {
            const CatalogItem *item = &playlists->items[i];

            if (!strcmp(item->owner_id, "spotify") &&
                (!strcmp(item->title, "Discover Weekly") || !strcmp(item->title, "Release Radar"))) {
                if (featured++ == slot) {
                    return item;
                }
            }
        }

        return !featured && slot < playlists->count ? &playlists->items[slot] : NULL;
    }

    if (shelf == 2) {
        return slot < view->library[1].count ? &view->library[1].items[slot] : NULL;
    }

    return NULL;
}

bool scene_home_made_for_you(const View *view) {
    const CatalogItem *item = scene_home_item(view, 0);
    return item && !strcmp(item->owner_id, "spotify") &&
           (!strcmp(item->title, "Discover Weekly") || !strcmp(item->title, "Release Radar"));
}

const CatalogItem *scene_home_quick_item(const View *view, unsigned index) {
    if (index == 0) {
        return NULL;
    }

    return index - 1 < view->library[2].count ? &view->library[2].items[index - 1] : NULL;
}

void scene_scrollbars(Scene *scene, Canvas *canvas) {
    HitId bars[] = {HIT_SCROLL_MAIN, HIT_SCROLL_LIBRARY, HIT_SCROLL_PANEL};

    for (unsigned i = 0; i < 3; ++i) {
        HitId id = bars[i];

        if (!scene->hits[id].enabled || (id == HIT_SCROLL_MAIN && scene->panel != PANEL_NONE &&
                                         scene->content.x + scene->content.w > scene->side_panel.x)) {
            continue;
        }

        Rect thumb = scene->hits[id].rect;
        bool active = scene->dragging == id || scene->hot == id;
        canvas->box(canvas->context, thumb, thumb.w / 2,
                    active ? DARK_MUTED : color_mix(0x363b43, 0x727c8a, scene->hover[id]));
    }
}
