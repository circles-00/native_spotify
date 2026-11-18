#include "scene.h"
#include "collection_scene.h"
#include "home_scene.h"
#include "panel_scene.h"
#include "search_scene.h"

#include <stdio.h>
#include <string.h>

#define BLACK 0x000000u
#define PANEL 0x121212u
#define WHITE 0xffffffu
#define MUTED 0xb3b3b3u
#define GREEN 0x1ed760u

static const Icon collection_icons[] = {ICON_HEART, ICON_ALBUM, ICON_PLAYLIST, ICON_QUEUE};
static const Color collection_colors[] = {0x5b35d5, 0x157b68, 0x264cad, 0xb94b2c};

static void text(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size,
                 (bold ? TEXT_BOLD : 0) | (rect.h <= size * 2 ? TEXT_VCENTER : 0), color);
}

static void line(Canvas *canvas, float x, float y, float width, Color color) {
    canvas->box(canvas->context, (Rect){x, y, width, 1}, 0, color);
}

static void pill(Scene *scene, Canvas *canvas, HitId id, const char *label, bool selected) {
    Rect rect = scene->hits[id].rect;
    Color background = selected ? WHITE : color_mix(0x242424, 0x383838, scene->hover[id]);
    canvas->box(canvas->context, rect, rect.h / 2, background);
    canvas->text(canvas->context, label, rect, 13, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                 selected ? BLACK : WHITE);
}

static void button(Scene *scene, Canvas *canvas, HitId id, const char *label, Icon icon, bool filled) {
    Hit hit = scene->hits[id];
    if (hit.rect.w <= 0) {
        return;
    }

    float hover = scene->hover[id];
    Color foreground = hit.enabled ? WHITE : 0x666666;
    bool active = (id == HIT_QUEUE && scene->panel == PANEL_QUEUE) ||
                  (id == HIT_NOW_PLAYING && scene->panel == PANEL_NOW_PLAYING) ||
                  (id == HIT_FRIENDS && scene->panel == PANEL_FRIENDS) ||
                  (id == HIT_LYRICS && scene->panel == PANEL_LYRICS);

    if (active) {
        foreground = GREEN;
    }

    if (filled) {
        canvas->box(canvas->context, hit.rect, hit.rect.h / 2, color_mix(WHITE, 0xe2e2e2, hover));
        foreground = BLACK;
    }

    if (!filled && hover > .01f) {
        canvas->box(canvas->context, hit.rect, hit.rect.h / 2, color_mix(PANEL, 0x333333, hover));
    }

    if (label) {
        canvas->text(canvas->context, label, hit.rect, 14, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                     foreground);
    }

    if (!label) {
        canvas_icon(canvas, icon,
                    (Rect){hit.rect.x + (hit.rect.w - 20) / 2, hit.rect.y + (hit.rect.h - 20) / 2, 20, 20},
                    foreground);
    }
}

static void artwork(Canvas *canvas, Rect rect, unsigned style, bool large) {
    Color top = collection_colors[style % 4];
    Color bottom = style % 4 == 0 ? 0xb1d6c1 : color_mix(top, 0x101820, .65f);
    canvas->gradient(canvas->context, rect, 5, top, bottom);

    float size = rect.w * (large ? .38f : .44f);
    canvas_icon(canvas, collection_icons[style % 4],
                (Rect){rect.x + (rect.w - size) / 2, rect.y + (rect.h - size) / 2, size, size}, WHITE);
}

static void input_text(Scene *scene, Canvas *canvas, Rect rect, const char *value, const char *placeholder,
                       bool focused) {
    if (scene->select_all && focused) {
        canvas->box(canvas->context, rect, 2, 0x305374);
    }

    canvas->text(canvas->context, value[0] ? value : placeholder, rect, 15, TEXT_INPUT | TEXT_VCENTER,
                 value[0] ? WHITE : MUTED);

    if (focused && !scene->select_all && canvas->measure) {
        float width = canvas->measure(canvas->context, value, 15);
        if (width > rect.w - 2) {
            width = rect.w - 2;
        }
        canvas->box(canvas->context, (Rect){rect.x + width, rect.y + (rect.h - 18) / 2, 1, 18}, 0, WHITE);
    }
}

