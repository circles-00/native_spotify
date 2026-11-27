#include "panel_scene.h"
#include "dark_palette.h"
#include "lyrics_scene.h"

#include <stdio.h>
#include <string.h>

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

static void draw_thumb(Canvas *canvas, const char *url, Rect rect, float radius, Icon fallback) {
    if (canvas_artwork(canvas, url, rect, radius)) {
        return;
    }

    canvas->box(canvas->context, rect, radius, DARK_TILE);
    canvas_icon(canvas, fallback,
                (Rect){rect.x + rect.w * .25f, rect.y + rect.h * .25f, rect.w * .5f, rect.h * .5f},
                DARK_MUTED);
}

Icon panel_device_icon(const View *view, const Device *device) {
    if (view->engine_device_id[0] && !strcmp(device->id, view->engine_device_id)) {
        return ICON_DEVICE;
    }

    if (!strcmp(device->type, "Computer")) {
        return ICON_DEVICE;
    }

    if (!strcmp(device->type, "Smartphone") || !strcmp(device->type, "Tablet")) {
        return ICON_PHONE;
    }

    return ICON_SPEAKER;
}

/* Index of the device Spotify is playing on, or -1. */
static int current_device(const View *view) {
    for (unsigned i = 0; i < view->devices.count; ++i) {
        const Device *device = &view->devices.items[i];

        if (view->playback.has_device && device->id[0] && !strcmp(device->id, view->playback.device_id)) {
            return (int)i;
        }
    }

    for (unsigned i = 0; i < view->devices.count; ++i) {
        if (view->devices.items[i].active) {
            return (int)i;
        }
    }

    return -1;
}

/* Devices listed under "Select another device"; -1 stands for setting up this computer. */
static unsigned other_devices(const View *view, int *out) {
    int current = current_device(view);
    unsigned count = 0;

    if (view->engine != ENGINE_READY) {
        out[count++] = -1;
    }

    for (unsigned i = 0; i < view->devices.count; ++i) {
        if ((int)i != current) {
            out[count++] = (int)i;
        }
    }

    return count;
}

static Rect device_card(Rect panel) {
    return (Rect){panel.x + 12, panel.y + 52, panel.w - 24, 140};
}

static float device_list_top(Rect panel) {
    return panel.y + 236;
}

static void devices_layout(Scene *scene, const View *view, Rect rect) {
    Rect card = device_card(rect);
    float center = card.x + card.w / 2;
    bool connected = view->playback.has_device;

    scene->hits[HIT_PANEL_PREVIOUS] = (Hit){{center - 68, card.y + 92, 32, 32},
                                            connected && playback_can_command(&view->playback, "previous")};
    scene->hits[HIT_PANEL_PLAY] = (Hit){{center - 20, card.y + 88, 40, 40}, connected};
    scene->hits[HIT_PANEL_NEXT] =
        (Hit){{center + 36, card.y + 92, 32, 32}, connected && playback_can_command(&view->playback, "next")};

    int entries[DEVICE_LIMIT + 1];
    unsigned count = other_devices(view, entries);
    float top = device_list_top(rect);
    float bottom = rect.y + rect.h - 8;
    unsigned capacity = bottom > top ? (unsigned)((bottom - top) / 56) : 0;
    scene->device_visible = capacity;

    if (scene->device_first + capacity > count) {
        scene->device_first = count > capacity ? count - capacity : 0;
    }

    for (unsigned slot = 0; slot < capacity && scene->device_first + slot < count; ++slot) {
        int index = entries[scene->device_first + slot];
        Rect row = {rect.x + 8, top + slot * 56, rect.w - 16, 52};

        if (index < 0) {
            scene->hits[HIT_TRANSFER] = (Hit){row, true};
            continue;
        }

        scene->hits[HIT_DEVICE_FIRST + index] = (Hit){row, device_can_transfer(&view->devices.items[index])};
    }
}

