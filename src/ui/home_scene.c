#include "home_scene.h"

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

static void place(Scene *scene, HitId id, Rect rect) {
    scene->hits[id] =
        (Hit){rect, rect.y + rect.h > scene->content.y + 56 && rect.y < scene->content.y + scene->content.h};
}

void home_scene_layout(Scene *scene, const View *view) {
    float width = scene->content.w - 48;
    unsigned columns = scene_columns(scene->content.w);
    unsigned quick_columns = width >= 800 ? 4 : 2;
    unsigned quick_count = view->recent_count ? view->recent_count : 4;
    unsigned quick_rows = (quick_count + quick_columns - 1) / quick_columns;
    float card = (width - (columns - 1) * 16) / columns;
    float album_height = view->library[1].count ? card + 72 : 44;
    float playlist_height = view->library[2].count ? card + 72 : 44;
    float albums_y = 68 + 68 + quick_rows * 62 + 22;
    float playlists_y = albums_y + 38 + album_height + 24;
    scene->home_extent = playlists_y + 38 + playlist_height + 20;
    scroll_layout(&scene->main_scroll, scene->home_extent, scene->content.h);
    scene->home_scroll = scene->main_scroll.offset;

    float x = scene->content.x + 24;
    float y = scene->content.y - scene->home_scroll;
    scene->hits[HIT_ALL] = (Hit){{x, scene->content.y + 14, 44, 30}, true};
    scene->hits[HIT_MUSIC] = (Hit){{x + 52, scene->content.y + 14, 72, 30}, true};

    if (view->recent_permission_required) {
        place(scene, HIT_HOME_HISTORY, (Rect){x + width - 200, y + 72, 200, 30});
    }

    float quick_width = (width - (quick_columns - 1) * 8) / quick_columns;

    for (unsigned i = 0; i < quick_count; ++i) {
        place(scene, (HitId)(HIT_HOME_QUICK_FIRST + i),
              (Rect){x + (i % quick_columns) * (quick_width + 8), y + 136 + (i / quick_columns) * 62,
                     quick_width, 54});
    }

    place(scene, HIT_HOME_ALBUMS, (Rect){x + width - 80, y + albums_y, 80, 28});
    place(scene, HIT_HOME_PLAYLISTS, (Rect){x + width - 80, y + playlists_y, 80, 28});

    for (unsigned i = 0; i < columns; ++i) {
        if (i < view->library[1].count) {
            place(scene, (HitId)(HIT_CARD_FIRST + i),
                  (Rect){x + i * (card + 16), y + albums_y + 38, card, card + 72});
        }

        if (i < view->library[2].count) {
            place(scene, (HitId)(HIT_CARD_FIRST + 5 + i),
                  (Rect){x + i * (card + 16), y + playlists_y + 38, card, card + 72});
        }
    }
}

static void cover(Canvas *canvas, const CatalogItem *item, Rect rect, Icon fallback) {
    canvas->clip(canvas->context, rect, 4);

    if (!item || !canvas->image(canvas->context, item->artwork, rect)) {
        canvas->box(canvas->context, rect, 4, 0x264b3b);
        canvas_icon(canvas, fallback,
                    (Rect){rect.x + rect.w * .3f, rect.y + rect.h * .3f, rect.w * .4f, rect.h * .4f},
                    0xffffff);
    }

    canvas->unclip(canvas->context);
}

