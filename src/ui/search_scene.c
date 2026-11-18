#include "search_scene.h"

#include <stdio.h>

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

const CatalogItem *search_scene_item(const Scene *scene, const View *view, HitId id) {
    if (id == HIT_SEARCH_TOP) {
        return view->search_artist_count ? &view->search_artists[0] : list_row(&view->list, 0);
    }

    if (id >= HIT_SEARCH_ARTIST_FIRST && id <= HIT_SEARCH_ARTIST_LAST) {
        unsigned index = (unsigned)(id - HIT_SEARCH_ARTIST_FIRST);
        return index < view->search_artist_count ? &view->search_artists[index] : NULL;
    }

    if (id >= HIT_SEARCH_ALBUM_FIRST && id <= HIT_SEARCH_ALBUM_LAST) {
        unsigned index = (unsigned)(id - HIT_SEARCH_ALBUM_FIRST);
        return index < view->search_album_count ? &view->search_albums[index] : NULL;
    }

    if (id >= HIT_ROW_FIRST && id <= HIT_ROW_LAST) {
        return list_row(&view->list, scene->first_row + (unsigned)(id - HIT_ROW_FIRST));
    }

    return NULL;
}

void search_scene_layout(Scene *scene, const View *view) {
    float x = scene->content.x + 24;
    float width = scene->content.w - 48;

    for (unsigned i = 0; i < 4; ++i) {
        scene->hits[HIT_SEARCH_ALL + i] = (Hit){{x + i * 86, scene->content.y + 16, 78, 32}, true};
    }

    if (scene->search_filter == SEARCH_ALL) {
        scene->first_row = 0;
        unsigned columns = scene_columns(scene->content.w);
        float card = (width - (columns - 1) * 16) / columns;
        scroll_layout(&scene->main_scroll, 2 * card + 666, scene->content.h);
        scene->search_scroll = scene->main_scroll.offset;
        float y = scene->content.y + 84 - scene->search_scroll;
        float left = width * .4f;
        scene->hits[HIT_SEARCH_TOP] = (Hit){{x, y + 42, left, 232}, view->list.loaded};

        for (unsigned i = 0; i < 4 && i < view->list.count; ++i) {
            scene->hits[HIT_ROW_FIRST + i] =
                (Hit){{x + left + 24, y + 42 + i * 58, width - left - 24, 58}, true};
        }

        for (unsigned i = 0; i < columns; ++i) {
            if (i < view->search_artist_count) {
                scene->hits[HIT_SEARCH_ARTIST_FIRST + i] =
                    (Hit){{x + i * (card + 16), y + 350, card, card + 72}, true};
            }

            if (i < view->search_album_count) {
                scene->hits[HIT_SEARCH_ALBUM_FIRST + i] =
                    (Hit){{x + i * (card + 16), y + card + 494, card, card + 72}, true};
            }
        }

        return;
    }

    bool grid = scene->search_filter == SEARCH_ARTISTS || scene->search_filter == SEARCH_ALBUMS;
    unsigned columns = grid ? scene_columns(scene->content.w) : 1;
    float card = (width - (columns - 1) * 16) / columns;
    float row_height = grid ? card + 88 : 58;
    unsigned rows = (view->list.count + columns - 1) / columns;
    scroll_layout(&scene->main_scroll, 84 + rows * row_height + 24, scene->content.h);
    scene->main_origin = 20;
    scene->main_row_height = row_height;
    scene->main_columns = columns;
    unsigned visible = scene->content.h > 84
                           ? (unsigned)((scene->content.h - 84 + (grid ? 16 : 0)) / row_height) * columns
                           : 0;

    if (visible > ROW_SLOTS) {
        visible = ROW_SLOTS;
    }

    float row_offset = scene->main_scroll.offset > 20 ? scene->main_scroll.offset - 20 : 0;
    scene->first_row = (unsigned)(row_offset / row_height) * columns;

    if (scene->first_row > view->list.count) {
        scene->first_row = view->list.count;
    }

    if (view->list.selected != scene->last_selected && view->list.selected >= 0 && visible) {
        unsigned selected = (unsigned)view->list.selected;

        if (selected < scene->first_row) {
            scene->first_row = selected / columns * columns;
            scroll_to(&scene->main_scroll, 20 + (float)(scene->first_row / columns) * row_height, 0, false);
        }

        if (selected >= scene->first_row + visible) {
            scene->first_row = (selected / columns - visible / columns + 1) * columns;
            scroll_to(&scene->main_scroll, 20 + (float)(scene->first_row / columns) * row_height, 0, false);
        }
    }

    scene->last_selected = view->list.selected;
    scene->search_scroll = scene->main_scroll.offset;
    float y = scene->content.y + 84 - scene->main_scroll.offset;
    unsigned shown = 0;

    for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < view->list.count; ++slot) {
        unsigned index = scene->first_row + slot;
        Rect rect = {x + (index % columns) * (card + 16), y + (index / columns) * row_height, card,
                     grid ? card + 72 : 58};

        if (rect.y >= scene->content.y + scene->content.h) {
            break;
        }

        scene->hits[HIT_ROW_FIRST + slot] = (Hit){rect, rect.y + rect.h > scene->content.y + 64 &&
                                                            rect.y < scene->content.y + scene->content.h};
        shown = slot + 1;
    }

    if (view->list.more && !view->list.loading && scene->first_row + shown + ROW_SLOTS >= view->list.count) {
        app_load_more();
    }
}

