#include "home_scene.h"
#include "dark_palette.h"

#include <time.h>

static void label(Canvas *canvas, const char *value, Rect rect, float size, unsigned style, Color color) {
    canvas->text(canvas->context, value, rect, size, style | TEXT_VCENTER, color);
}

static void place(Scene *scene, HitId id, Rect rect) {
    scene->hits[id] =
        (Hit){rect, rect.y + rect.h > scene->content.y && rect.y < scene->content.y + scene->content.h};
}

static unsigned quick_count(const View *view) {
    unsigned playlists = view->library[2].count;

    if (!playlists) {
        return 4;
    }

    return playlists < 7 ? playlists + 1 : 8;
}

void home_scene_layout(Scene *scene, const View *view) {
    float x = scene->content.x + 21;
    float width = scene->content.w - 42;
    unsigned columns = width >= 900 ? 4 : 2;
    unsigned count = quick_count(view);
    float tile = (width - (columns - 1) * 9) / columns;
    float shelf_y = 58 + ((count + columns - 1) / columns) * 59 + 18;
    unsigned shelf_columns = (unsigned)((width + 16) / 153);

    if (shelf_columns < 2) {
        shelf_columns = 2;
    }

    if (shelf_columns > HOME_SHELF_COLUMNS) {
        shelf_columns = HOME_SHELF_COLUMNS;
    }

    float card = (width - (shelf_columns - 1) * 10) / shelf_columns;

    if (card > 149) {
        card = 149;
    }
    float headings[3];

    for (unsigned shelf = 0; shelf < 3; ++shelf) {
        headings[shelf] = shelf_y;
        shelf_y += scene_home_item(view, shelf * HOME_SHELF_COLUMNS) ? card + 103 : 60;
    }

    scene->home_extent = shelf_y;
    scroll_layout(&scene->main_scroll, scene->home_extent, scene->content.h);
    scene->home_scroll = scene->main_scroll.offset;
    float y = scene->content.y - scene->home_scroll;

    if (view->recent_permission_required) {
        place(scene, HIT_HOME_HISTORY, (Rect){x + width - 168, y + 14, 168, 28});
    }

    for (unsigned i = 0; i < count; ++i) {
        place(scene, (HitId)(HIT_HOME_QUICK_FIRST + i),
              (Rect){x + i % columns * (tile + 9), y + 58 + i / columns * 59, tile, 53});
    }

    const HitId headings_ids[] = {scene_home_made_for_you(view) ? HIT_NONE : HIT_HOME_PLAYLISTS, HIT_NONE,
                                  HIT_HOME_ALBUMS};

    for (unsigned shelf = 0; shelf < 3; ++shelf) {
        scene->home_headings[shelf] = y + headings[shelf];

        if (headings_ids[shelf] != HIT_NONE) {
            place(scene, headings_ids[shelf], (Rect){x + width - 72, y + headings[shelf], 72, 24});
        }

        for (unsigned i = 0; i < shelf_columns; ++i) {
            unsigned index = shelf * HOME_SHELF_COLUMNS + i;

            if (scene_home_item(view, index)) {
                place(scene, (HitId)(HIT_CARD_FIRST + index),
                      (Rect){x + i * (card + 10), y + headings[shelf] + 31, card, card + 44});
            }
        }
    }
}

static void cover(Canvas *canvas, const CatalogItem *item, Rect rect, Icon fallback) {
    if (!canvas_artwork(canvas, item ? item->artwork : NULL, rect, 6)) {
        if (fallback == ICON_HEART) {
            canvas->gradient(canvas->context, rect, 6, 0x4e21dc, 0xb5e2d5);
        } else {
            canvas->box(canvas->context, rect, 6, DARK_HOVER);
        }

        float size = rect.w <= 53 ? 23 : 34;
        canvas_icon(canvas, fallback,
                    (Rect){rect.x + (rect.w - size) / 2, rect.y + (rect.h - size) / 2, size, size},
                    DARK_TEXT);
    }
}

