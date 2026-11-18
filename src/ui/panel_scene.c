#include "panel_scene.h"

#include <stdio.h>

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

void panel_scene_toggle(Scene *scene, SidePanel panel) {
    scene->panel = scene->panel == panel ? PANEL_NONE : panel;
    scene->panel_first = 0;
    scene->panel_scroll = (Scroll){0};
    app_queue_visible(scene->panel == PANEL_QUEUE);

    if (scene->panel == PANEL_QUEUE) {
        app_action(ACTION_QUEUE_PANEL);
    }
}

void panel_scene_layout(Scene *scene, const View *view) {
    if (scene->panel == PANEL_NONE) {
        return;
    }

    Rect rect = scene->side_panel;
    scene->hits[HIT_PANEL_CLOSE] = (Hit){{rect.x + rect.w - 44, rect.y + 12, 28, 28}, true};

    if (scene->panel == PANEL_QUEUE) {
        float list_h = rect.h > 224 ? rect.h - 224 : 56;
        scroll_layout(&scene->panel_scroll, view->queue.count * 56.0f, list_h);
        scene->panel_first = (unsigned)(scene->panel_scroll.offset / 56.0f);

        if (scene->panel_first >= view->queue.count) {
            scene->panel_first = 0;
        }

        Rect thumb =
            scroll_thumb(&scene->panel_scroll, (Rect){rect.x + rect.w - 10, rect.y + 164, 6, list_h});
        scene->hits[HIT_SCROLL_PANEL] = (Hit){thumb, thumb.h > 0};
        scene->hits[HIT_PANEL_QUEUE] = (Hit){{rect.x + 16, rect.y + rect.h - 52, rect.w - 32, 36}, true};
        return;
    }

    scene->panel_scroll = (Scroll){0};

    if (scene->panel == PANEL_NOW_PLAYING) {
        float side = rect.h - 216 < rect.w - 32 ? rect.h - 216 : rect.w - 32;
        scene->hits[HIT_PANEL_ARTIST] =
            (Hit){{rect.x + 16, rect.y + side + 112, rect.w - 32, 28}, view->playback.artist[0] != 0};
        scene->hits[HIT_PANEL_PLAY] =
            (Hit){{rect.x + 16, rect.y + side + 156, rect.w - 32, 36}, view->playback.has_device};
    }
}