static void cover(Canvas *canvas, const CatalogItem *item, Rect rect) {
    float radius = item->kind == ITEM_ARTIST ? rect.w / 2 : 4;
    canvas->clip(canvas->context, rect, radius);

    if (!canvas->image(canvas->context, item->artwork, rect)) {
        canvas->box(canvas->context, rect, radius, 0x282828);
        canvas_icon(canvas, item->kind == ITEM_ARTIST ? ICON_LIBRARY : ICON_ALBUM,
                    (Rect){rect.x + rect.w * .3f, rect.y + rect.h * .3f, rect.w * .4f, rect.h * .4f},
                    0xb3b3b3);
    }

    canvas->unclip(canvas->context);
}

static void result(Scene *scene, Canvas *canvas, HitId id, const CatalogItem *item, bool top, bool song) {
    Rect rect = scene->hits[id].rect;

    if (!item || rect.w <= 0 || rect.y + rect.h < scene->content.y + 64 ||
        rect.y >= scene->content.y + scene->content.h) {
        return;
    }

    canvas->box(canvas->context, rect, 6, color_mix(song ? 0x121212 : 0x181818, 0x282828, scene->hover[id]));

    if (song) {
        cover(canvas, item, (Rect){rect.x + 8, rect.y + 7, 44, 44});
        label(canvas, item->title, (Rect){rect.x + 64, rect.y + 7, rect.w - 124, 22}, 14, false, 0xffffff);
        label(canvas, item->detail, (Rect){rect.x + 64, rect.y + 30, rect.w - 124, 20}, 12, false, 0xb3b3b3);
        char duration[24];
        snprintf(duration, sizeof(duration), "%u:%02u", item->duration_ms / 60000,
                 item->duration_ms / 1000 % 60);
        label(canvas, duration, (Rect){rect.x + rect.w - 48, rect.y + 18, 44, 24}, 12, false, 0xb3b3b3);
        return;
    }

    float side = top ? 88 : rect.w - 24;
    cover(canvas, item, (Rect){rect.x + (top ? 20 : 12), rect.y + (top ? 20 : 12), side, side});
    label(canvas, item->title,
          (Rect){rect.x + (top ? 20 : 12), rect.y + side + (top ? 40 : 24), rect.w - 32, top ? 44 : 24},
          top ? 28 : 15, true, 0xffffff);
    label(canvas,
          item->kind == ITEM_ARTIST  ? "Artist"
          : item->kind == ITEM_ALBUM ? "Album"
                                     : item->detail,
          (Rect){rect.x + (top ? 20 : 12), rect.y + side + (top ? 94 : 54), rect.w - 32, 22}, 12, false,
          0xb3b3b3);
}

void search_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    float x = scene->content.x + 24;
    float y = scene->content.y + 84 - scene->search_scroll;
    float width = scene->content.w - 48;
    canvas->clip(canvas->context,
                 (Rect){scene->content.x, scene->content.y + 64, scene->content.w, scene->content.h - 64}, 0);

    if (scene->search_filter == SEARCH_ALL) {
        label(canvas, "Top result", (Rect){x, y, width * .4f, 34}, 24, true, 0xffffff);
        label(canvas, "Songs", (Rect){x + width * .4f + 24, y, width * .6f - 24, 34}, 24, true, 0xffffff);
        result(scene, canvas, HIT_SEARCH_TOP, search_scene_item(scene, view, HIT_SEARCH_TOP), true, false);
        unsigned columns = scene_columns(scene->content.w);
        float card = (width - (columns - 1) * 16) / columns;
        label(canvas, "Artists", (Rect){x, y + 304, width, 34}, 24, true, 0xffffff);
        label(canvas, "Albums", (Rect){x, y + card + 448, width, 34}, 24, true, 0xffffff);

        for (unsigned i = 0; i < columns; ++i) {
            result(scene, canvas, (HitId)(HIT_SEARCH_ARTIST_FIRST + i),
                   search_scene_item(scene, view, (HitId)(HIT_SEARCH_ARTIST_FIRST + i)), false, false);
            result(scene, canvas, (HitId)(HIT_SEARCH_ALBUM_FIRST + i),
                   search_scene_item(scene, view, (HitId)(HIT_SEARCH_ALBUM_FIRST + i)), false, false);
        }
    }

    for (unsigned slot = 0; slot < ROW_SLOTS; ++slot) {
        HitId id = (HitId)(HIT_ROW_FIRST + slot);
        result(scene, canvas, id, search_scene_item(scene, view, id), false,
               scene->search_filter == SEARCH_ALL || scene->search_filter == SEARCH_SONGS);
    }

    if (!view->list.loading && !view->list.count && !view->search_artist_count && !view->search_album_count) {
        label(canvas, "No results found", (Rect){x, y + 48, width, 36}, 24, true, 0xffffff);
    }

    canvas->unclip(canvas->context);
    canvas->box(canvas->context, (Rect){scene->content.x, scene->content.y, scene->content.w, 64}, 0,
                0x121212);
    static const char *filters[] = {"All", "Songs", "Artists", "Albums"};

    for (unsigned i = 0; i < 4; ++i) {
        HitId id = (HitId)(HIT_SEARCH_ALL + i);
        Rect rect = scene->hits[id].rect;
        bool active = scene->search_filter == (SearchFilter)i;
        canvas->box(canvas->context, rect, 16,
                    active ? 0xffffff : color_mix(0x232323, 0x333333, scene->hover[id]));
        canvas->text(canvas->context, filters[i], rect, 13, TEXT_CENTER | TEXT_VCENTER,
                     active ? 0 : 0xffffff);
    }
}
