#include "scene_internal.h"
#include "artwork_cache.h"
#include "collection_scene.h"
#include "dark_palette.h"
#include "skeleton.h"
#include "home_scene.h"
#include "panel_scene.h"
#include "platform.h"
#include "search_scene.h"
#include "settings_scene.h"

#include <stdio.h>
#include <string.h>

#define BLACK 0x000000u
#define PANEL DARK_BODY
#define WHITE DARK_TEXT
#define MUTED DARK_MUTED
#define GREEN DARK_ACCENT

static const Icon collection_icons[] = {ICON_HEART, ICON_ALBUM, ICON_PLAYLIST, ICON_QUEUE};
static const Color collection_colors[] = {0x5b35d5, 0x157b68, 0x264cad, 0xb94b2c};

static void text(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size,
                 (bold ? TEXT_BOLD : 0) | (rect.h <= size * 2 ? TEXT_VCENTER : 0), color);
}

static void line(Canvas *canvas, float x, float y, float width, Color color) {
    canvas->box(canvas->context, (Rect){x, y, width, 1}, 0, color);
}

static void equalizer(Canvas *canvas, float x, float y) {
    canvas->box(canvas->context, (Rect){x, y + 6, 2.5f, 8}, 1, GREEN);
    canvas->box(canvas->context, (Rect){x + 4.5f, y + 1, 2.5f, 13}, 1, GREEN);
    canvas->box(canvas->context, (Rect){x + 9, y + 4, 2.5f, 10}, 1, GREEN);
}

static void pill(Scene *scene, Canvas *canvas, HitId id, const char *label, bool selected) {
    Rect rect = scene->hits[id].rect;
    Color background = selected ? WHITE : color_mix(DARK_TILE, DARK_HOVER, scene->hover[id]);

    canvas->box(canvas->context, rect, rect.h / 2, background);
    canvas->text(canvas->context, label, rect, 12, TEXT_CENTER | TEXT_VCENTER, selected ? BLACK : WHITE);
}

static void button(Scene *scene, Canvas *canvas, HitId id, const char *label, Icon icon, bool filled,
                   Color base_bg, float icon_size, bool muted_rest) {
    Hit hit = scene->hits[id];

    if (hit.rect.w <= 0) {
        return;
    }

    float hover = scene->hover[id];
    bool active = (id == HIT_QUEUE && scene->panel == PANEL_QUEUE) ||
                  (id == HIT_DEVICES && scene->panel == PANEL_DEVICES) ||
                  (id == HIT_FRIENDS && scene->panel == PANEL_FRIENDS) ||
                  (id == HIT_LYRICS && scene->panel == PANEL_LYRICS);
    Color rest = muted_rest ? color_mix(MUTED, WHITE, hover) : WHITE;
    Color foreground = active ? GREEN : (hit.enabled ? rest : 0x555555);

    if (filled) {
        canvas->box(canvas->context, hit.rect, hit.rect.h / 2, color_mix(WHITE, 0xe2e2e2, hover));
        foreground = BLACK;
    }

    if (!filled && hover > .01f) {
        canvas->box(canvas->context, hit.rect, hit.rect.h / 2,
                    color_mix(base_bg, base_bg == BLACK ? 0x222222 : 0x333333, hover));
    }

    if (label) {
        canvas->text(canvas->context, label, hit.rect, 14, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                     foreground);
    }

    if (!label) {
        canvas_icon(canvas, icon,
                    (Rect){hit.rect.x + (hit.rect.w - icon_size) / 2,
                           hit.rect.y + (hit.rect.h - icon_size) / 2, icon_size, icon_size},
                    foreground);
    }

    if (active && base_bg == DARK_CHROME) {
        canvas->ellipse(canvas->context,
                        (Rect){hit.rect.x + (hit.rect.w - 4) / 2, hit.rect.y + hit.rect.h - 1, 4, 4}, true, 1,
                        GREEN);
    }
}

static void artwork(Canvas *canvas, Rect rect, unsigned style, bool large, float radius) {
    Color top = collection_colors[style % 4];
    Color bottom = style % 4 == 0 ? 0xb1d6c1 : color_mix(top, 0x101820, .65f);

    canvas->gradient(canvas->context, rect, radius, top, bottom);

    float size = rect.w * (large ? .38f : .44f);
    canvas_icon(canvas, collection_icons[style % 4],
                (Rect){rect.x + (rect.w - size) / 2, rect.y + (rect.h - size) / 2, size, size}, WHITE);
}

static void draw_rounded_art(Canvas *canvas, const char *url, Rect rect, float radius, unsigned style,
                             bool large) {
    if (!canvas_artwork(canvas, url, rect, radius)) {
        artwork(canvas, rect, style, large, radius);
    }
}

static unsigned input_utf8_next(const char *value, unsigned pos, unsigned len) {
    if (pos >= len) {
        return len;
    }

    ++pos;

    while (pos < len && ((unsigned char)value[pos] & 0xc0) == 0x80) {
        ++pos;
    }

    return pos;
}

static float measure_prefix(Canvas *canvas, const char *value, unsigned bytes) {
    if (!bytes || !canvas->measure) {
        return 0.0f;
    }

    char scratch[SEARCH_CAPACITY];

    if (bytes >= sizeof(scratch)) {
        bytes = sizeof(scratch) - 1;
    }

    memcpy(scratch, value, bytes);
    scratch[bytes] = 0;
    return canvas->measure(canvas->context, scratch, 13);
}

static void input_text(Scene *scene, Canvas *canvas, Rect rect, const char *value, const char *placeholder,
                       HitId id, bool focused) {
    InputState *state = id == HIT_LIST_FILTER ? &scene->filter_input : &scene->search_input;
    unsigned len = (unsigned)strlen(value);

    if (state->cursor > len) {
        state->cursor = len;
    }

    if (state->anchor > len) {
        state->anchor = len;
    }

    if (focused && scene->select_all && len > 0 && state->cursor == state->anchor) {
        state->anchor = 0;
        state->cursor = len;
    }

    if (focused && scene->input_click_pending) {
        scene->input_click_pending = false;

        if (!len || !canvas->measure) {
            state->cursor = len;
            state->anchor = len;
        } else {
            float target_x = scene->input_click_x - rect.x + state->scroll_x;
            unsigned pos = 0;
            unsigned best = len;
            float prev_w = 0.0f;

            while (pos < len) {
                unsigned next = input_utf8_next(value, pos, len);
                float next_w = measure_prefix(canvas, value, next);

                if (target_x <= (prev_w + next_w) * 0.5f) {
                    best = pos;
                    break;
                }

                pos = next;
                prev_w = next_w;
            }

            state->cursor = best;
            state->anchor = best;
        }

        scene->select_all = false;
    }

    unsigned start = state->cursor < state->anchor ? state->cursor : state->anchor;
    unsigned end = state->cursor < state->anchor ? state->anchor : state->cursor;
    float visible_w = rect.w > 2.0f ? rect.w - 2.0f : 0.0f;
    float total_w = value[0] && canvas->measure ? canvas->measure(canvas->context, value, 13) : 0.0f;
    float cursor_x = focused ? measure_prefix(canvas, value, state->cursor) : 0.0f;

    if (!focused || total_w <= visible_w) {
        state->scroll_x = 0.0f;
    } else {
        if (cursor_x - state->scroll_x > visible_w) {
            state->scroll_x = cursor_x - visible_w;
        }

        if (cursor_x < state->scroll_x) {
            state->scroll_x = cursor_x;
        }

        float max_scroll = total_w - visible_w;

        if (state->scroll_x > max_scroll) {
            state->scroll_x = max_scroll;
        }

        if (state->scroll_x < 0.0f) {
            state->scroll_x = 0.0f;
        }
    }

    bool clipped = canvas->clip != NULL && canvas->unclip != NULL;

    if (clipped) {
        canvas->clip(canvas->context, rect, 0);
    }

    if (focused && start < end && canvas->measure) {
        float x0 = measure_prefix(canvas, value, start) - state->scroll_x;
        float x1 = measure_prefix(canvas, value, end) - state->scroll_x;

        if (x0 < 0.0f) {
            x0 = 0.0f;
        }

        if (x1 > rect.w) {
            x1 = rect.w;
        }

        if (x1 > x0) {
            float sel_h = rect.h < 18.0f ? rect.h : 18.0f;
            canvas->box(canvas->context,
                        (Rect){rect.x + x0, rect.y + (rect.h - sel_h) * 0.5f, x1 - x0, sel_h}, 3, 0x305374);
        }
    }

    if (!value[0]) {
        canvas->text(canvas->context, placeholder, rect, 13, TEXT_INPUT | TEXT_VCENTER, MUTED);
    } else {
        Rect text_rect =
            total_w + 8.0f > rect.w ? (Rect){rect.x - state->scroll_x, rect.y, total_w + 8.0f, rect.h} : rect;
        canvas->text(canvas->context, value, text_rect, 13, TEXT_INPUT | TEXT_VCENTER, WHITE);
    }

    if (focused && start == end && canvas->measure) {
        float caret_x = rect.x + cursor_x - state->scroll_x;

        if (caret_x > rect.x + visible_w) {
            caret_x = rect.x + visible_w;
        }

        if (caret_x < rect.x) {
            caret_x = rect.x;
        }

        canvas->box(canvas->context, (Rect){caret_x, rect.y + (rect.h - 14.0f) * 0.5f, 1, 14}, 0, WHITE);
    }

    if (clipped) {
        canvas->unclip(canvas->context);
    }
}

