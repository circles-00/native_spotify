#include "search_scene.h"
#include "dark_palette.h"
#include "skeleton.h"

#include <stdio.h>

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

const CatalogItem *search_scene_item(const Scene *scene, const View *view, HitId id) {
    if (id == HIT_SEARCH_TOP) {
        const char *query = view->list.request.query[0] ? view->list.request.query : scene->query;
        const CatalogItem *tracks = view->list.first == 0 ? view->list.rows : NULL;
        unsigned track_count = 0;

        if (tracks) {
            track_count = view->list.rows_count < view->list.count ? view->list.rows_count : view->list.count;
        }

        const CatalogItem *top = browse_search_top(
            query, tracks, track_count, view->search_artists, view->search_artist_count, view->search_albums,
            view->search_album_count, view->search_playlists, view->search_playlist_count);
        return top ? top : list_row(&view->list, 0);
    }

    if (id >= HIT_SEARCH_ARTIST_FIRST && id <= HIT_SEARCH_ARTIST_LAST) {
        unsigned index = (unsigned)(id - HIT_SEARCH_ARTIST_FIRST);
        return index < view->search_artist_count ? &view->search_artists[index] : NULL;
    }

    if (id >= HIT_SEARCH_ALBUM_FIRST && id <= HIT_SEARCH_ALBUM_LAST) {
        unsigned index = (unsigned)(id - HIT_SEARCH_ALBUM_FIRST);
        return index < view->search_album_count ? &view->search_albums[index] : NULL;
    }

    if (id >= HIT_SEARCH_PLAYLIST_FIRST && id <= HIT_SEARCH_PLAYLIST_LAST) {
        unsigned index = (unsigned)(id - HIT_SEARCH_PLAYLIST_FIRST);
        return index < view->search_playlist_count ? &view->search_playlists[index] : NULL;
    }

    if (id >= HIT_ROW_FIRST && id <= HIT_ROW_LAST) {
        return list_row(&view->list, scene->first_row + (unsigned)(id - HIT_ROW_FIRST));
    }

    return NULL;
}

void search_scene_layout(Scene *scene, const View *view) {
    float x = scene->content.x + 24;
    float width = scene->content.w - 48;

    for (unsigned i = 0; i < 5; ++i) {
        scene->hits[HIT_SEARCH_ALL + i] = (Hit){{x + i * 88, scene->content.y + 16, 80, 32}, true};
    }

    if (scene->search_filter == SEARCH_ALL) {
        scene->first_row = 0;
        unsigned columns = scene_columns(scene->content.w);
        float card = (width - (columns - 1) * 16) / columns;
        bool loading = view->list.loading && !view->list.loaded;
        bool show_playlists = loading || view->search_playlist_count > 0;
        scroll_layout(&scene->main_scroll, show_playlists ? 3 * card + 810 : 2 * card + 666,
                      scene->content.h);
        scene->search_scroll = scene->main_scroll.offset;
        float y = scene->content.y + 84 - scene->search_scroll;
        float left = width * .4f;
        scene->hits[HIT_SEARCH_TOP] = (Hit){{x, y + 42, left, 232}, view->list.loaded};

        for (unsigned i = 0; i < 4 && (loading || i < view->list.count); ++i) {
            scene->hits[HIT_ROW_FIRST + i] =
                (Hit){{x + left + 24, y + 42 + i * 58, width - left - 24, 58}, i < view->list.count};
        }

        for (unsigned i = 0; i < columns; ++i) {
            if (loading || i < view->search_artist_count) {
                scene->hits[HIT_SEARCH_ARTIST_FIRST + i] =
                    (Hit){{x + i * (card + 16), y + 350, card, card + 72}, i < view->search_artist_count};
            }

            if (loading || i < view->search_album_count) {
                scene->hits[HIT_SEARCH_ALBUM_FIRST + i] = (Hit){
                    {x + i * (card + 16), y + card + 494, card, card + 72}, i < view->search_album_count};
            }

            if (show_playlists && (loading || i < view->search_playlist_count)) {
                scene->hits[HIT_SEARCH_PLAYLIST_FIRST + i] =
                    (Hit){{x + i * (card + 16), y + 2 * card + 638, card, card + 72},
                          i < view->search_playlist_count};
            }
        }

        return;
    }

    bool grid = scene->search_filter == SEARCH_ARTISTS || scene->search_filter == SEARCH_ALBUMS ||
                scene->search_filter == SEARCH_PLAYLISTS;
    unsigned columns = grid ? scene_columns(scene->content.w) : 1;
    float card = (width - (columns - 1) * 16) / columns;
    float row_height = grid ? card + 88 : 58;
    unsigned display_count = skeleton_count(&view->list, grid ? columns : 5);
    unsigned rows = (display_count + columns - 1) / columns;
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

    for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < display_count; ++slot) {
        unsigned index = scene->first_row + slot;
        Rect rect = {x + (index % columns) * (card + 16), y + (index / columns) * row_height, card,
                     grid ? card + 72 : 58};

        if (rect.y >= scene->content.y + scene->content.h) {
            break;
        }

        scene->hits[HIT_ROW_FIRST + slot] =
            (Hit){rect, index < view->list.count && rect.y + rect.h > scene->content.y + 64};
        shown = slot + 1;
    }

    if (view->list.more && !view->list.loading && scene->first_row + shown + ROW_SLOTS >= view->list.count) {
        app_load_more();
    }
}