static void header(Scene *scene, const View *view, Canvas *canvas) {
    canvas_icon(canvas, ICON_LIBRARY, (Rect){24, 19, 28, 28}, WHITE);
    canvas->box(canvas->context, scene->hits[HIT_HOME].rect, 24, 0x1f1f1f);
    button(scene, canvas, HIT_HOME, NULL, ICON_HOME, false);

    canvas->box(canvas->context, scene->search, 24, 0x1f1f1f);
    if (scene->focus == HIT_SEARCH) {
        Rect outline = scene->search;
        canvas->box(canvas->context, outline, 24, 0x555555);
        canvas->box(canvas->context, (Rect){outline.x + 1, outline.y + 1, outline.w - 2, outline.h - 2}, 23,
                    0x1f1f1f);
    }

    canvas_icon(canvas, ICON_SEARCH, (Rect){scene->search.x + 16, 20, 24, 24}, MUTED);

    if (!canvas->native_input) {
        Rect input = {scene->search.x + 50, 20, scene->search.w - 68, 24};
        input_text(scene, canvas, input, scene->query, "What do you want to play?",
                   scene->focus == HIT_SEARCH);
    }

    Rect avatar = scene->hits[HIT_PROFILE].rect;
    canvas->box(canvas->context, avatar, avatar.w / 2,
                color_mix(0x1f1f1f, 0x333333, scene->hover[HIT_PROFILE]));
    Rect inner = {avatar.x + 3, avatar.y + 3, avatar.w - 6, avatar.h - 6};
    canvas->clip(canvas->context, inner, inner.w / 2);

    if (!view->profile.artwork[0] || !canvas->image(canvas->context, view->profile.artwork, inner)) {
        canvas->box(canvas->context, inner, inner.w / 2,
                    color_mix(0x282828, 0x3a3a3a, scene->hover[HIT_PROFILE]));
        canvas_icon(canvas, ICON_USER, (Rect){inner.x + 6, inner.y + 6, inner.w - 12, inner.h - 12}, WHITE);
    }

    canvas->unclip(canvas->context);
}

static void sidebar(Scene *scene, const View *view, Canvas *canvas) {
    Rect bounds = scene->sidebar;
    canvas->box(canvas->context, bounds, 8, PANEL);
    canvas_icon(canvas, ICON_LIBRARY, (Rect){20, 79, 20, 20}, MUTED);
    text(canvas, "Your Library", (Rect){48, 78, bounds.w - 92, 22}, 15, true, WHITE);
    button(scene, canvas, HIT_REFRESH, NULL, ICON_REFRESH, false);

    pill(scene, canvas, HIT_PLAYLISTS, "Playlists", !scene->home && scene->section == BROWSE_PLAYLISTS);
    pill(scene, canvas, HIT_ALBUMS, "Albums", !scene->home && scene->section == BROWSE_SAVED_ALBUMS);
    text(canvas, "Your collection", (Rect){20, 156, bounds.w - 36, 18}, 12, false, MUTED);

    Rect liked = scene->hits[HIT_TRACKS].rect;
    canvas->box(canvas->context, liked, 6, color_mix(PANEL, 0x282828, scene->hover[HIT_TRACKS]));
    artwork(canvas, (Rect){liked.x + 6, liked.y + 6, 44, 44}, 0, false);
    text(canvas, "Liked Songs", (Rect){liked.x + 58, liked.y + 7, liked.w - 66, 20}, 14, true, WHITE);
    text(canvas, "Playlist · Saved tracks", (Rect){liked.x + 58, liked.y + 29, liked.w - 66, 18}, 12, false,
         MUTED);
    line(canvas, 20, 246, bounds.w - 24, 0x252525);

    if (view->playlists.count) {
        canvas->clip(canvas->context, (Rect){bounds.x, 250, bounds.w, bounds.y + bounds.h - 262}, 0);

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
            canvas->box(canvas->context, row, 6, color_mix(PANEL, 0x282828, scene->hover[id]));

            if (!canvas->image(canvas->context, item->artwork, (Rect){row.x + 6, row.y + 4, 40, 40})) {
                artwork(canvas, (Rect){row.x + 6, row.y + 4, 40, 40}, 2, false);
            }

            text(canvas, item->title, (Rect){row.x + 56, row.y + 4, row.w - 64, 22}, 13, true, WHITE);
            text(canvas, item->detail, (Rect){row.x + 56, row.y + 25, row.w - 64, 18}, 12, false, MUTED);
        }

        canvas->unclip(canvas->context);
        return;
    }

    text(canvas, "Find your favorites", (Rect){20, 270, bounds.w - 40, 22}, 15, true, WHITE);
    text(canvas,
         view->authenticated ? "Browse your saved music using the filters above."
                             : "Log in to see the music you've saved.",
         (Rect){20, 298, bounds.w - 40, 56}, 13, false, MUTED);
}