static void header(Scene *scene, const View *view, Canvas *canvas) {
    for (unsigned i = 0; i < 2; ++i) {
        HitId id = i ? HIT_FORWARD : HIT_BACK;
        Rect rect = scene->hits[id].rect;
        canvas->box(canvas->context, rect, rect.h / 2, color_mix(0x0b0d10, DARK_HOVER, scene->hover[id]));
        canvas_icon(canvas, i ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_LEFT,
                    (Rect){rect.x + 6, rect.y + 6, 16, 16}, scene->hits[id].enabled ? WHITE : 0x737d89);
    }

    Rect top_home = scene->hits[HIT_TOP_HOME].rect;

    if (top_home.w > 0) {
        canvas->box(canvas->context, top_home, top_home.h / 2,
                    color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_TOP_HOME]));
        canvas_icon(canvas, ICON_HOME,
                    (Rect){top_home.x + (top_home.w - 18) / 2, top_home.y + (top_home.h - 18) / 2, 18, 18},
                    scene->home ? WHITE : color_mix(MUTED, WHITE, scene->hover[HIT_TOP_HOME]));
    }

    bool search_focused = scene->focus == HIT_SEARCH;
    Color search_fill = color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_SEARCH]);
    float radius = scene->search.h * 0.5f;

    if (search_focused) {
        Rect outline = scene->search;
        canvas->box(canvas->context, outline, radius, 0x596575);
        canvas->box(canvas->context, (Rect){outline.x + 1, outline.y + 1, outline.w - 2, outline.h - 2},
                    radius - 1, search_fill);
    } else {
        canvas->box(canvas->context, scene->search, radius, search_fill);
    }

    float icon_size = 17;
    canvas_icon(canvas, ICON_SEARCH,
                (Rect){scene->search.x + 13, scene->search.y + (scene->search.h - icon_size) / 2, icon_size,
                       icon_size},
                search_focused ? WHITE : color_mix(MUTED, WHITE, scene->hover[HIT_SEARCH]));

    Rect input = {scene->search.x + 38, scene->search.y + 6, scene->search.w - (scene->query[0] ? 70 : 52),
                  scene->search.h - 12};
    input_text(scene, canvas, input, scene->query, "Search", HIT_SEARCH, search_focused);

    if (scene->query[0]) {
        Rect clear = scene->hits[HIT_SEARCH_CLEAR].rect;

        if (scene->hover[HIT_SEARCH_CLEAR] > .01f) {
            canvas->box(canvas->context, clear, clear.h / 2,
                        color_mix(search_fill, DARK_HOVER, scene->hover[HIT_SEARCH_CLEAR]));
        }

        canvas_icon(canvas, ICON_CLOSE,
                    (Rect){clear.x + (clear.w - 12) / 2, clear.y + (clear.h - 12) / 2, 12, 12},
                    color_mix(MUTED, WHITE, scene->hover[HIT_SEARCH_CLEAR]));
    }

    Rect badge = scene->hits[HIT_PLAYING_ON].rect;

    if (scene->hits[HIT_PLAYING_ON].enabled) {
        float hover = scene->hover[HIT_PLAYING_ON];
        canvas->box(canvas->context, badge, badge.h / 2, color_mix(0x10264a, 0x173463, hover));
        canvas->box(canvas->context, (Rect){badge.x + 1, badge.y + 1, badge.w - 2, badge.h - 2}, badge.h / 2 - 1,
                    color_mix(0x0c1d3a, 0x122a52, hover));
        canvas_icon(canvas, ICON_SPEAKER, (Rect){badge.x + 11, badge.y + 7, 16, 16}, 0x6cb2ff);

        char playing_on[300];
        snprintf(playing_on, sizeof(playing_on), "Playing on %s", view->playback.device);
        canvas->text(canvas->context, playing_on, (Rect){badge.x + 33, badge.y, badge.w - 44, badge.h}, 12,
                     TEXT_BOLD | TEXT_VCENTER, 0x6cb2ff);
    }

    Rect avatar = scene->hits[HIT_PROFILE].rect;
    canvas->box(canvas->context, avatar, avatar.w / 2,
                color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_PROFILE]));
    Rect inner = {avatar.x + 3, avatar.y + 3, avatar.w - 6, avatar.h - 6};
    if (!canvas_artwork(canvas, view->profile.artwork, inner, inner.w / 2)) {
        canvas_icon(canvas, ICON_USER, (Rect){inner.x + 6, inner.y + 6, inner.w - 12, inner.h - 12}, WHITE);
    }
}

static bool playlist_playing(const View *view, const CatalogItem *item) {
    return item->id[0] && !strncmp(view->playback.context_uri, "spotify:playlist:", 17) &&
           !strcmp(item->id, view->playback.context_uri + 17);
}

static bool playlist_open(const Scene *scene, const View *view, const CatalogItem *item) {
    return !scene->home &&
           (scene->section == BROWSE_PLAYLIST_TRACKS || scene->section == BROWSE_ALBUM_TRACKS) &&
           item->id[0] && !strcmp(item->id, view->list.request.id);
}

/* The collapsed library: centred icons and artwork only, like Spotify's narrow library. */
static void collapsed_library(Scene *scene, const View *view, Canvas *canvas) {
    Rect toggle = scene->hits[HIT_LIBRARY_COLLAPSE].rect;

    if (scene->hover[HIT_LIBRARY_COLLAPSE] > .01f) {
        canvas->box(canvas->context, toggle, 6,
                    color_mix(DARK_CHROME, DARK_HOVER, scene->hover[HIT_LIBRARY_COLLAPSE]));
    }

    canvas_icon(canvas, ICON_LIBRARY, (Rect){toggle.x + (toggle.w - 19) / 2, toggle.y + 6, 19, 19},
                color_mix(MUTED, WHITE, scene->hover[HIT_LIBRARY_COLLAPSE]));

    Rect liked = scene->hits[HIT_TRACKS].rect;
    bool liked_active = !scene->home && scene->section == BROWSE_SAVED_TRACKS;

    if (liked_active || scene->hover[HIT_TRACKS] > .01f) {
        canvas->box(
            canvas->context, liked, 6,
            color_mix(liked_active ? DARK_SELECTED : DARK_CHROME, DARK_HOVER, scene->hover[HIT_TRACKS]));
    }

    artwork(canvas, (Rect){liked.x + (liked.w - 39) / 2, liked.y + 7, 39, 39}, 0, false, 6);
    canvas->clip(canvas->context, scene->library_rows, 0);

    for (unsigned i = 0; i < 24; ++i) {
        HitId id = (HitId)(HIT_LIBRARY_FIRST + i);
        Rect row = scene->hits[id].rect;
        const CatalogItem *item = row.h > 0 ? list_row(&view->playlists, scene->library_first + i) : NULL;

        if (!item) {
            continue;
        }

        bool active = playlist_open(scene, view, item);

        if (active || scene->hover[id] > .01f) {
            canvas->box(canvas->context, row, 6,
                        color_mix(active ? DARK_SELECTED : DARK_CHROME, DARK_HOVER, scene->hover[id]));
        }

        Rect cover = {row.x + (row.w - 39) / 2, row.y + 7, 39, 39};
        draw_rounded_art(canvas, item->artwork, cover, 6, 2, false);

        if (playlist_playing(view, item)) {
            canvas->box(canvas->context, (Rect){cover.x + cover.w - 15, cover.y + cover.h - 15, 18, 18}, 9,
                        DARK_CHROME);
            canvas_icon(canvas, ICON_VOLUME, (Rect){cover.x + cover.w - 13, cover.y + cover.h - 13, 14, 14},
                        GREEN);
        }
    }

    canvas->unclip(canvas->context);
}