static void cover(Canvas *canvas, const CatalogItem *item, Rect rect, float non_artist_radius) {
    float radius = item->kind == ITEM_ARTIST ? rect.w / 2 : non_artist_radius;
    Icon fallback = item->kind == ITEM_ARTIST     ? ICON_USER
                    : item->kind == ITEM_PLAYLIST ? ICON_PLAYLIST
                                                  : ICON_ALBUM;

    if (!canvas_artwork(canvas, item->artwork, rect, radius)) {
        canvas->box(canvas->context, rect, radius, DARK_TILE);
        canvas_icon(canvas, fallback,
                    (Rect){rect.x + rect.w * .3f, rect.y + rect.h * .3f, rect.w * .4f, rect.h * .4f},
                    0xb3b3b3);
    }
}

static void result(Scene *scene, Canvas *canvas, HitId id, const CatalogItem *item, bool top, bool song,
                   bool loading) {
    Rect rect = scene->hits[id].rect;

    if (rect.w <= 0 || rect.y + rect.h < scene->content.y + 64 ||
        rect.y >= scene->content.y + scene->content.h) {
        return;
    }

    if (!item) {
        if (loading && song) {
            skeleton_row(canvas, rect, 8, 44, 64);
        } else if (loading) {
            skeleton_card(canvas, rect,
                          scene->search_filter == SEARCH_ARTISTS ||
                              (id >= HIT_SEARCH_ARTIST_FIRST && id <= HIT_SEARCH_ARTIST_LAST),
                          top ? 88 : 0);
        }

        return;
    }

    float hover = scene->hover[id];

    if (!song || hover > .01f) {
        canvas->box(canvas->context, rect, song ? 6 : 8,
                    color_mix(song ? DARK_BODY : DARK_TILE, DARK_HOVER, hover));
    }

    if (song) {
        Rect art = scene_song_art(scene, id);
        cover(canvas, item, art, 4);
        canvas_song_play(canvas, art, 4, hover, item_can_play(item));
        label(canvas, item->title, (Rect){rect.x + 64, rect.y + 7, rect.w - 124, 22}, 14, false, 0xffffff);

        float sub_x = rect.x + 64;
        float sub_w = rect.w - 124;

        if (item->explicit_content) {
            canvas->box(canvas->context, (Rect){sub_x, rect.y + 33, 13, 13}, 2, 0xa7a7a7);
            canvas->text(canvas->context, "E", (Rect){sub_x, rect.y + 33, 13, 13}, 9,
                         TEXT_CENTER | TEXT_VCENTER | TEXT_BOLD, 0x121212);
            sub_x += 19;
            sub_w -= 19;
        }

        label(canvas, item->detail, (Rect){sub_x, rect.y + 30, sub_w, 20}, 12, false,
              color_mix(0xb3b3b3, 0xffffff, hover * .45f));

        char duration[24];
        snprintf(duration, sizeof(duration), "%u:%02u", item->duration_ms / 60000,
                 item->duration_ms / 1000 % 60);
        label(canvas, duration, (Rect){rect.x + rect.w - 48, rect.y + 18, 44, 24}, 12, false, 0xb3b3b3);
        return;
    }

    Rect art = top ? scene_song_art(scene, id) : (Rect){rect.x + 12, rect.y + 12, rect.w - 24, rect.w - 24};
    cover(canvas, item, art, 6);
    canvas_song_play(canvas, art, 6, hover, item_can_play(item));
    label(canvas, item->title,
          (Rect){rect.x + (top ? 20 : 12), art.y + art.h + (top ? 20 : 10), rect.w - (top ? 40 : 24),
                 top ? 44 : 22},
          top ? 28 : 14, true, 0xffffff);

    char subtitle[320];

    if (item->kind == ITEM_ARTIST) {
        snprintf(subtitle, sizeof(subtitle), "Artist");
    } else if (item->kind == ITEM_PLAYLIST) {
        snprintf(subtitle, sizeof(subtitle), "Playlist%s%s", item->detail[0] ? " · " : "", item->detail);
    } else if (top && item->kind == ITEM_ALBUM) {
        snprintf(subtitle, sizeof(subtitle), "Album%s%s", item->detail[0] ? " · " : "", item->detail);
    } else if (top && item->kind == ITEM_TRACK) {
        snprintf(subtitle, sizeof(subtitle), "Song%s%s", item->detail[0] ? " · " : "", item->detail);
    } else if (item->kind == ITEM_ALBUM) {
        snprintf(subtitle, sizeof(subtitle), "%s", item->detail[0] ? item->detail : "Album");
    } else {
        snprintf(subtitle, sizeof(subtitle), "%s", item->detail);
    }

    float sub_w = (top && item->kind == ITEM_TRACK) ? rect.w - 94 : rect.w - (top ? 40 : 24);
    label(canvas, subtitle,
          (Rect){rect.x + (top ? 20 : 12), art.y + art.h + (top ? 72 : 34), sub_w, top ? 22 : 36}, 12, false,
          0xb3b3b3);
}