static void devices_draw(Scene *scene, const View *view, Canvas *canvas, Rect rect) {
    Rect card = device_card(rect);
    int current = current_device(view);
    bool connected = view->playback.has_device;
    canvas->box(canvas->context, card, 8, DARK_TILE);

    if (!connected) {
        canvas_icon(canvas, ICON_SPEAKER, (Rect){card.x + 16, card.y + 18, 28, 28}, DARK_MUTED);
        label(canvas, "No active device", (Rect){card.x + 56, card.y + 14, card.w - 68, 20}, 14, true,
              DARK_TEXT);
        label(canvas, "Choose a device below to start listening.",
              (Rect){card.x + 56, card.y + 36, card.w - 68, 36}, 12, false, DARK_MUTED);
    } else {
        const Device *device = current >= 0 ? &view->devices.items[current] : NULL;
        bool local = view->engine_device_id[0] && !strcmp(view->playback.device_id, view->engine_device_id);
        Icon icon = device ? panel_device_icon(view, device) : local ? ICON_DEVICE : ICON_SPEAKER;
        const char *name = local ? "This computer" : view->playback.device[0] ? view->playback.device : "Device";

        canvas_icon(canvas, icon, (Rect){card.x + 16, card.y + 18, 28, 28}, DARK_ACCENT);
        label(canvas, view->playback.playing ? "Playing on" : "Current device",
              (Rect){card.x + 56, card.y + 12, card.w - 68, 18}, 11, false, DARK_MUTED);
        label(canvas, name, (Rect){card.x + 56, card.y + 30, card.w - 68, 24}, 15, true, DARK_ACCENT);

        if (view->playback.title[0]) {
            char now[1100];
            snprintf(now, sizeof(now), "%s%s%s", view->playback.title, view->playback.artist[0] ? " · " : "",
                     view->playback.artist);
            label(canvas, now, (Rect){card.x + 56, card.y + 54, card.w - 68, 20}, 12, false, DARK_MUTED);
        }
    }

    const HitId controls[] = {HIT_PANEL_PREVIOUS, HIT_PANEL_PLAY, HIT_PANEL_NEXT};
    const Icon icons[] = {ICON_PREVIOUS, view->playback.playing ? ICON_PAUSE : ICON_PLAY, ICON_NEXT};

    for (unsigned i = 0; i < 3; ++i) {
        HitId id = controls[i];
        Rect button = scene->hits[id].rect;
        bool enabled = scene->hits[id].enabled;
        bool play = id == HIT_PANEL_PLAY;
        float inset = play ? 11 : 8;

        if (play) {
            canvas->box(canvas->context, button, button.h / 2,
                        enabled ? color_mix(DARK_TEXT, 0xffffff, scene->hover[id]) : DARK_HOVER);
        } else if (scene->hover[id] > .01f) {
            canvas->box(canvas->context, button, button.h / 2, color_mix(DARK_TILE, DARK_HOVER, scene->hover[id]));
        }

        Color color = play ? DARK_BODY : enabled ? color_mix(DARK_MUTED, DARK_TEXT, scene->hover[id]) : 0x555555;
        canvas_icon(canvas, icons[i], (Rect){button.x + inset, button.y + inset, button.w - 2 * inset,
                                             button.h - 2 * inset}, color);
    }

    label(canvas, "Select another device", (Rect){rect.x + 16, rect.y + 206, rect.w - 32, 22}, 14, true,
          DARK_TEXT);

    int entries[DEVICE_LIMIT + 1];
    unsigned count = other_devices(view, entries);

    if (!count) {
        label(canvas, "No other devices found. Open Spotify on your phone, speaker or TV to see it here.",
              (Rect){rect.x + 16, device_list_top(rect) + 4, rect.w - 32, 54}, 12, false, DARK_MUTED);
        return;
    }

    for (unsigned slot = 0; slot < scene->device_visible && scene->device_first + slot < count; ++slot) {
        int index = entries[scene->device_first + slot];
        HitId id = index < 0 ? HIT_TRANSFER : (HitId)(HIT_DEVICE_FIRST + index);
        Rect row = scene->hits[id].rect;
        bool enabled = scene->hits[id].enabled;

        if (scene->hover[id] > .01f && enabled) {
            canvas->box(canvas->context, row, 6, color_mix(DARK_CHROME, DARK_HOVER, scene->hover[id]));
        }

        const Device *device = index < 0 ? NULL : &view->devices.items[index];
        bool local = !device || (view->engine_device_id[0] && !strcmp(device->id, view->engine_device_id));
        Icon icon = device ? panel_device_icon(view, device) : ICON_DEVICE;
        const char *name = local ? "This computer" : device->name;
        const char *detail = !device                ? "Set up Spotify Soloist to play here"
                             : device->restricted   ? "Can't be controlled from this app"
                             : local                ? "This app"
                                                    : "Spotify Connect";
        Color color = enabled ? DARK_TEXT : 0x666666;

        canvas_icon(canvas, icon, (Rect){row.x + 10, row.y + 13, 26, 26}, color);
        label(canvas, name, (Rect){row.x + 48, row.y + 7, row.w - 56, 20}, 14, false, color);
        label(canvas, detail, (Rect){row.x + 48, row.y + 27, row.w - 56, 18}, 11, false, DARK_MUTED);
    }
}