static void card(Scene *scene, Canvas *canvas, HitId id, const char *title, const char *subtitle,
                 unsigned style, const char *url, bool circle) {
    Rect rect = scene->hits[id].rect;
    if (rect.w <= 0 || rect.y + rect.h <= scene->content.y || rect.y >= scene->content.y + scene->content.h) {
        return;
    }

    float hover = scene->hover[id];
    canvas->box(canvas->context, rect, 8, color_mix(PANEL, 0x282828, hover));

    Rect cover = {rect.x + 12, rect.y + 12, rect.w - 24, rect.w - 24};
    canvas->clip(canvas->context, cover, circle ? cover.w / 2 : 5);

    if (!url || !canvas->image(canvas->context, url, cover)) {
        artwork(canvas, cover, style, true);
    }

    canvas->unclip(canvas->context);
    text(canvas, title, (Rect){rect.x + 12, cover.y + cover.h + 14, rect.w - 24, 24}, 15, true, WHITE);
    text(canvas, subtitle, (Rect){rect.x + 12, cover.y + cover.h + 42, rect.w - 24, 38}, 12, false, MUTED);
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
         scene->section == BROWSE_ARTIST_ALBUMS) &&
        scene->collection[0]) {
        title = scene->collection;
    }

    if (grid) {
        text(canvas, title, (Rect){x, y + 36, bounds.w - 48, 48}, 32, true, WHITE);
    }

    if (!grid) {
        canvas->gradient(canvas->context, (Rect){bounds.x, y, bounds.w, 190}, 0,
                         color_mix(collection_colors[style], PANEL, .4f), PANEL);
        Rect cover = {x, y + 24, 136, 136};

        if (!scene->collection_artwork[0] ||
            (scene->section != BROWSE_ALBUM_TRACKS && scene->section != BROWSE_PLAYLIST_TRACKS) ||
            !canvas->image(canvas->context, scene->collection_artwork, cover)) {
            artwork(canvas, cover, style, true);
        }
        text(canvas, scene->section == BROWSE_SEARCH ? "SEARCH" : "YOUR COLLECTION",
             (Rect){x + 156, y + 35, bounds.w - 210, 20}, 11, true, WHITE);
        text(canvas, title, (Rect){x + 156, y + 68, bounds.w - 210, 58}, 36, true, WHITE);

        char summary[384];
        unsigned total = loaded ? view->list.total : 0;
        snprintf(summary, sizeof(summary), "%u %s", total,
                 scene->section == BROWSE_QUEUE ? "up next"
                 : total == 1                   ? "song"
                                                : "songs");
        if (scene->section == BROWSE_SEARCH && loaded) {
            snprintf(summary, sizeof(summary), "Results for %s", view->list.request.query);
        }

        text(canvas, summary, (Rect){x + 156, y + 137, bounds.w - 210, 22}, 13, false, MUTED);
        Rect play = scene->hits[HIT_OPEN].rect;
        canvas->box(canvas->context, play, 28, color_mix(GREEN, 0x3be477, scene->hover[HIT_OPEN]));
        canvas_icon(canvas, ICON_PLAY, (Rect){play.x + 14, play.y + 14, 28, 28}, BLACK);
        button(scene, canvas, HIT_LINK, NULL, ICON_LINK, false);

        text(canvas, "#", (Rect){x + 8, y + 258, 24, 20}, 13, false, MUTED);
        text(canvas, "Title", (Rect){x + 48, y + 258, 200, 20}, 13, false, MUTED);
        text(canvas, "Artist / show", (Rect){x + bounds.w * .58f, y + 258, 160, 20}, 13, false, MUTED);
        line(canvas, x, y + 279, bounds.w - 48, 0x303030);
    }

    unsigned count = loaded ? view->list.count : 0;

    for (unsigned slot = 0; slot < ROW_SLOTS; ++slot) {
        HitId id = (HitId)(HIT_ROW_FIRST + slot);
        Rect row = scene->hits[id].rect;
        unsigned index = scene->first_row + slot;
        const CatalogItem *item = list_row(&view->list, index);

        if (row.w <= 0 || !item || row.y + row.h <= bounds.y || row.y >= bounds.y + bounds.h) {
            continue;
        }

        if (grid) {
            card(scene, canvas, id, item->title, item->kind == ITEM_ARTIST ? "Artist" : item->detail,
                 index + style, item->artwork, item->kind == ITEM_ARTIST);
            continue;
        }

        bool selected = (int)index == view->list.selected;
        canvas->box(canvas->context, row, 5,
                    color_mix(selected ? 0x303030 : PANEL, 0x2a2a2a, scene->hover[id]));

        char number[16];
        snprintf(number, sizeof(number), "%u", index + 1);
        text(canvas, number, (Rect){row.x + 16, row.y + 17, 28, 22}, 14, false, MUTED);
        Rect cover = {row.x + 48, row.y + 8, 40, 40};

        if (!canvas->image(canvas->context, item->artwork[0] ? item->artwork : scene->collection_artwork,
                           cover)) {
            artwork(canvas, cover, index + style, false);
        }
        text(canvas, item->title, (Rect){row.x + 100, row.y + 6, bounds.w * .48f - 110, 24}, 14, true, WHITE);
        text(canvas, item->playable ? "Spotify" : "Unavailable",
             (Rect){row.x + 100, row.y + 30, bounds.w * .48f - 110, 20}, 12, false, MUTED);
        text(canvas, item->detail, (Rect){x + bounds.w * .58f, row.y + 16, bounds.w * .35f - 48, 24}, 13,
             false, MUTED);
    }

    if (!count) {
        float y = bounds.y + (grid ? 150 : 318);
        text(canvas, view->list.loading ? "Loading your music…" : "Nothing here yet.",
             (Rect){x + 12, y, bounds.w - 72, 32}, 20, true, WHITE);
        text(canvas, view->list.loading ? "" : "Choose a collection or search above.",
             (Rect){x + 12, y + 42, bounds.w - 72, 32}, 13, false, MUTED);
    }

    if (count && view->list.loading && view->list.more) {
        text(canvas, "Loading more…", (Rect){x, bounds.y + bounds.h - 36, bounds.w - 48, 20}, 12, false,
             MUTED);
    }
}