void home_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    Rect bounds = scene->content;
    float x = bounds.x + 24;
    float width = bounds.w - 48;
    float y = bounds.y - scene->home_scroll;
    unsigned quick_columns = width >= 800 ? 4 : 2;
    unsigned quick_count = view->recent_count ? view->recent_count : 4;
    unsigned rows = (quick_count + quick_columns - 1) / quick_columns;
    unsigned columns = scene_columns(scene->content.w);
    float card = (width - (columns - 1) * 16) / columns;
    float albums_y = y + 68 + 68 + rows * 62 + 22;
    float playlists_y = albums_y + 38 + (view->library[1].count ? card + 72 : 44) + 24;
    canvas->clip(canvas->context, (Rect){bounds.x, bounds.y + 56, bounds.w, bounds.h - 56}, 0);
    canvas->gradient(canvas->context, (Rect){bounds.x, y + 56, bounds.w, 176}, 0, 0x234637, 0x121212);
    label(canvas, view->recent_count ? "Jump back in" : "Find your next listen",
          (Rect){x, y + 68, width - 210, 34}, 24, true, 0xffffff);
    label(canvas,
          view->recent_count ? "Albums from your recent listening"
                             : "Search for music, or pick a collection to explore.",
          (Rect){x, y + 104, width, 22}, 13, false, 0xb3b3b3);

    if (scene->hits[HIT_HOME_HISTORY].enabled) {
        Rect rect = scene->hits[HIT_HOME_HISTORY].rect;
        canvas->box(canvas->context, rect, 15, 0x282828);
        canvas->text(canvas->context, "Enable recently played", rect, 12, TEXT_CENTER | TEXT_VCENTER,
                     0xffffff);
    }

    static const char *shortcuts[] = {"Liked Songs", "Albums", "Playlists", "Search"};
    static const Icon icons[] = {ICON_HEART, ICON_ALBUM, ICON_PLAYLIST, ICON_SEARCH};

    for (unsigned i = 0; i < quick_count; ++i) {
        HitId id = (HitId)(HIT_HOME_QUICK_FIRST + i);
        Rect rect = scene->hits[id].rect;
        const CatalogItem *item = view->recent_count ? &view->recent[i] : NULL;
        canvas->box(canvas->context, rect, 5, color_mix(0x263b31, 0x385246, scene->hover[id]));
        cover(canvas, item, (Rect){rect.x, rect.y, 54, 54}, item ? ICON_ALBUM : icons[i]);
        canvas->text(canvas->context, item ? item->title : shortcuts[i],
                     (Rect){rect.x + 66, rect.y + 6, rect.w - 76, 42}, 13, TEXT_BOLD | TEXT_VCENTER,
                     0xffffff);
    }

    label(canvas, "Your saved albums", (Rect){x, albums_y, width - 96, 30}, 20, true, 0xffffff);
    label(canvas, "Your playlists", (Rect){x, playlists_y, width - 96, 30}, 20, true, 0xffffff);
    canvas->text(canvas->context, "Show all", scene->hits[HIT_HOME_ALBUMS].rect, 12,
                 TEXT_CENTER | TEXT_VCENTER, 0xb3b3b3);
    canvas->text(canvas->context, "Show all", scene->hits[HIT_HOME_PLAYLISTS].rect, 12,
                 TEXT_CENTER | TEXT_VCENTER, 0xb3b3b3);

    if (!view->library[1].count) {
        label(canvas, "Saved albums will appear here.", (Rect){x, albums_y + 38, width, 24}, 13, false,
              0xb3b3b3);
    }

    if (!view->library[2].count) {
        label(canvas, "Your playlists will appear here.", (Rect){x, playlists_y + 38, width, 24}, 13, false,
              0xb3b3b3);
    }

    for (unsigned i = 0; i < 10; ++i) {
        HitId id = (HitId)(HIT_CARD_FIRST + i);
        Rect rect = scene->hits[id].rect;
        const CatalogItem *item = scene_home_item(view, i);

        if (!item || rect.w <= 0 || rect.y >= bounds.y + bounds.h || rect.y + rect.h <= bounds.y + 56) {
            continue;
        }

        canvas->box(canvas->context, rect, 6, color_mix(0x181818, 0x282828, scene->hover[id]));
        cover(canvas, item, (Rect){rect.x + 10, rect.y + 10, rect.w - 20, rect.w - 20},
              i < 5 ? ICON_ALBUM : ICON_PLAYLIST);
        label(canvas, item->title, (Rect){rect.x + 10, rect.y + rect.w + 2, rect.w - 20, 22}, 14, true,
              0xffffff);
        label(canvas, item->detail, (Rect){rect.x + 10, rect.y + rect.w + 26, rect.w - 20, 20}, 12, false,
              0xb3b3b3);
    }

    canvas->unclip(canvas->context);
    canvas->box(canvas->context, (Rect){bounds.x, bounds.y, bounds.w, 56}, 0, 0x121212);
    HitId filters[] = {HIT_ALL, HIT_MUSIC};
    static const char *names[] = {"All", "Music"};

    for (unsigned i = 0; i < 2; ++i) {
        Rect rect = scene->hits[filters[i]].rect;
        canvas->box(canvas->context, rect, 16, i ? 0x282828 : 0xffffff);
        canvas->text(canvas->context, names[i], rect, 13, TEXT_CENTER | TEXT_VCENTER, i ? 0xffffff : 0);
    }
}