/* A thin line marks a draggable edge while it is hovered or dragged. */
static void resize_edge(Scene *scene, Canvas *canvas, HitId id, float x) {
    float hover = scene->dragging == id ? 1 : scene->hover[id];

    if (!scene->hits[id].enabled || hover <= .01f) {
        return;
    }

    Rect handle = scene->hits[id].rect;
    canvas->box(canvas->context, (Rect){x - 1, handle.y, 2, handle.h}, 1,
                color_mix(DARK_BORDER, 0x9a9a9a, hover));
}

static void sidebar(Scene *scene, const View *view, Canvas *canvas) {
    Rect bounds = scene->sidebar;
    bool collapsed = scene->library_collapsed;

    canvas->box(canvas->context, bounds, 0, DARK_CHROME);
    const HitId nav_ids[] = {HIT_HOME, HIT_SEARCH_NAV};
    const Icon nav_icons[] = {ICON_HOME, ICON_SEARCH};
    const char *nav_labels[] = {"Home", "Search"};

    for (unsigned i = 0; i < 2; ++i) {
        HitId id = nav_ids[i];
        Rect row = scene->hits[id].rect;
        bool active = i == 0 ? scene->home : !scene->home && scene->section == BROWSE_SEARCH;
        if (scene->hover[id] > .01f) {
            canvas->box(canvas->context, row, 6, color_mix(DARK_CHROME, DARK_HOVER, scene->hover[id]));
        }

        Color color = active ? WHITE : MUTED;
        float icon_x = collapsed ? row.x + (row.w - 19) / 2 : row.x + 10;
        canvas_icon(canvas, nav_icons[i], (Rect){icon_x, row.y + 9, 19, 19}, color);

        if (!collapsed) {
            canvas->text(canvas->context, nav_labels[i], (Rect){row.x + 40, row.y, row.w - 46, row.h}, 13,
                         TEXT_BOLD | TEXT_VCENTER, color);
        }
    }

    line(canvas, 14, 104, bounds.w - 26, DARK_BORDER);

    if (collapsed) {
        collapsed_library(scene, view, canvas);
        return;
    }

    Rect toggle = scene->hits[HIT_LIBRARY_COLLAPSE].rect;
    float toggle_hover = scene->hover[HIT_LIBRARY_COLLAPSE];

    if (toggle_hover > .01f) {
        canvas->box(canvas->context, toggle, 6, color_mix(DARK_CHROME, DARK_HOVER, toggle_hover));
    }

    canvas_icon(canvas, ICON_LIBRARY, (Rect){17, 118, 17, 17}, color_mix(MUTED, WHITE, toggle_hover));
    text(canvas, "Library", (Rect){44, 115, bounds.w - 89, 22}, 13, true, WHITE);
    button(scene, canvas, HIT_REFRESH, NULL, ICON_REFRESH, false, DARK_CHROME, 16, true);

    pill(scene, canvas, HIT_PLAYLISTS, "Playlists", scene->home || scene->section == BROWSE_PLAYLISTS);
    pill(scene, canvas, HIT_ALBUMS, "Albums", !scene->home && scene->section == BROWSE_SAVED_ALBUMS);
    pill(scene, canvas, HIT_ARTISTS, "Artists", !scene->home && scene->section == BROWSE_FOLLOWED_ARTISTS);

    Rect liked = scene->hits[HIT_TRACKS].rect;
    bool liked_active = !scene->home && scene->section == BROWSE_SAVED_TRACKS;

    if (liked_active || scene->hover[HIT_TRACKS] > .01f) {
        canvas->box(
            canvas->context, liked, 6,
            color_mix(liked_active ? DARK_SELECTED : DARK_CHROME, DARK_HOVER, scene->hover[HIT_TRACKS]));
    }

    artwork(canvas, (Rect){liked.x + 7, liked.y + 7, 39, 39}, 0, false, 6);
    text(canvas, "Liked Songs", (Rect){liked.x + 56, liked.y + 9, liked.w - 58, 20}, 13, false,
         liked_active ? GREEN : WHITE);
    char saved[48];

    if (view->library_loaded[0]) {
        unsigned total = view->library[0].total;
        snprintf(saved, sizeof(saved), "Playlist · %u %s", total, total == 1 ? "song" : "songs");
    } else {
        snprintf(saved, sizeof(saved), "Playlist · Saved tracks");
    }

    text(canvas, saved, (Rect){liked.x + 56, liked.y + 27, liked.w - 58, 18}, 11, false, MUTED);

    if (view->playlists.count) {
        canvas->clip(canvas->context, scene->library_rows, 0);

        for (unsigned i = 0; i < 24; ++i) {
            HitId id = (HitId)(HIT_LIBRARY_FIRST + i);
            Rect row = scene->hits[id].rect;

            if (row.h <= 0) {
                continue;
            }

            const CatalogItem *item = list_row(&view->playlists, scene->library_first + i);

            if (!item) {
                continue;
            }

            bool playing_context = playlist_playing(view, item);
            bool active = playlist_open(scene, view, item);

            if (active || scene->hover[id] > .01f) {
                canvas->box(canvas->context, row, 6,
                            color_mix(active ? DARK_SELECTED : DARK_CHROME, DARK_HOVER, scene->hover[id]));
            }

            draw_rounded_art(canvas, item->artwork, (Rect){row.x + 7, row.y + 7, 39, 39}, 6, 2, false);
            text(canvas, item->title, (Rect){row.x + 56, row.y + 9, row.w - (playing_context ? 80 : 58), 20},
                 13, false, active || playing_context ? GREEN : WHITE);
            char detail[320];
            snprintf(detail, sizeof(detail), "Playlist%s%s", item->detail[0] ? " · " : "", item->detail);
            text(canvas, detail, (Rect){row.x + 56, row.y + 27, row.w - (active ? 80 : 58), 18}, 11, false,
                 MUTED);

            if (playing_context) {
                canvas_icon(canvas, ICON_VOLUME, (Rect){row.x + row.w - 20, row.y + 19, 14, 14}, GREEN);
            }
        }

        canvas->unclip(canvas->context);
        return;
    }

    text(canvas, "Your playlists will appear here.", (Rect){18, 255, bounds.w - 36, 52}, 12, false, MUTED);
}

static void card(Scene *scene, Canvas *canvas, HitId id, const char *title, const char *subtitle,
                 unsigned style, const char *url, bool circle) {
    Rect rect = scene->hits[id].rect;

    if (rect.w <= 0 || rect.y + rect.h <= scene->content.y || rect.y >= scene->content.y + scene->content.h) {
        return;
    }

    float hover = scene->hover[id];
    canvas->box(canvas->context, rect, 8, color_mix(0x181818, 0x282828, hover));

    Rect cover = {rect.x + 12, rect.y + 12, rect.w - 24, rect.w - 24};
    draw_rounded_art(canvas, url, cover, circle ? cover.w / 2 : 6, style, true);
    text(canvas, title, (Rect){rect.x + 12, cover.y + cover.h + 10, rect.w - 24, 22}, 14, true, WHITE);
    text(canvas, subtitle, (Rect){rect.x + 12, cover.y + cover.h + 34, rect.w - 24, 36}, 12, false, MUTED);
}