void home_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    Rect bounds = scene->content;
    float x = bounds.x + 21;
    float width = bounds.w - 42;
    float y = bounds.y - scene->home_scroll;
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    int hour = local ? local->tm_hour : 12;
    const char *greeting = hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
    label(canvas, greeting, (Rect){x, y + 6, width - (view->recent_permission_required ? 176 : 0), 38}, 27,
          TEXT_BOLD, DARK_TEXT);

    if (scene->hits[HIT_HOME_HISTORY].enabled) {
        Rect rect = scene->hits[HIT_HOME_HISTORY].rect;
        canvas->box(canvas->context, rect, 14,
                    color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_HOME_HISTORY]));
        label(canvas, "Enable recently played", rect, 12, TEXT_CENTER, DARK_TEXT);
    }

    static const char *shortcuts[] = {"Liked Songs", "Albums", "Playlists", "Search"};
    static const Icon icons[] = {ICON_HEART, ICON_ALBUM, ICON_PLAYLIST, ICON_SEARCH};

    for (unsigned i = 0; i < quick_count(view); ++i) {
        HitId id = (HitId)(HIT_HOME_QUICK_FIRST + i);

        if (!scene->hits[id].enabled) {
            continue;
        }

        Rect rect = scene->hits[id].rect;
        const CatalogItem *item = scene_home_quick_item(view, i);
        canvas->box(canvas->context, rect, 6, color_mix(DARK_TILE, DARK_HOVER, scene->hover[id]));
        cover(canvas, item, (Rect){rect.x, rect.y, 53, 53}, item ? ICON_PLAYLIST : icons[i < 4 ? i : 2]);
        label(canvas, item ? item->title : shortcuts[i < 4 ? i : 2],
              (Rect){rect.x + 64, rect.y, rect.w - 75, rect.h}, 13, TEXT_BOLD, DARK_TEXT);
    }

    const char *titles[] = {scene_home_made_for_you(view) ? "Made for you" : "Your playlists",
                            "Recently played", "Saved albums"};
    static const char *empty[] = {"Your playlists will appear here.",
                                  "Albums you listen to will appear here.", "Save albums to find them here."};

    for (unsigned shelf = 0; shelf < 3; ++shelf) {
        float top = scene->home_headings[shelf];
        label(canvas, titles[shelf], (Rect){x, top, width - 82, 24}, 16, TEXT_BOLD, DARK_TEXT);

        if (!scene_home_item(view, shelf * HOME_SHELF_COLUMNS)) {
            label(canvas, empty[shelf], (Rect){x, top + 24, width, 23}, 12, 0, DARK_MUTED);
        }
    }

    const HitId links[] = {HIT_HOME_PLAYLISTS, HIT_HOME_ALBUMS};

    for (unsigned i = 0; i < 2; ++i) {
        HitId id = links[i];

        if (scene->hits[id].enabled) {
            label(canvas, "Show all", scene->hits[id].rect, 12, TEXT_CENTER,
                  color_mix(DARK_MUTED, DARK_TEXT, scene->hover[id]));
        }
    }

    for (unsigned i = 0; i < 3 * HOME_SHELF_COLUMNS; ++i) {
        HitId id = (HitId)(HIT_CARD_FIRST + i);
        const CatalogItem *item = scene_home_item(view, i);

        if (!item || !scene->hits[id].enabled) {
            continue;
        }

        Rect rect = scene->hits[id].rect;
        canvas->box(canvas->context, rect, 6, color_mix(DARK_BODY, DARK_TILE, scene->hover[id]));
        float art = rect.w - 18;
        cover(canvas, item, (Rect){rect.x + 10, rect.y + 10, art, art},
              i < HOME_SHELF_COLUMNS ? ICON_PLAYLIST : ICON_ALBUM);
        label(canvas, item->title, (Rect){rect.x + 10, rect.y + art + 18, art, 17}, 12, TEXT_BOLD, DARK_TEXT);
        canvas->text(canvas->context, item->detail, (Rect){rect.x + 10, rect.y + art + 35, art, 30}, 11, 0,
                     DARK_MUTED);
    }
}