static void player(Scene *scene, const View *view, Canvas *canvas) {
    float y = scene->height - 88;
    float title_width = scene->width < 1100 ? 148 : 180;
    canvas->box(canvas->context, scene->player, 0, BLACK);
    if (!canvas->image(canvas->context, view->playback.artwork, (Rect){16, y + 12, 56, 56})) {
        artwork(canvas, (Rect){16, y + 12, 56, 56}, 1, false);
    }
    text(canvas, view->playback.title[0] ? view->playback.title : "Nothing playing yet",
         (Rect){88, y + 18, title_width, 24}, 13, true, WHITE);
    text(canvas, view->playback.title[0] ? view->playback.artist : "Choose a track from your library",
         (Rect){88, y + 40, title_width, 20}, 11, false, MUTED);

    button(scene, canvas, HIT_PREVIOUS, NULL, ICON_PREVIOUS, false);
    button(scene, canvas, HIT_PLAY, NULL, view->playback.playing ? ICON_PAUSE : ICON_PLAY, true);
    button(scene, canvas, HIT_NEXT, NULL, ICON_NEXT, false);
    button(scene, canvas, HIT_DEVICES, NULL, ICON_DEVICE, false);
    button(scene, canvas, HIT_QUEUE, NULL, ICON_QUEUE, false);
    button(scene, canvas, HIT_NOW_PLAYING, NULL, ICON_ALBUM, false);
    button(scene, canvas, HIT_LYRICS, NULL, ICON_LYRICS, false);
    canvas_icon(canvas, ICON_SHUFFLE, scene->hits[HIT_SHUFFLE].rect, view->playback.shuffle ? GREEN : MUTED);
    canvas_icon(canvas, ICON_REPEAT, scene->hits[HIT_REPEAT].rect,
                view->playback.repeat != REPEAT_OFF ? GREEN : MUTED);

    if (view->playback.repeat == REPEAT_TRACK) {
        Rect rect = scene->hits[HIT_REPEAT].rect;
        canvas->text(canvas->context, "1", (Rect){rect.x + 7, rect.y + 5, 10, 14}, 9,
                     TEXT_CENTER | TEXT_VCENTER, GREEN);
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
    canvas->box(canvas->context, (Rect){seek.x, seek.y + 5, seek.w, 4}, 2, 0x4d4d4d);
    canvas->box(canvas->context, (Rect){seek.x, seek.y + 5, seek.w * fraction / 1000, 4}, 2,
                scene->hot == HIT_SEEK ? GREEN : WHITE);

    char position[24];
    unsigned milliseconds = scene->dragging == HIT_SEEK
                                ? (unsigned)((uint64_t)view->playback.duration_ms * scene->draft.seek / 1000)
                                : view->playback.position_ms;
    snprintf(position, sizeof(position), "%u:%02u", milliseconds / 60000, milliseconds / 1000 % 60);
    text(canvas, position, (Rect){seek.x - 42, seek.y - 2, 38, 18}, 10, false, MUTED);
    snprintf(position, sizeof(position), "%u:%02u", view->playback.duration_ms / 60000,
             view->playback.duration_ms / 1000 % 60);
    text(canvas, position, (Rect){seek.x + seek.w + 8, seek.y - 2, 40, 18}, 10, false, MUTED);

    Rect volume = scene->hits[HIT_VOLUME].rect;
    canvas_icon(canvas, ICON_VOLUME, (Rect){volume.x - 28, volume.y - 2, 20, 20}, MUTED);
    canvas->box(canvas->context, (Rect){volume.x, volume.y + 6, volume.w, 4}, 2, 0x4d4d4d);
    canvas->box(canvas->context, (Rect){volume.x, volume.y + 6, volume.w * scene->draft.volume / 100, 4}, 2,
                WHITE);
}

static void devices(Scene *scene, const View *view, Canvas *canvas) {
    if (!scene->device_popup) {
        return;
    }

    Rect bounds = scene->device_panel;
    canvas->box(canvas->context, (Rect){bounds.x - 4, bounds.y - 4, bounds.w + 8, bounds.h + 8}, 12,
                0x080808);
    canvas->box(canvas->context, bounds, 10, 0x282828);
    text(canvas, "Where to play", (Rect){bounds.x + 20, bounds.y + 8, 245, 28}, 18, true, WHITE);
    button(scene, canvas, HIT_CLOSE_DEVICES, NULL, ICON_CLOSE, false);

    for (unsigned i = 0; i < view->devices.count; ++i) {
        HitId id = (HitId)(HIT_DEVICE_FIRST + i);
        Rect row = scene->hits[id].rect;
        if (row.h <= 0) {
            continue;
        }

        bool selected = (int)i == view->devices.selected;
        canvas->box(canvas->context, row, 5,
                    selected ? 0x404040 : color_mix(0x282828, 0x383838, scene->hover[id]));
        canvas_icon(canvas, ICON_DEVICE, (Rect){row.x + 8, row.y + 10, 20, 20}, selected ? GREEN : WHITE);
        bool local = view->engine_device_id[0] && !strcmp(view->devices.items[i].id, view->engine_device_id);
        text(canvas, local ? "This computer" : view->devices.items[i].name,
             (Rect){row.x + 42, row.y + 8, row.w - 52, 24}, 13, false, WHITE);
    }

    if (!view->devices.count) {
        text(canvas, view->engine == ENGINE_READY ? "Looking for your devices…" : "No other devices found.",
             (Rect){bounds.x + 20, bounds.y + 58, 280, 48}, 13, false, MUTED);
    }

    const Device *selected = devices_selected(&view->devices);
    bool local = selected && view->engine_device_id[0] && !strcmp(selected->id, view->engine_device_id);
    button(scene, canvas, HIT_TRANSFER,
           view->engine != ENGINE_READY ? "Set up this computer"
           : local                      ? "Play on this computer"
                                        : "Transfer playback",
           ICON_DEVICE, true);
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
    canvas->box(canvas->context, panel, 14, 0x282828);
    canvas_icon(canvas, ICON_DEVICE, (Rect){panel.x + 32, panel.y + 30, 28, 28}, GREEN);
    text(canvas, "Play on this computer", (Rect){panel.x + 72, panel.y + 28, panel.w - 130, 32}, 20, true,
         WHITE);
    button(scene, canvas, HIT_CLOSE_SETUP, NULL, ICON_CLOSE, false);
    text(canvas, message, (Rect){panel.x + 32, panel.y + 84, panel.w - 64, 96}, 14, false, MUTED);

    // A rejected paste keeps the instructions visible and explains the problem underneath.
    if (instructions && view->engine_message[0]) {
        text(canvas, view->engine_message, (Rect){panel.x + 32, panel.y + 188, panel.w - 64, 48}, 13, false,
             0xf3727f);
    }
    Rect key = scene->hits[HIT_SETUP_KEY].rect;
    canvas->box(canvas->context, key, key.h / 2, color_mix(0x3a3a3a, 0x4d4d4d, scene->hover[HIT_SETUP_KEY]));
    canvas->text(canvas->context, "Get your key", key, 14, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER, WHITE);
    button(scene, canvas, HIT_SETUP_PASTE, "Paste key", ICON_LINK, true);

    if (scene->hits[HIT_SETUP_FORGET].enabled) {
        canvas->text(canvas->context, "Remove key", scene->hits[HIT_SETUP_FORGET].rect, 12,
                     TEXT_CENTER | TEXT_VCENTER, color_mix(MUTED, WHITE, scene->hover[HIT_SETUP_FORGET]));
    }
}

static void centered_text(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size,
                 TEXT_CENTER | (bold ? TEXT_BOLD : 0) | (rect.h <= size * 2 ? TEXT_VCENTER : 0), color);
}