static void sort_arrow(Canvas *canvas, float x, float cy, bool descending, Color color) {
    Point up[] = {{x, cy + 2.5f}, {x + 7, cy + 2.5f}, {x + 3.5f, cy - 2.5f}};
    Point down[] = {{x, cy - 2.5f}, {x + 7, cy - 2.5f}, {x + 3.5f, cy + 2.5f}};
    canvas->path(canvas->context, descending ? down : up, 3, true, true, 1, color);
}

static void sort_header(Scene *scene, const View *view, Canvas *canvas, HitId id, const char *label, Rect rect,
                        bool sortable) {
    ListSort sort = (ListSort)(id - HIT_SORT_INDEX);
    bool active = sortable && sort != LIST_SORT_NONE && view->list.sort == sort;
    Color color = active ? WHITE : color_mix(MUTED, WHITE, sortable ? scene->hover[id] : 0);
    text(canvas, label, rect, 12, false, color);

    if (!active || !canvas->measure) {
        return;
    }

    float width = canvas->measure(canvas->context, label, 12);
    sort_arrow(canvas, rect.x + width + 6, rect.y + rect.h / 2, view->list.sort_descending, GREEN);
}

static void list_filter(Scene *scene, Canvas *canvas) {
    Rect box = scene->hits[HIT_LIST_FILTER].rect;

    if (!scene->hits[HIT_LIST_FILTER].enabled) {
        return;
    }

    bool focused = scene->focus == HIT_LIST_FILTER;
    Color fill = color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_LIST_FILTER]);

    if (focused) {
        canvas->box(canvas->context, box, 4, 0x596575);
        canvas->box(canvas->context, (Rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 3, fill);
    } else {
        canvas->box(canvas->context, box, 4, fill);
    }

    canvas_icon(canvas, ICON_SEARCH, (Rect){box.x + 9, box.y + 8, 16, 16}, focused ? WHITE : MUTED);
    const char *placeholder = scene->section == BROWSE_SAVED_TRACKS ? "Search in Liked Songs"
                              : scene->section == BROWSE_SONG_RADIO ? "Search in radio"
                                                                    : "Search in playlist";
    input_text(scene, canvas, (Rect){box.x + 32, box.y + 4, box.w - 66, box.h - 8}, scene->list_filter,
               placeholder, HIT_LIST_FILTER, focused);

    Rect clear = scene->hits[HIT_LIST_FILTER_CLEAR].rect;

    if (scene->hits[HIT_LIST_FILTER_CLEAR].enabled) {
        canvas_icon(canvas, ICON_CLOSE, (Rect){clear.x + 6, clear.y + 6, 12, 12},
                    color_mix(MUTED, WHITE, scene->hover[HIT_LIST_FILTER_CLEAR]));
    }
}