void panel_scene_toggle(Scene *scene, SidePanel panel) {
    scene->panel = scene->panel == panel ? PANEL_NONE : panel;
    scene->panel_recent = false;
    scene->panel_first = 0;
    scene->panel_scroll = (Scroll){0};
    scene->lyrics_fullscreen = false;
    lyrics_scene_reset(scene);
    app_lyrics_visible(scene->panel == PANEL_LYRICS);
    app_queue_visible(scene->panel == PANEL_QUEUE);
    app_devices_visible(scene->panel == PANEL_DEVICES);
    scene->device_first = 0;

    if (scene->panel == PANEL_QUEUE) {
        app_action(ACTION_QUEUE_PANEL);
    }
}

void panel_scene_layout(Scene *scene, const View *view) {
    if (scene->panel == PANEL_NONE) {
        return;
    }

    Rect rect = scene->side_panel;
    scene->hits[HIT_PANEL_CLOSE] = (Hit){{rect.x + rect.w - 36, rect.y + 11, 25, 26}, true};

    if (scene->panel == PANEL_LYRICS) {
        lyrics_scene_layout(scene, view);
        return;
    }

    if (scene->panel == PANEL_QUEUE) {
        scene->hits[HIT_PANEL_QUEUE_TAB] = (Hit){{rect.x + 14, rect.y + 11, 58, 26}, true};
        scene->hits[HIT_PANEL_RECENT_TAB] = (Hit){{rect.x + 79, rect.y + 11, 58, 26}, true};
        scene->hits[HIT_PANEL_QUEUE] = (Hit){{rect.x + rect.w - 71, rect.y + 11, 25, 26}, true};

        if (scene->panel_recent) {
            for (unsigned i = 0;
                 i < view->recent_count && i <= HIT_PANEL_RECENT_LAST - HIT_PANEL_RECENT_FIRST; ++i) {
                Rect row = {rect.x + 12, rect.y + 86 + i * QUEUE_ROW_HEIGHT, rect.w - 24, QUEUE_ROW_HEIGHT};
                scene->hits[HIT_PANEL_RECENT_FIRST + i] = (Hit){row, row.y + row.h <= rect.y + rect.h};
            }

            return;
        }

        float list_h = rect.h - QUEUE_LIST_TOP - QUEUE_LIST_BOTTOM;
        scroll_layout(&scene->panel_scroll, view->queue.total * QUEUE_ROW_HEIGHT, list_h);
        scene->panel_first = (unsigned)(scene->panel_scroll.offset / QUEUE_ROW_HEIGHT);

        if (scene->panel_first >= view->queue.total) {
            scene->panel_first = 0;
        }

        app_queue_viewport(scene->panel_first);

        Rect thumb = scroll_thumb(&scene->panel_scroll,
                                  (Rect){rect.x + rect.w - 5, rect.y + QUEUE_LIST_TOP, 4, list_h});

        float top = rect.y + QUEUE_LIST_TOP;
        float sub = scene->panel_scroll.offset - scene->panel_first * QUEUE_ROW_HEIGHT;

        for (unsigned i = 0; i < QUEUE_VISIBLE_SLOTS && scene->panel_first + i < view->queue.total; ++i) {
            const CatalogItem *item = queue_row(&view->queue, scene->panel_first + i);

            if (!item) {
                continue;
            }

            float y = top + i * QUEUE_ROW_HEIGHT - sub;
            float first = y > top ? y : top;
            float end = y + QUEUE_ROW_HEIGHT < top + list_h ? y + QUEUE_ROW_HEIGHT : top + list_h;

            if (item->kind != ITEM_TRACK || !spotify_id_valid(item->id) || end <= first) {
                continue;
            }

            scene->hits[HIT_QUEUE_ROW_FIRST + i] = (Hit){{rect.x + 8, first, rect.w - 16, end - first}, true};
            scene->hits[HIT_QUEUE_LIKE_FIRST + i] =
                (Hit){{rect.x + rect.w - 76, y + 8, 25, 26}, y + 8 >= first && y + 34 <= end};
        }
        scene->hits[HIT_SCROLL_PANEL] = (Hit){thumb, thumb.h > 0};
        scene->hits[HIT_QUEUE_CLEAR] = (Hit){{rect.x + rect.w - 98, rect.y + 172, 86, 26}, view->queue.queued > 0};
        return;
    }

    scene->panel_scroll = (Scroll){0};

    if (scene->panel == PANEL_DEVICES) {
        devices_layout(scene, view, rect);
    }
}