void panel_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    if (scene->panel == PANEL_NONE) {
        return;
    }

    static const char *titles[] = {"", "Now Playing", "Queue", "Friend Activity", "Lyrics"};
    Rect rect = scene->side_panel;
    canvas->box(canvas->context, (Rect){rect.x - 4, rect.y, rect.w + 4, rect.h}, 8, 0x080808);
    canvas->box(canvas->context, rect, 8, 0x121212);
    canvas->clip(canvas->context, rect, 8);
    label(canvas, titles[scene->panel], (Rect){rect.x + 16, rect.y + 14, rect.w - 68, 30}, 18, true,
          0xffffff);
    canvas_icon(canvas, ICON_CLOSE, scene->hits[HIT_PANEL_CLOSE].rect, 0xb3b3b3);

    if (scene->panel == PANEL_NOW_PLAYING) {
        float side = rect.h - 216 < rect.w - 32 ? rect.h - 216 : rect.w - 32;
        Rect image = {rect.x + 16, rect.y + 64, side, side};
        canvas->clip(canvas->context, image, 6);

        if (!canvas->image(canvas->context, view->playback.artwork, image)) {
            canvas->box(canvas->context, image, 6, 0x263b31);
            canvas_icon(canvas, ICON_ALBUM,
                        (Rect){image.x + side * .3f, image.y + side * .3f, side * .4f, side * .4f}, 0xb3b3b3);
        }

        canvas->unclip(canvas->context);
        label(canvas, view->playback.title[0] ? view->playback.title : "Nothing playing yet",
              (Rect){rect.x + 16, rect.y + side + 80, rect.w - 32, 32}, 22, true, 0xffffff);
        label(canvas, view->playback.artist, scene->hits[HIT_PANEL_ARTIST].rect, 14, false, 0xb3b3b3);
        Rect play = scene->hits[HIT_PANEL_PLAY].rect;
        canvas->box(canvas->context, play, 18, 0x1ed760);
        canvas->text(canvas->context, view->playback.playing ? "Pause" : "Play", play, 14,
                     TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER, 0);
        label(canvas, view->playback.device, (Rect){rect.x + 16, rect.y + rect.h - 36, rect.w - 32, 22}, 12,
              false, 0xb3b3b3);
    }

    if (scene->panel == PANEL_QUEUE) {
        label(canvas, "Now playing", (Rect){rect.x + 16, rect.y + 60, rect.w - 32, 22}, 12, true, 0xb3b3b3);
        label(canvas, view->playback.title, (Rect){rect.x + 16, rect.y + 88, rect.w - 32, 26}, 15, true,
              0x1ed760);
        label(canvas, "Next up", (Rect){rect.x + 16, rect.y + 128, rect.w - 32, 24}, 16, true, 0xffffff);
        float list_h = rect.h > 224 ? rect.h - 224 : 0;
        float sub = scene->panel_scroll.offset - scene->panel_first * 56.0f;
        canvas->clip(canvas->context, (Rect){rect.x + 12, rect.y + 160, rect.w - 24, list_h}, 0);

        for (unsigned i = 0; i < 16 && scene->panel_first + i < view->queue.count; ++i) {
            const CatalogItem *item = &view->queue.items[scene->panel_first + i];
            Rect cover = {rect.x + 16, rect.y + 164 + i * 56 - sub, 40, 40};

            if (cover.y + 48 <= rect.y + 160 || cover.y >= rect.y + 160 + list_h) {
                continue;
            }

            if (!canvas->image(canvas->context, item->artwork, cover)) {
                canvas->box(canvas->context, cover, 4, 0x282828);
            }

            label(canvas, item->title, (Rect){rect.x + 68, cover.y, rect.w - 84, 22}, 13, true, 0xffffff);
            label(canvas, item->detail, (Rect){rect.x + 68, cover.y + 24, rect.w - 84, 20}, 11, false,
                  0xb3b3b3);
        }

        canvas->unclip(canvas->context);

        if (!view->queue.count) {
            label(canvas, "No upcoming tracks returned by Spotify.",
                  (Rect){rect.x + 16, rect.y + 168, rect.w - 32, 64}, 13, false, 0xb3b3b3);
        }

        Rect button = scene->hits[HIT_PANEL_QUEUE].rect;
        canvas->box(canvas->context, button, 18, 0x282828);
        canvas->text(canvas->context, "Open full queue", button, 13, TEXT_CENTER | TEXT_VCENTER, 0xffffff);
    }

    if (scene->panel == PANEL_FRIENDS || scene->panel == PANEL_LYRICS) {
        bool lyrics = scene->panel == PANEL_LYRICS;
        label(canvas,
              lyrics && view->playback.title[0] ? view->playback.title
                                                : "Not available through the public API",
              (Rect){rect.x + 16, rect.y + 76, rect.w - 32, 64}, 18, true, 0xffffff);
        label(canvas,
              lyrics ? "Spotify does not expose lyrics to this client. Live lyrics require an authorized, "
                       "licensed lyrics provider."
                     : "Spotify does not expose friends' listening activity to third-party clients. This "
                       "panel cannot show a live friends feed.",
              (Rect){rect.x + 16, rect.y + 160, rect.w - 32, 150}, 14, false, 0xb3b3b3);
        label(canvas, "No private endpoints, scraped lyrics or fabricated activity.",
              (Rect){rect.x + 16, rect.y + 330, rect.w - 32, 64}, 12, false, 0xb3b3b3);
    }

    canvas->unclip(canvas->context);
}