static void content(Scene *scene, const View *view, Canvas *canvas) {
    Rect bounds = scene->content;
    float x = bounds.x + 24;
    float y = bounds.y - scene->main_scroll.offset;
    bool grid = scene->section == BROWSE_SAVED_ALBUMS || scene->section == BROWSE_PLAYLISTS ||
                scene->section == BROWSE_FOLLOWED_ARTISTS;
    bool loaded = view->list.loaded;
    unsigned style = scene->section == BROWSE_SAVED_TRACKS   ? 0
                     : scene->section == BROWSE_SAVED_ALBUMS ? 1
                                                             : 2;
    const char *title = browse_label(scene->section);

    if (scene->section == BROWSE_SAVED_TRACKS) {
        title = "Liked Songs";
    }

    if ((scene->section == BROWSE_ALBUM_TRACKS || scene->section == BROWSE_PLAYLIST_TRACKS ||
         scene->section == BROWSE_ARTIST_ALBUMS || scene->section == BROWSE_ARTIST_LIKES ||
         scene->section == BROWSE_SONG_RADIO) &&
        scene->collection[0]) {
        title = scene->collection;
    }

    if (grid) {
        text(canvas, title, (Rect){x, y + 36, bounds.w - 48, 48}, 32, true, WHITE);
    }

    if (!grid) {
        Color tint = style == 0 ? 0x402b93 : collection_colors[style];

        if (scene->collection_artwork[0] &&
            (scene->section == BROWSE_ALBUM_TRACKS || scene->section == BROWSE_PLAYLIST_TRACKS ||
             scene->section == BROWSE_SONG_RADIO)) {
            artwork_color(scene->collection_artwork, &tint);
        }

        Color mid = color_mix(tint, PANEL, .82f);
        canvas->gradient(canvas->context, (Rect){bounds.x, y, bounds.w, 184}, 0, tint, mid);
        canvas->gradient(canvas->context, (Rect){bounds.x, y + 184, bounds.w, 52}, 0, mid, PANEL);

        Rect cover = {x, y + 20, 156, 156};
        const char *cover_url =
            (scene->section == BROWSE_ALBUM_TRACKS || scene->section == BROWSE_PLAYLIST_TRACKS ||
             scene->section == BROWSE_ARTIST_LIKES || scene->section == BROWSE_SONG_RADIO)
                ? scene->collection_artwork
                : NULL;
        draw_rounded_art(canvas, cover_url, cover, 6, style, true);

        const char *kind_label = scene->section == BROWSE_SEARCH         ? "Search"
                                 : scene->section == BROWSE_QUEUE        ? "Queue"
                                 : scene->section == BROWSE_SAVED_TRACKS ? "Playlist"
                                                                         : "Playlist";
        float meta_x = x + 180;
        float meta_w = bounds.w - 204;
        text(canvas, kind_label, (Rect){meta_x, y + 48, meta_w, 20}, 13, true, WHITE);
        text(canvas, title, (Rect){meta_x, y + 72, meta_w, 56}, strlen(title) > 26 || meta_w < 420 ? 30 : 42,
             true, WHITE);

        char summary[384];
        unsigned total = loaded ? view->list.total : 0;
        char duration[64] = {0};

        if (loaded && view->list.duration_known && view->list.duration_ms > 0) {
            collection_duration_label(view->list.duration_ms, duration, sizeof(duration));
        }

        const char *progress = scene->section != BROWSE_ARTIST_LIKES || view->artist_likes_ready ? ""
                               : view->artist_likes_failed ? " · Incomplete; refresh to retry"
                                                          : " · Loading liked songs…";
        snprintf(summary, sizeof(summary), "%u %s%s%s%s", total,
                 scene->section == BROWSE_QUEUE ? "up next"
                 : total == 1                   ? "song"
                                                : "songs",
                 duration[0] ? ", " : "", duration, progress);

        if (scene->section == BROWSE_SEARCH && loaded) {
            snprintf(summary, sizeof(summary), "Results for %s", view->list.request.query);
        }

        if (loaded) {
            text(canvas, summary, (Rect){meta_x, y + 142, meta_w, 22}, 13, false, 0xe0e0e0);
        } else if (view->list.loading) {
            skeleton_block(canvas, (Rect){meta_x, y + 149, meta_w * .38f, 10}, 4);
        }

        Rect play = scene->hits[HIT_OPEN].rect;
        Color play_fill = scene->hits[HIT_OPEN].enabled ? color_mix(GREEN, 0x3be477, scene->hover[HIT_OPEN])
                                                        : DARK_TILE;
        canvas->box(canvas->context, play, play.h / 2, play_fill);
        canvas_icon(canvas, ICON_PLAY, (Rect){play.x + 13, play.y + 13, 18, 18}, BLACK);
        button(scene, canvas, HIT_LINK, NULL, ICON_LINK, false, PANEL, 20, true);
        list_filter(scene, canvas);

        bool sortable = scene->hits[HIT_SORT_TITLE].enabled;
        const char *middle = scene->section == BROWSE_SEARCH ? "Artist / album" : "Album";
        sort_header(scene, view, canvas, HIT_SORT_INDEX, "#", (Rect){x + 6, y + 254, 24, 20}, sortable);
        sort_header(scene, view, canvas, HIT_SORT_TITLE, "Title", (Rect){x + 40, y + 254, 110, 20}, sortable);
        sort_header(scene, view, canvas, HIT_SORT_ALBUM, middle, (Rect){x + bounds.w * .53f, y + 254, 110, 20},
                    sortable);

        bool by_duration = sortable && view->list.sort == LIST_SORT_DURATION;
        Color clock = by_duration ? WHITE : color_mix(MUTED, WHITE, scene->hover[HIT_SORT_DURATION]);
        canvas_icon(canvas, ICON_CLOCK, (Rect){bounds.x + bounds.w - 66, y + 256, 16, 16}, clock);

        if (by_duration) {
            sort_arrow(canvas, bounds.x + bounds.w - 84, y + 264, view->list.sort_descending, GREEN);
        }

        line(canvas, x, y + 277, bounds.w - 48, DARK_BORDER);
    }

    unsigned count = loaded ? view->list.count : 0;

    for (unsigned slot = 0; slot < ROW_SLOTS; ++slot) {
        HitId id = (HitId)(HIT_ROW_FIRST + slot);
        Rect row = scene->hits[id].rect;
        unsigned index = scene->first_row + slot;
        const CatalogItem *item = list_row(&view->list, index);

        if (row.w <= 0 || row.y + row.h <= bounds.y || row.y >= bounds.y + bounds.h) {
            continue;
        }

        if (!item) {
            if (view->list.loading && index >= view->list.count) {
                if (grid) {
                    skeleton_card(canvas, row, scene->section == BROWSE_FOLLOWED_ARTISTS, 0);
                } else {
                    skeleton_row(canvas, row, 48, 40, 100);
                }
            }

            continue;
        }

        if (grid) {
            card(scene, canvas, id, item->title, item->kind == ITEM_ARTIST ? "Artist" : item->detail,
                 index + style, item->artwork, item->kind == ITEM_ARTIST);
            continue;
        }

        bool selected = (int)index == view->list.selected;
        float hover = scene->hover[id];

        if (selected || hover > .01f) {
            canvas->box(canvas->context, row, 6, color_mix(selected ? 0x2a2a2a : 0x1c1c1c, 0x2a2a2a, hover));
        }

        bool playing = view->playback.uri[0] && strlen(view->playback.uri) > 22 &&
                       !strcmp(view->playback.uri + strlen(view->playback.uri) - 22, item->id);

        if (playing) {
            equalizer(canvas, row.x + 15, row.y + 21);
        } else {
            char number[16];
            snprintf(number, sizeof(number), "%u", index + 1);
            text(canvas, number, (Rect){row.x + 14, row.y + 17, 28, 22}, 14, false, MUTED);
        }

        Rect cover = scene_song_art(scene, id);
        draw_rounded_art(canvas, item->artwork[0] ? item->artwork : scene->collection_artwork, cover, 4,
                         index + style, false);
        canvas_song_play(canvas, cover, 4, hover, item_can_play(item));

        float title_w = bounds.w * .45f - 104;
        Color title_color = !item->playable ? 0x777777 : playing ? GREEN : WHITE;
        text(canvas, item->title, (Rect){row.x + 100, row.y + 7, title_w, 22}, 14, false, title_color);

        float sub_x = row.x + 100;
        float sub_w = title_w;

        if (item->explicit_content) {
            canvas->box(canvas->context, (Rect){sub_x, row.y + 33, 13, 13}, 2, 0xa7a7a7);
            canvas->text(canvas->context, "E", (Rect){sub_x, row.y + 33, 13, 13}, 9,
                         TEXT_CENTER | TEXT_VCENTER | TEXT_BOLD, PANEL);
            sub_x += 19;
            sub_w -= 19;
        }

        const char *artist_text = item->detail[0] ? item->detail : (item->playable ? "" : "Unavailable");
        text(canvas, artist_text, (Rect){sub_x, row.y + 29, sub_w, 20}, 12, false,
             color_mix(MUTED, WHITE, hover * .45f));

        const char *middle_col = item->album_title[0] ? item->album_title : item->detail;
        float middle_x = x + bounds.w * .53f;
        text(canvas, middle_col, (Rect){middle_x, row.y + 17, row.x + row.w - 103 - middle_x, 22}, 13, false,
             color_mix(MUTED, WHITE, hover * .45f));

        if (item->duration_known) {
            char dur[24];
            snprintf(dur, sizeof(dur), "%u:%02u", item->duration_ms / 60000, item->duration_ms / 1000 % 60);
            text(canvas, dur, (Rect){bounds.x + bounds.w - 72, row.y + 17, 44, 22}, 13, false, MUTED);
        }
    }

    if (!count && !view->list.loading && view->list.filter[0]) {
        char message[SEARCH_CAPACITY + 32];
        snprintf(message, sizeof(message), "No songs match \"%s\".", view->list.filter);
        text(canvas, message, (Rect){x + 12, y + 318, bounds.w - 72, 32}, 16, true, WHITE);
        return;
    }

    if (!count && !view->list.loading) {
        float empty_y = bounds.y + (grid ? 150 : 318);
        text(canvas, "Nothing here yet.", (Rect){x + 12, empty_y, bounds.w - 72, 32}, 20, true, WHITE);
        text(canvas, "Choose a collection or search above.", (Rect){x + 12, empty_y + 42, bounds.w - 72, 32},
             13, false, MUTED);
    }
}