void search_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    float x = scene->content.x + 24;
    float y = scene->content.y + 84 - scene->search_scroll;
    float width = scene->content.w - 48;

    canvas->clip(canvas->context,
                 (Rect){scene->content.x, scene->content.y + 64, scene->content.w, scene->content.h - 64}, 0);

    if (scene->search_filter == SEARCH_ALL) {
        bool loading = view->list.loading && !view->list.loaded;
        label(canvas, "Top result", (Rect){x, y, width * .4f, 34}, 24, true, 0xffffff);
        label(canvas, "Songs", (Rect){x + width * .4f + 24, y, width * .6f - 24, 34}, 24, true, 0xffffff);
        result(scene, canvas, HIT_SEARCH_TOP, search_scene_item(scene, view, HIT_SEARCH_TOP), true, false,
               loading);
        unsigned columns = scene_columns(scene->content.w);
        float card = (width - (columns - 1) * 16) / columns;
        label(canvas, "Artists", (Rect){x, y + 304, width, 34}, 24, true, 0xffffff);
        label(canvas, "Albums", (Rect){x, y + card + 448, width, 34}, 24, true, 0xffffff);

        if (loading || view->search_playlist_count > 0) {
            label(canvas, "Playlists", (Rect){x, y + 2 * card + 592, width, 34}, 24, true, 0xffffff);
        }

        for (unsigned i = 0; i < columns; ++i) {
            result(scene, canvas, (HitId)(HIT_SEARCH_ARTIST_FIRST + i),
                   search_scene_item(scene, view, (HitId)(HIT_SEARCH_ARTIST_FIRST + i)), false, false,
                   loading);
            result(scene, canvas, (HitId)(HIT_SEARCH_ALBUM_FIRST + i),
                   search_scene_item(scene, view, (HitId)(HIT_SEARCH_ALBUM_FIRST + i)), false, false,
                   loading);

            if (loading || view->search_playlist_count > 0) {
                result(scene, canvas, (HitId)(HIT_SEARCH_PLAYLIST_FIRST + i),
                       search_scene_item(scene, view, (HitId)(HIT_SEARCH_PLAYLIST_FIRST + i)), false, false,
                       loading);
            }
        }
    }

    for (unsigned slot = 0; slot < ROW_SLOTS; ++slot) {
        HitId id = (HitId)(HIT_ROW_FIRST + slot);
        result(scene, canvas, id, search_scene_item(scene, view, id), false,
               scene->search_filter == SEARCH_ALL || scene->search_filter == SEARCH_SONGS,
               view->list.loading && scene->first_row + slot >= view->list.count);
    }

    if (!view->list.loading && !view->list.count && !view->search_artist_count && !view->search_album_count &&
        !view->search_playlist_count) {
        label(canvas, "No results found", (Rect){x, y + 48, width, 36}, 24, true, 0xffffff);
    }

    canvas->unclip(canvas->context);
    canvas->box(canvas->context, (Rect){scene->content.x, scene->content.y, scene->content.w, 64}, 0,
                DARK_BODY);
    static const char *filters[] = {"All", "Songs", "Artists", "Albums", "Playlists"};

    for (unsigned i = 0; i < 5; ++i) {
        HitId id = (HitId)(HIT_SEARCH_ALL + i);
        Rect rect = scene->hits[id].rect;
        bool active = scene->search_filter == (SearchFilter)i;
        canvas->box(canvas->context, rect, 16,
                    active ? DARK_TEXT : color_mix(DARK_TILE, DARK_HOVER, scene->hover[id]));
        canvas->text(canvas->context, filters[i], rect, 13, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                     active ? 0 : 0xffffff);
    }
}