void panel_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    if (scene->panel == PANEL_NONE) {
        return;
    }

    if (scene->panel == PANEL_LYRICS) {
        lyrics_scene_draw(scene, view, canvas);
        return;
    }

    static const char *titles[] = {"", "Connect to a device", "Queue", "Friend Activity"};
    Rect rect = scene->side_panel;

    canvas->box(canvas->context, rect, 0, DARK_CHROME);
    canvas->box(canvas->context, (Rect){rect.x, rect.y, 1, rect.h}, 0, DARK_BORDER);
    canvas->clip(canvas->context, rect, 0);
    Rect title = {rect.x + 12, rect.y + 10, rect.w - 58, 24};

    if (scene->panel == PANEL_QUEUE) {
        for (unsigned i = 0; i < 2; ++i) {
            HitId id = i ? HIT_PANEL_RECENT_TAB : HIT_PANEL_QUEUE_TAB;
            Rect tab = scene->hits[id].rect;
            bool selected = scene->panel_recent == (i != 0);
            canvas->box(canvas->context, tab, tab.h / 2,
                        selected ? DARK_TEXT : color_mix(DARK_TILE, DARK_HOVER, scene->hover[id]));
            canvas->text(canvas->context, i ? "Recent" : "Queue", tab, 12, TEXT_CENTER | TEXT_VCENTER,
                         selected ? DARK_BODY : DARK_TEXT);
        }

        Rect full = scene->hits[HIT_PANEL_QUEUE].rect;
        canvas_icon(canvas, ICON_QUEUE, (Rect){full.x + 4, full.y + 4, 17, 17},
                    color_mix(DARK_MUTED, DARK_TEXT, scene->hover[HIT_PANEL_QUEUE]));
    } else {
        label(canvas, titles[scene->panel], title, 15, true, DARK_TEXT);
    }

    Rect close = scene->hits[HIT_PANEL_CLOSE].rect;

    if (scene->hover[HIT_PANEL_CLOSE] > .01f) {
        canvas->box(canvas->context, close, close.h / 2,
                    color_mix(DARK_CHROME, DARK_HOVER, scene->hover[HIT_PANEL_CLOSE]));
    }

    canvas_icon(canvas, ICON_CLOSE, (Rect){close.x + 6, close.y + 6, 13, 13},
                color_mix(DARK_MUTED, DARK_TEXT, scene->hover[HIT_PANEL_CLOSE]));

    if (scene->panel == PANEL_DEVICES) {
        devices_draw(scene, view, canvas, rect);
    }

    if (scene->panel == PANEL_QUEUE && scene->panel_recent) {
        label(canvas, "Recently played", (Rect){rect.x + 11, rect.y + 54, rect.w - 22, 22}, 13, true,
              DARK_TEXT);

        if (!view->recent_count) {
            label(canvas, "Your recently played songs will appear here.",
                  (Rect){rect.x + 11, rect.y + 87, rect.w - 22, 48}, 12, false, DARK_MUTED);
        }

        for (unsigned i = 0; i < view->recent_count && i <= HIT_PANEL_RECENT_LAST - HIT_PANEL_RECENT_FIRST;
             ++i) {
            HitId id = (HitId)(HIT_PANEL_RECENT_FIRST + i);

            if (!scene->hits[id].enabled) {
                continue;
            }

            Rect row = scene->hits[id].rect;
            const CatalogItem *item = &view->recent[i];
            canvas->box(canvas->context, row, 6, color_mix(DARK_CHROME, DARK_HOVER, scene->hover[id]));
            draw_thumb(canvas, item->artwork, (Rect){row.x + 7, row.y + 5, 32, 32}, 4, ICON_ALBUM);
            label(canvas, item->title, (Rect){row.x + 45, row.y + 5, row.w - 55, 19}, 13, false, DARK_TEXT);
            label(canvas, item->detail, (Rect){row.x + 45, row.y + 23, row.w - 55, 17}, 11, false,
                  DARK_MUTED);
        }
    }

    if (scene->panel == PANEL_QUEUE && !scene->panel_recent) {
        const char *source = NULL;
        const char *uri = view->playback.context_uri;

        if (!strncmp(uri, "spotify:playlist:", 17)) {
            const BrowsePage *playlists = &view->library[2];

            for (unsigned i = 0; i < playlists->count; ++i) {
                if (!strcmp(uri + 17, playlists->items[i].id)) {
                    source = playlists->items[i].title;
                    break;
                }
            }
        }

        if (!strncmp(uri, "spotify:album:", 14)) {
            source = view->playback.album;
        }

        if (source && source[0]) {
            label(canvas, "Playing from", (Rect){rect.x + 11, rect.y + 55, 74, 20}, 12, false, DARK_MUTED);
            label(canvas, source, (Rect){rect.x + 86, rect.y + 55, rect.w - 98, 20}, 12, true, DARK_TEXT);
        }

        label(canvas, "Now playing", (Rect){rect.x + 11, rect.y + 90, rect.w - 24, 20}, 12, true, DARK_TEXT);

        if (view->playback.title[0]) {
            Rect now_art = {rect.x + 19, rect.y + 119, 32, 32};
            draw_thumb(canvas, view->playback.artwork, now_art, 4, ICON_ALBUM);
            label(canvas, view->playback.title, (Rect){rect.x + 57, rect.y + 118, rect.w - 108, 19}, 13,
                  false, DARK_ACCENT);
            label(canvas, view->playback.artist, (Rect){rect.x + 57, rect.y + 136, rect.w - 108, 16}, 11,
                  false, DARK_MUTED);

            if (view->playback.duration_ms > 0) {
                char now_dur[24];
                snprintf(now_dur, sizeof(now_dur), "%u:%02u", view->playback.duration_ms / 60000,
                         view->playback.duration_ms / 1000 % 60);
                label(canvas, now_dur, (Rect){rect.x + rect.w - 48, rect.y + 128, 34, 17}, 11, false,
                      DARK_MUTED);
            }
        } else {
            label(canvas, "Nothing playing", (Rect){rect.x + 19, rect.y + 119, rect.w - 24, 20}, 12, false,
                  DARK_MUTED);
        }

        label(canvas, view->queue.queued ? "Next in queue" : "Next up",
              (Rect){rect.x + 11, rect.y + 175, rect.w - 120, 20}, 12, true, DARK_TEXT);

        Rect clear = scene->hits[HIT_QUEUE_CLEAR].rect;

        if (scene->hits[HIT_QUEUE_CLEAR].enabled) {
            canvas->box(canvas->context, clear, clear.h / 2,
                        color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_QUEUE_CLEAR]));
            canvas->text(canvas->context, "Clear queue", clear, 12, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                         color_mix(DARK_MUTED, DARK_TEXT, scene->hover[HIT_QUEUE_CLEAR]));
        }

        float list_h = rect.h - QUEUE_LIST_TOP - QUEUE_LIST_BOTTOM;
        float top = rect.y + QUEUE_LIST_TOP;
        canvas->clip(canvas->context, (Rect){rect.x + 8, top, rect.w - 16, list_h}, 0);

        for (unsigned i = 0; i < QUEUE_VISIBLE_SLOTS && scene->panel_first + i < view->queue.total; ++i) {
            const CatalogItem *item = queue_row(&view->queue, scene->panel_first + i);

            if (!item) {
                continue;
            }

            HitId id = (HitId)(HIT_QUEUE_ROW_FIRST + i);
            Rect cover = scene_song_art(scene, id);

            if (cover.y + 34 <= top || cover.y >= top + list_h) {
                continue;
            }

            draw_thumb(canvas, item->artwork, cover, 4, ICON_ALBUM);
            canvas_song_play(canvas, cover, 4, scene->hover[id], item_can_play(item));

            // A green edge marks songs the listener queued themselves.
            if (scene->panel_first + i < view->queue.queued) {
                canvas->box(canvas->context, (Rect){rect.x + 10, cover.y + 4, 3, cover.h - 8}, 1.5f, DARK_ACCENT);
            }
            float text_w = rect.w - 137;
            HitId like = (HitId)(HIT_QUEUE_LIKE_FIRST + i);
            Rect heart = scene->hits[like].rect;

            if (scene->hits[like].enabled &&
                (scene->hot == (HitId)(HIT_QUEUE_ROW_FIRST + i) || scene->hot == like || scene->focus == like)) {
                canvas_icon(canvas, item->liked ? ICON_HEART : ICON_HEART_OUTLINE,
                            (Rect){heart.x + 4, heart.y + 4, 18, 18},
                            item->liked ? 0x1ed760 : color_mix(DARK_MUTED, DARK_TEXT, scene->hover[like]));
            }
            label(canvas, item->title, (Rect){rect.x + 57, cover.y - 1, text_w, 19}, 13, false, DARK_TEXT);

            float sub_x = rect.x + 57;
            float sub_w = text_w;

            if (item->explicit_content) {
                canvas->box(canvas->context, (Rect){sub_x, cover.y + 18, 10, 10}, 2, DARK_MUTED);
                canvas->text(canvas->context, "E", (Rect){sub_x, cover.y + 18, 10, 10}, 8,
                             TEXT_CENTER | TEXT_VCENTER | TEXT_BOLD, DARK_BODY);
                sub_x += 14;
                sub_w -= 14;
            }

            label(canvas, item->detail, (Rect){sub_x, cover.y + 17, sub_w, 17}, 11, false, DARK_MUTED);

            if (item->duration_known) {
                char dur[24];
                snprintf(dur, sizeof(dur), "%u:%02u", item->duration_ms / 60000,
                         item->duration_ms / 1000 % 60);
                label(canvas, dur, (Rect){rect.x + rect.w - 48, cover.y + 9, 34, 17}, 11, false, DARK_MUTED);
            }
        }

        canvas->unclip(canvas->context);

        if (!view->queue.total) {
            label(canvas, "No upcoming tracks returned by Spotify.",
                  (Rect){rect.x + 12, top + 6, rect.w - 24, 54}, 12, false, DARK_MUTED);
        }
    }

    if (scene->panel == PANEL_FRIENDS) {
        label(canvas, "Not available through the public API",
              (Rect){rect.x + 16, rect.y + 76, rect.w - 32, 64}, 18, true, 0xffffff);
        label(canvas, "Spotify does not expose friends' listening activity to third-party clients. This "
                      "panel cannot show a live friends feed.",
              (Rect){rect.x + 16, rect.y + 160, rect.w - 32, 150}, 14, false, 0xb3b3b3);
    }

    canvas->unclip(canvas->context);
}