static void player(Scene *scene, const View *view, Canvas *canvas) {
    float y = scene->player.y;
    const char *title = view->playback.title[0] ? view->playback.title : "Nothing playing yet";
    const char *artist = view->playback.title[0] ? view->playback.artist : "Choose a track from your library";
    float title_x = 77.0f;

    if (view->playback.title[0] && canvas->measure) {
        float title_w = canvas->measure(canvas->context, title, 13);
        float artist_w = canvas->measure(canvas->context, artist, 11);
        float measured = title_w > artist_w ? title_w : artist_w;

        scene->player_title_w = title_w;
        scene->player_artist_w = artist_w;
        scene->player_text_w = measured;
        scene->player_text_measured = true;
        copy_utf8(scene->player_measured_title, sizeof(scene->player_measured_title), title);
        copy_utf8(scene->player_measured_artist, sizeof(scene->player_measured_artist), artist);
        scene_player_update_text_hits(scene, view, title_w, artist_w);
    }

    bool has_like = scene->hits[HIT_PLAYER_LIKE].enabled;
    float title_width = scene_player_text_limit(scene, has_like);

    canvas->box(canvas->context, scene->player, 0, DARK_CHROME);
    line(canvas, 0, y, scene->width, DARK_BORDER);
    draw_rounded_art(canvas, view->playback.artwork, (Rect){18, y + 14, 49, 49}, 6, 1, false);

    bool song_hot = scene->hits[HIT_PLAYER_SONG].enabled && scene->hover[HIT_PLAYER_SONG] > .05f;
    bool artist_hot = scene->hits[HIT_PLAYER_ARTIST].enabled && scene->hover[HIT_PLAYER_ARTIST] > .05f;
    Color song_color = WHITE;
    Color artist_color = scene->hits[HIT_PLAYER_ARTIST].enabled
                             ? color_mix(MUTED, WHITE, scene->hover[HIT_PLAYER_ARTIST])
                             : MUTED;
    text(canvas, title, (Rect){title_x, y + 21, title_width, 20}, 13, false, song_color);

    if (song_hot && scene->hits[HIT_PLAYER_SONG].rect.w > 0.0f) {
        line(canvas, title_x, y + 38.0f, scene->hits[HIT_PLAYER_SONG].rect.w, song_color);
    }

    text(canvas, artist, (Rect){title_x, y + 39, title_width, 17}, 11, false, artist_color);

    if (artist_hot && scene->hits[HIT_PLAYER_ARTIST].rect.w > 0.0f) {
        line(canvas, title_x, y + 54.0f, scene->hits[HIT_PLAYER_ARTIST].rect.w, artist_color);
    }

    button(scene, canvas, HIT_PREVIOUS, NULL, ICON_PREVIOUS, false, DARK_CHROME, 15, true);
    button(scene, canvas, HIT_PLAY, NULL, view->playback.playing ? ICON_PAUSE : ICON_PLAY, true, DARK_CHROME,
           14, false);
    button(scene, canvas, HIT_NEXT, NULL, ICON_NEXT, false, DARK_CHROME, 15, true);
    button(scene, canvas, HIT_MUTE, NULL, scene->draft.volume == 0 && scene->hits[HIT_MUTE].enabled ? ICON_MUTE : ICON_VOLUME,
           false, DARK_CHROME, 17, true);
    button(scene, canvas, HIT_QUEUE, NULL, ICON_QUEUE, false, DARK_CHROME, 15, true);
    button(scene, canvas, HIT_DEVICES, NULL, ICON_SPEAKER, false, DARK_CHROME, 16, true);
    button(scene, canvas, HIT_LYRICS, NULL, ICON_LYRICS, false, DARK_CHROME, 15, true);

    Rect shuffle = scene->hits[HIT_SHUFFLE].rect;
    Color shuffle_color =
        !scene->hits[HIT_SHUFFLE].enabled
            ? 0x555555
            : (view->playback.shuffle ? color_mix(GREEN, 0x3be477, scene->hover[HIT_SHUFFLE])
                                      : color_mix(MUTED, WHITE, scene->hover[HIT_SHUFFLE]));
    canvas_icon(canvas, ICON_SHUFFLE, (Rect){shuffle.x + 6, shuffle.y + 6, 16, 16}, shuffle_color);

    if (view->playback.shuffle) {
        canvas->ellipse(canvas->context, (Rect){shuffle.x + 12, shuffle.y + 25, 4, 4}, true, 1, GREEN);
    }

    Rect repeat = scene->hits[HIT_REPEAT].rect;
    bool repeat_on = view->playback.repeat != REPEAT_OFF;
    Color repeat_color = !scene->hits[HIT_REPEAT].enabled
                             ? 0x555555
                             : (repeat_on ? color_mix(GREEN, 0x3be477, scene->hover[HIT_REPEAT])
                                          : color_mix(MUTED, WHITE, scene->hover[HIT_REPEAT]));
    canvas_icon(canvas, ICON_REPEAT, (Rect){repeat.x + 6, repeat.y + 6, 16, 16}, repeat_color);

    if (repeat_on) {
        canvas->ellipse(canvas->context, (Rect){repeat.x + 12, repeat.y + 25, 4, 4}, true, 1, GREEN);
    }

    if (view->playback.repeat == REPEAT_TRACK) {
        canvas->text(canvas->context, "1", (Rect){repeat.x + 9, repeat.y + 7, 10, 14}, 8,
                     TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER, GREEN);
    }

    Rect seek = scene->hits[HIT_SEEK].rect;
    unsigned fraction =
        scene->dragging == HIT_SEEK ? scene->draft.seek
        : view->playback.duration_ms
            ? (unsigned)((uint64_t)view->playback.position_ms * 1000 / view->playback.duration_ms)
            : 0;

    if (fraction > 1000) {
        fraction = 1000;
    }

    bool hot_seek = scene->hot == HIT_SEEK || scene->dragging == HIT_SEEK;
    float seek_fill = seek.w * fraction / 1000;
    canvas->box(canvas->context, (Rect){seek.x, seek.y + 6, seek.w, 2}, 1, 0x484e58);
    canvas->box(canvas->context, (Rect){seek.x, seek.y + 6, seek_fill, 2}, 1, hot_seek ? GREEN : WHITE);

    if (hot_seek && scene->hits[HIT_SEEK].enabled) {
        float knob = 8;
        canvas->ellipse(canvas->context,
                        (Rect){seek.x + seek_fill - knob / 2, seek.y + 7 - knob / 2, knob, knob}, true, 1,
                        WHITE);
    }

    char position[24];
    unsigned milliseconds = scene->dragging == HIT_SEEK
                                ? (unsigned)((uint64_t)view->playback.duration_ms * scene->draft.seek / 1000)
                                : view->playback.position_ms;
    snprintf(position, sizeof(position), "%u:%02u", milliseconds / 60000, milliseconds / 1000 % 60);
    canvas->text(canvas->context, position, (Rect){seek.x - 36, seek.y, 32, 14}, 10,
                 TEXT_CENTER | TEXT_VCENTER, MUTED);
    snprintf(position, sizeof(position), "%u:%02u", view->playback.duration_ms / 60000,
             view->playback.duration_ms / 1000 % 60);
    text(canvas, position, (Rect){seek.x + seek.w + 6, seek.y, 35, 14}, 10, false, MUTED);

    Rect volume = scene->hits[HIT_VOLUME].rect;
    bool hot_volume = scene->hot == HIT_VOLUME || scene->dragging == HIT_VOLUME;
    float vol_fill = volume.w * scene->draft.volume / 100;
    canvas->box(canvas->context, (Rect){volume.x, volume.y + 7, volume.w, 2}, 1, 0x484e58);
    canvas->box(canvas->context, (Rect){volume.x, volume.y + 7, vol_fill, 2}, 1, hot_volume ? GREEN : WHITE);

    if (hot_volume && scene->hits[HIT_VOLUME].enabled) {
        float knob = 8;
        canvas->ellipse(canvas->context,
                        (Rect){volume.x + vol_fill - knob / 2, volume.y + 8 - knob / 2, knob, knob}, true, 1,
                        WHITE);
    }
}

static void setup(Scene *scene, const View *view, Canvas *canvas) {
    if (!scene->setup_popup) {
        return;
    }

    static const char *steps[] = {
        [ENGINE_OFF] = "Log in to Spotify first.",
        [ENGINE_NEEDS_KEY] =
            "Spotify plays music on this computer through Spotify Soloist, its official "
            "background player. Create your free Soloist key (Premium account), copy it, then "
            "press Paste key. You only do this once.",
        [ENGINE_DOWNLOADING] = "Downloading Spotify Soloist from Spotify…",
        [ENGINE_STARTING] = "Starting Spotify Soloist…",
        [ENGINE_PAIRING] = "",
        [ENGINE_READY] = "Ready. Music now plays on this computer.",
        [ENGINE_FAILED] = "",
    };
    Rect panel = scene->setup_panel;
    bool instructions = view->engine == ENGINE_NEEDS_KEY;
    const char *message =
        !instructions && view->engine_message[0] ? view->engine_message : steps[view->engine];

    canvas->box(canvas->context, (Rect){panel.x - 4, panel.y - 4, panel.w + 8, panel.h + 8}, 16, 0x080808);
    canvas->box(canvas->context, panel, 14, DARK_SHEET);
    canvas_icon(canvas, ICON_DEVICE, (Rect){panel.x + 32, panel.y + 30, 28, 28}, GREEN);
    text(canvas, "Play on this computer", (Rect){panel.x + 72, panel.y + 28, panel.w - 130, 32}, 20, true,
         WHITE);
    button(scene, canvas, HIT_CLOSE_SETUP, NULL, ICON_CLOSE, false, DARK_SHEET, 18, true);
    text(canvas, message, (Rect){panel.x + 32, panel.y + 84, panel.w - 64, 96}, 14, false, MUTED);

    // A rejected paste keeps the instructions visible and explains the problem underneath.
    if (instructions && view->engine_message[0]) {
        text(canvas, view->engine_message, (Rect){panel.x + 32, panel.y + 188, panel.w - 64, 48}, 13, false,
             DARK_ERROR);
    }

    Rect key = scene->hits[HIT_SETUP_KEY].rect;
    canvas->box(canvas->context, key, key.h / 2, color_mix(DARK_CONTROL, 0x4d4d4d, scene->hover[HIT_SETUP_KEY]));
    canvas->text(canvas->context, "Get your key", key, 14, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER, WHITE);
    button(scene, canvas, HIT_SETUP_PASTE, "Paste key", ICON_LINK, true, DARK_SHEET, 18, false);

    if (scene->hits[HIT_SETUP_FORGET].enabled) {
        canvas->text(canvas->context, "Remove key", scene->hits[HIT_SETUP_FORGET].rect, 12,
                     TEXT_CENTER | TEXT_VCENTER, color_mix(MUTED, WHITE, scene->hover[HIT_SETUP_FORGET]));
    }
}