static void login(Scene *scene, const View *view, Canvas *canvas) {
    Rect panel = scene->login_panel;
    canvas->gradient(canvas->context, panel, 16, 0x1c1c1c, PANEL);
    canvas->clip(canvas->context, panel, 16);
    canvas_icon(canvas, ICON_ALBUM, (Rect){panel.x + panel.w / 2 - 18, panel.y + 36, 36, 36}, GREEN);
    centered_text(canvas, "Native Spotify", (Rect){panel.x + 40, panel.y + 84, panel.w - 80, 30}, 21, true,
                  WHITE);
    centered_text(canvas, "Log in to Spotify", (Rect){panel.x + 32, panel.y + 136, panel.w - 64, 48}, 32,
                  true, WHITE);
    centered_text(canvas, "Your music and playlists. A lighter native player.",
                  (Rect){panel.x + 40, panel.y + 196, panel.w - 80, 44}, 14, false, MUTED);
    button(scene, canvas, HIT_ACCOUNT, view->connecting ? "Connecting…" : "Continue with Spotify", ICON_LINK,
           true);
    centered_text(canvas, "Secure sign-in opens in your browser.",
                  (Rect){panel.x + 40, panel.y + 330, panel.w - 80, 24}, 12, false, MUTED);
    centered_text(canvas, view->status, (Rect){panel.x + 40, panel.y + 370, panel.w - 80, 56}, 12, false,
                  MUTED);
    canvas->unclip(canvas->context);
}

void scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    artwork_begin_frame();
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

    if (scene->panel == PANEL_QUEUE) {
        for (unsigned i = 0; i < 16 && scene->panel_first + i < view->queue.count; ++i) {
            artwork_pin(view->queue.items[scene->panel_first + i].artwork);
        }
    }

    if (!scene->home && scene->section == BROWSE_SEARCH && scene->search_filter == SEARCH_ALL) {
        for (unsigned i = 0; i < view->search_artist_count; ++i) {
            artwork_pin(view->search_artists[i].artwork);
        }

        for (unsigned i = 0; i < view->search_album_count; ++i) {
            artwork_pin(view->search_albums[i].artwork);
        }
    }

    for (unsigned i = 0; i < 10; ++i) {
        const CatalogItem *item = scene->home ? scene_home_item(view, i) : NULL;

        if (item) {
            artwork_pin(item->artwork);
        }
    }

    if (view->profile.artwork[0]) {
        artwork_pin(view->profile.artwork);
    }

    if (scene->home) {
        for (unsigned i = 0; i < view->recent_count; ++i) {
            artwork_pin(view->recent[i].artwork);
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

    canvas->box(canvas->context, (Rect){0, 0, scene->width, scene->height}, 0, BLACK);

    if (scene->login_screen) {
        login(scene, view, canvas);
        return;
    }

    header(scene, view, canvas);
    sidebar(scene, view, canvas);
    canvas->box(canvas->context, scene->content, 8, PANEL);
    canvas->clip(canvas->context, scene->content, 8);

    if (scene->home) {
        home_scene_draw(scene, view, canvas);
    }
    if (!scene->home && scene->section == BROWSE_SEARCH) {
        search_scene_draw(scene, view, canvas);
    }

    if (!scene->home && collection_page(scene->section)) {
        collection_scene_draw(scene, view, canvas);
    }

    if (!scene->home && scene->section != BROWSE_SEARCH && !collection_page(scene->section)) {
        content(scene, view, canvas);
    }

    if (view->status[0]) {
        Rect notice = {scene->content.x + 24, scene->content.y + scene->content.h - 52,
                       scene->content.w - 150, 36};
        canvas->box(canvas->context, notice, 6, 0x282828);
        text(canvas, view->status, (Rect){notice.x + 12, notice.y + 8, notice.w - 24, 20}, 12, false, MUTED);
    }

    canvas->unclip(canvas->context);
    player(scene, view, canvas);
    panel_scene_draw(scene, view, canvas);
    scene_scrollbars(scene, canvas);
    devices(scene, view, canvas);
    setup(scene, view, canvas);

    if (scene->menu.open) {
        Rect r = scene->menu.rect;
        canvas->box(canvas->context, (Rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 10, 0x080808);
        canvas->box(canvas->context, r, 8, 0x282828);

        for (unsigned i = 0; i < scene->menu.count && i < 6; ++i) {
            HitId id = (HitId)(HIT_CTX_FIRST + i);
            Rect row = scene->hits[id].rect;
            canvas->box(canvas->context, row, 4, color_mix(0x282828, 0x3e3e3e, scene->hover[id]));
            text(canvas, scene->menu.labels[i], (Rect){row.x + 12, row.y + 4, row.w - 24, 24}, 13, false,
                 WHITE);
        }
    }
}