static void centered_text(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size,
                 TEXT_CENTER | (bold ? TEXT_BOLD : 0) | (rect.h <= size * 2 ? TEXT_VCENTER : 0), color);
}

static void intro(Scene *scene, const View *view, Canvas *canvas) {
    float cx = scene->width * 0.5f;
    float cy = scene->height * 0.5f;
    canvas->box(canvas->context, (Rect){cx - 180, cy - 150, 360, 290}, 20,
                color_mix(PANEL, DARK_TILE, 0.35f));
    canvas_brand_logo(canvas, (Rect){cx - 46, cy - 114, 92, 92});
    centered_text(canvas, "Native Spotify", (Rect){cx - 160, cy - 4, 320, 36}, 26, true, WHITE);
    centered_text(canvas, view->authenticated ? "Loading your music…" : "Connecting to Spotify…",
                  (Rect){cx - 160, cy + 36, 320, 24}, 13, false, MUTED);

    Rect track = {cx - 96, cy + 82, 192, 4};
    canvas->box(canvas->context, track, 2, DARK_TILE);
    float phase = (float)(platform_milliseconds() % 1300) / 1300.0f;
    float pill_w = 64.0f;
    float pill_x = track.x - pill_w * 0.5f + phase * (track.w + pill_w * 0.5f);
    float left = pill_x < track.x ? track.x : pill_x;
    float right = pill_x + pill_w > track.x + track.w ? track.x + track.w : pill_x + pill_w;

    if (right > left) {
        canvas->box(canvas->context, (Rect){left, track.y, right - left, track.h}, 2, DARK_PROGRESS);
    }
}

static void login(Scene *scene, const View *view, Canvas *canvas) {
    Rect panel = scene->login_panel;
    canvas->gradient(canvas->context, panel, 16, DARK_TILE, PANEL);
    canvas->clip(canvas->context, panel, 16);
    canvas_brand_logo(canvas, (Rect){panel.x + panel.w / 2 - 24, panel.y + 30, 48, 48});
    centered_text(canvas, "Native Spotify", (Rect){panel.x + 40, panel.y + 84, panel.w - 80, 30}, 21, true,
                  WHITE);
    centered_text(canvas, "Log in to Spotify", (Rect){panel.x + 32, panel.y + 136, panel.w - 64, 48}, 32,
                  true, WHITE);
    centered_text(canvas, "Your music and playlists. A lighter native player.",
                  (Rect){panel.x + 40, panel.y + 196, panel.w - 80, 44}, 14, false, MUTED);
    button(scene, canvas, HIT_ACCOUNT, view->connecting ? "Connecting…" : "Continue with Spotify", ICON_LINK,
           true, PANEL, 18, false);
    centered_text(canvas, "Secure sign-in opens in your browser.",
                  (Rect){panel.x + 40, panel.y + 330, panel.w - 80, 24}, 12, false, MUTED);
    centered_text(canvas, view->status, (Rect){panel.x + 40, panel.y + 370, panel.w - 80, 56}, 12, false,
                  MUTED);
    canvas->unclip(canvas->context);
}

static void song_heart(Scene *scene, Canvas *canvas, HitId id, bool liked) {
    Rect rect = scene->hits[id].rect;

    if (!scene->hits[id].enabled) {
        return;
    }

    canvas_icon(canvas, liked ? ICON_HEART : ICON_HEART_OUTLINE,
                (Rect){rect.x + (rect.w - 18) / 2, rect.y + (rect.h - 18) / 2, 18, 18},
                liked ? GREEN : color_mix(MUTED, WHITE, scene->hover[id]));
}

static void list_hearts(Scene *scene, const View *view, Canvas *canvas, HitId first, HitId last) {
    for (HitId id = first; id <= last; ++id) {
        HitId parent = id == HIT_TOP_LIKE ? HIT_SEARCH_TOP
                     : id <= HIT_ROW_LIKE_LAST ? HIT_ROW_FIRST + id - HIT_ROW_LIKE_FIRST
                                              : HIT_POPULAR_FIRST + id - HIT_POPULAR_LIKE_FIRST;

        if (scene->hot != id && scene->focus != id && scene->hot != parent && scene->hover[parent] <= .01f) {
            continue;
        }

        const CatalogItem *item = scene_like_item(scene, view, id);

        if (item) {
            song_heart(scene, canvas, id, item->liked);
        }
    }
}

static void search_history(Scene *scene, const View *view, Canvas *canvas) {
    Rect panel = scene->search_history_panel;

    if (!scene->search_history_open || panel.w <= 0) {
        return;
    }

    canvas->box(canvas->context, panel, 8, DARK_TILE);
    text(canvas, "Recent searches", (Rect){panel.x + 14, panel.y + 7, panel.w - 28, 22}, 13, true, WHITE);

    if (!view->search_history_count) {
        text(canvas, "No recent songs", (Rect){panel.x + 14, panel.y + 34, panel.w - 28, 28}, 12, false, MUTED);
        return;
    }

    Rect list = {panel.x + 5, panel.y + 34, panel.w - 10, scene->search_history_scroll.viewport};
    canvas->clip(canvas->context, list, 0);

    for (unsigned i = 0; i < view->search_history_count && i < DESKTOP_SEARCH_HISTORY_LIMIT; ++i) {
        HitId id = (HitId)(HIT_HISTORY_FIRST + i);
        HitId like = (HitId)(HIT_HISTORY_LIKE_FIRST + i);
        HitId remove = (HitId)(HIT_HISTORY_REMOVE_FIRST + i);
        Rect row = scene->hits[id].rect;

        if (row.w <= 0) {
            continue;
        }

        const CatalogItem *item = &view->search_history[i];
        bool active = scene->hot == id || scene->hot == like || scene->hot == remove || scene->focus == id;

        if (active) {
            canvas->box(canvas->context, row, 5, DARK_HOVER);
        }

        Rect art = scene_song_art(scene, id);
        draw_rounded_art(canvas, item->artwork, art, 4, i, false);
        canvas_song_play(canvas, art, 4, scene->hover[id], item_can_play(item));
        text(canvas, item->title, (Rect){row.x + 56, row.y + 5, row.w - 122, 21}, 13, true, WHITE);
        text(canvas, item->detail, (Rect){row.x + 56, row.y + 26, row.w - 122, 18}, 12, false, MUTED);
        Rect clear = scene->hits[remove].rect;

        if (clear.w > 0) {
            canvas_icon(canvas, ICON_CLOSE, (Rect){clear.x + 6, clear.y + 6, 14, 14},
                        color_mix(MUTED, WHITE, scene->hover[remove]));
        }

        if (active || scene->focus == like) {
            song_heart(scene, canvas, like, item->liked);
        }
    }

    canvas->unclip(canvas->context);
    Rect thumb = scroll_thumb(&scene->search_history_scroll, (Rect){panel.x + panel.w - 6, list.y, 3, list.h});

    if (thumb.h > 0) {
        canvas->box(canvas->context, thumb, 2, DARK_MUTED);
    }
}

void scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    const char *history_art[DESKTOP_SEARCH_HISTORY_LIMIT];
    unsigned history_count = 0;

    if (scene->search_history_open && scene->search_history_panel.w > 0) {
        for (unsigned i = 0; i < view->search_history_count && i < DESKTOP_SEARCH_HISTORY_LIMIT; ++i) {
            if (scene->hits[HIT_HISTORY_FIRST + i].rect.w > 0 && view->search_history[i].artwork[0]) {
                history_art[history_count++] = view->search_history[i].artwork;
            }
        }
    }

    // The overlay draws last; protect its covers before background artwork can evict them.
    artwork_begin_frame(history_art, history_count);
    artwork_pin(view->playback.artwork);
    artwork_pin(scene->collection_artwork);
    artwork_pin(view->details.item.artwork);
    artwork_pin(view->details.artist.artwork);
    artwork_pin(view->profile.artwork);

    for (unsigned i = 0; i < view->details.popular_count; ++i) {
        artwork_pin(view->details.popular[i].artwork);
    }

    for (unsigned i = 0; i < view->profile.following_count; ++i) {
        artwork_pin(view->profile.following[i].artwork);
    }

    if (scene->panel == PANEL_QUEUE && scene->panel_recent) {
        for (unsigned i = 0; i < view->recent_count; ++i) {
            artwork_pin(view->recent[i].artwork);
        }
    } else if (scene->panel == PANEL_QUEUE) {
        for (unsigned i = 0; i < QUEUE_VISIBLE_SLOTS; ++i) {
            const CatalogItem *item = queue_row(&view->queue, scene->panel_first + i);

            if (item) {
                artwork_pin(item->artwork);
            }
        }
    }

    if (!scene->home && scene->section == BROWSE_SEARCH && scene->search_filter == SEARCH_ALL) {
        for (unsigned i = 0; i < view->search_artist_count; ++i) {
            artwork_pin(view->search_artists[i].artwork);
        }

        for (unsigned i = 0; i < view->search_album_count; ++i) {
            artwork_pin(view->search_albums[i].artwork);
        }

        for (unsigned i = 0; i < view->search_playlist_count; ++i) {
            artwork_pin(view->search_playlists[i].artwork);
        }
    }

    for (unsigned i = 0; scene->home && i < 3 * HOME_SHELF_COLUMNS; ++i) {
        const CatalogItem *item = scene_home_item(view, i);

        if (item && scene->hits[HIT_CARD_FIRST + i].enabled) {
            artwork_pin(item->artwork);
        }
    }

    for (unsigned i = 1; scene->home && i <= HIT_HOME_QUICK_LAST - HIT_HOME_QUICK_FIRST; ++i) {
        const CatalogItem *item = scene_home_quick_item(view, i);

        if (item && scene->hits[HIT_HOME_QUICK_FIRST + i].enabled) {
            artwork_pin(item->artwork);
        }
    }

    for (unsigned i = 0; i < 24; ++i) {
        const CatalogItem *item = list_row(&view->playlists, scene->library_first + i);

        if (item && scene->hits[HIT_LIBRARY_FIRST + i].enabled) {
            artwork_pin(item->artwork);
        }
    }

    for (unsigned slot = 0; slot < ROW_SLOTS; ++slot) {
        const CatalogItem *item = list_row(&view->list, scene->first_row + slot);

        if (scene->hits[HIT_ROW_FIRST + slot].rect.w > 0 && item) {
            artwork_pin(item->artwork);
        }
    }

    canvas->box(canvas->context, (Rect){0, 0, scene->width, scene->height}, 0, PANEL);

    if (scene->intro_screen) {
        intro(scene, view, canvas);
        return;
    }

    if (scene->login_screen) {
        login(scene, view, canvas);
        return;
    }

    if (scene->lyrics_fullscreen) {
        player(scene, view, canvas);
        song_heart(scene, canvas, HIT_PLAYER_LIKE, view->playback_liked);
        panel_scene_draw(scene, view, canvas);
        scene_scrollbars(scene, canvas);
            setup(scene, view, canvas);
        return;
    }

    sidebar(scene, view, canvas);
    header(scene, view, canvas);
    canvas->clip(canvas->context, scene->content, 0);

    if (scene->home) {
        home_scene_draw(scene, view, canvas);
    }

    if (!scene->home && scene->section == BROWSE_SEARCH) {
        search_scene_draw(scene, view, canvas);
    }

    if (!scene->home && collection_page(scene->section)) {
        collection_scene_draw(scene, view, canvas);
    }

    if (!scene->home && scene->section == BROWSE_SETTINGS) {
        settings_scene_draw(scene, view, canvas);
    }

    if (!scene->home && scene->section != BROWSE_SEARCH && scene->section != BROWSE_SETTINGS &&
        !collection_page(scene->section)) {
        content(scene, view, canvas);
    }

    if (!scene->home && scene->section != BROWSE_SETTINGS) {
        list_hearts(scene, view, canvas, HIT_TOP_LIKE, HIT_POPULAR_LIKE_LAST);
    }

    if (view->status[0]) {
        Rect notice = {scene->content.x + 24, scene->content.y + scene->content.h - 52,
                       scene->content.w - 150, 36};
        canvas->box(canvas->context, notice, 8, 0x363636);
        canvas->box(canvas->context, (Rect){notice.x + 1, notice.y + 1, notice.w - 2, notice.h - 2}, 7,
                    0x222222);
        text(canvas, view->status, (Rect){notice.x + 12, notice.y + 8, notice.w - 24, 20}, 12, false, MUTED);
    }

    canvas->unclip(canvas->context);
    player(scene, view, canvas);
    song_heart(scene, canvas, HIT_PLAYER_LIKE, view->playback_liked);
    panel_scene_draw(scene, view, canvas);
    resize_edge(scene, canvas, HIT_LIBRARY_RESIZE, scene->sidebar.x + scene->sidebar.w);
    resize_edge(scene, canvas, HIT_PANEL_RESIZE, scene->side_panel.x);
    search_history(scene, view, canvas);
    scene_scrollbars(scene, canvas);
    setup(scene, view, canvas);

    if (scene->menu.open) {
        Rect r = scene->menu.rect;
        canvas->box(canvas->context, (Rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 10, 0x080808);
        canvas->box(canvas->context, r, 8, DARK_TILE);

        for (unsigned i = 0; i < scene->menu.count && i < 6; ++i) {
            HitId id = (HitId)(HIT_CTX_FIRST + i);
            Rect row = scene->hits[id].rect;
            canvas->box(canvas->context, row, 4, color_mix(DARK_TILE, DARK_HOVER, scene->hover[id]));
            static const Icon icons[] = {
                [CTX_PLAY] = ICON_PLAY,    [CTX_QUEUE] = ICON_QUEUE,  [CTX_DEQUEUE] = ICON_CLOSE,       [CTX_RADIO] = ICON_RADIO,
                [CTX_ALBUM] = ICON_ALBUM,  [CTX_ARTIST] = ICON_USER,       [CTX_LINK] = ICON_EXTERNAL_LINK,
                [CTX_PROFILE] = ICON_USER, [CTX_SETTINGS] = ICON_SETTINGS, [CTX_LOGOUT] = ICON_LOGOUT};
            ContextAction action = scene->menu.actions[i];
            Icon icon = icons[action];

            if (action == CTX_LOGOUT && !view->authenticated) {
                icon = ICON_LOGIN;
            }

            if (action == CTX_PLAY && !item_can_play(&scene->menu.item)) {
                icon = scene->menu.item.kind == ITEM_ALBUM    ? ICON_ALBUM
                       : scene->menu.item.kind == ITEM_ARTIST ? ICON_USER
                                                              : ICON_PLAYLIST;
            }

            canvas_icon(canvas, icon, (Rect){row.x + 10, row.y + (row.h - 18) / 2, 18, 18},
                        color_mix(MUTED, WHITE, scene->hover[id]));
            canvas->text(canvas->context, scene->menu.labels[i],
                         (Rect){row.x + 38, row.y, row.w - 48, row.h}, 13, TEXT_VCENTER, WHITE);
        }
    }
}
