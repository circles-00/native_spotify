#include "lyrics_scene.h"
#include "dark_palette.h"
#include "platform.h"

#include <string.h>

void lyrics_scene_reset(Scene *scene) {
    scene->lyrics_follow = true;
    scene->lyric_active = -2;
    scene->lyric_layout_width = 0;
    scene->panel_scroll = (Scroll){0};
}

Rect lyrics_scene_scroll_track(const Scene *scene) {
    return (Rect){scene->side_panel.x + scene->side_panel.w - 10, scene->lyric_list.y, 4,
                  scene->lyric_list.h};
}

void lyrics_scene_layout(Scene *scene, const View *view) {
    const Lyrics *lyrics = &view->lyrics;
    LyricsState state = lyrics_match(lyrics, &view->playback) ? lyrics->state : LYRICS_IDLE;
    Rect panel = scene->side_panel;
    const float width = scene->lyrics_fullscreen && panel.w > 1064 ? 1000 : panel.w - 36;
    scene->lyric_list = (Rect){panel.x + (panel.w - width) / 2, panel.y + 104, width, panel.h - 141};
    scene->hits[HIT_LYRICS_FULLSCREEN] = (Hit){{panel.x + panel.w - 70, panel.y + 11, 26, 26}, true};

    if (strcmp(scene->lyric_uri, view->playback.uri) || state != scene->lyric_state) {
        copy_utf8(scene->lyric_uri, sizeof(scene->lyric_uri), view->playback.uri);
        scene->lyric_state = state;
        lyrics_scene_reset(scene);

        if (scene->pressed >= HIT_LYRIC_FIRST && scene->pressed <= HIT_LYRIC_LAST) {
            scene->pressed = HIT_NONE;
        }
    }

    if ((state != LYRICS_SYNCED && state != LYRICS_PLAIN) || scene->lyric_layout_width != width ||
        scene->lyric_list.h <= 0) {
        return;
    }

    Scroll *scroll = &scene->panel_scroll;
    float follow_y = scene->lyric_list.h * .33f;
    float tail = state == LYRICS_SYNCED ? scene->lyric_list.h - follow_y : 0;
    scroll_layout(scroll, scene->lyric_y[lyrics->count] + tail, scene->lyric_list.h);
    int active = view->playback.position_known ? lyrics_active_line(lyrics, view->playback.position_ms) : -1;

    if (active != scene->lyric_active) {
        if (scene->lyrics_follow && state == LYRICS_SYNCED) {
            scroll_to(scroll, active < 0 ? 0 : scene->lyric_y[active] - follow_y, platform_milliseconds(),
                      scene->lyric_active != -2);
            scene->animating = scene->animating || scroll->active;
        }

        scene->lyric_active = active;
    }

    if (state == LYRICS_SYNCED && !scene->lyrics_follow) {
        scene->hits[HIT_LYRICS_FOLLOW] =
            (Hit){{panel.x + panel.w - 140, panel.y + panel.h - 32, 122, 25}, true};
    }

    Rect thumb = scroll_thumb(scroll, lyrics_scene_scroll_track(scene));
    scene->hits[HIT_SCROLL_PANEL] = (Hit){thumb, thumb.h > 0};

    if (state != LYRICS_SYNCED || !playback_can_seek(&view->playback)) {
        return;
    }

    for (unsigned i = 0; i < lyrics->count; ++i) {
        float top = scene->lyric_y[i] - scroll->offset;
        float end = scene->lyric_y[i + 1] - scroll->offset;
        top = top < 0 ? 0 : top;
        end = end > scene->lyric_list.h ? scene->lyric_list.h : end;

        if (end > top && lyrics->lines[i].time_ms < view->playback.duration_ms &&
            lyrics->text[lyrics->lines[i].text]) {
            scene->hits[HIT_LYRIC_FIRST + i] =
                (Hit){{scene->lyric_list.x, scene->lyric_list.y + top, width, end - top}, true};
        }
    }
}

static void wrap(Canvas *canvas, const char *text, Rect box, float size, bool bold, Color color, float *y) {
    float advance = size * 1.35f;
    char part[LYRICS_TEXT_LIMIT];

    do {
        size_t length = strlen(text);
        size_t take = length;
        float width = bold ? box.w * .91f : box.w;

        if (canvas->measure && canvas->measure(canvas->context, text, size) > width) {
            size_t fit = 0, space = 0;

            for (size_t end = 1; end <= length; ++end) {
                if (((unsigned char)text[end] & 0xc0) == 0x80) {
                    continue;
                }

                memcpy(part, text, end);
                part[end] = 0;

                if (fit && canvas->measure(canvas->context, part, size) > width) {
                    break;
                }

                fit = end;

                if (text[end] == ' ' || text[end] == '\t') {
                    space = end;
                }
            }

            take = fit == length ? fit : space ? space : fit;
        }

        if (color && take) {
            memcpy(part, text, take);
            part[take] = 0;
            canvas->text(canvas->context, part, (Rect){box.x, box.y + *y, box.w, advance}, size,
                         bold ? TEXT_BOLD : 0, color);
        }

        *y += advance;
        text += take;

        while (*text == ' ' || *text == '\t') {
            ++text;
        }
    } while (*text);
}

void lyrics_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    Rect panel = scene->side_panel;
    Rect list = scene->lyric_list;
    const Lyrics *lyrics = &view->lyrics;
    bool synced = scene->lyric_state == LYRICS_SYNCED;
    bool has_lyrics = synced || scene->lyric_state == LYRICS_PLAIN;
    float size = scene->lyrics_fullscreen ? (synced ? 48 : 34) : (synced ? 22 : 17);
    float gap = synced ? size * .48f : size * .23f;
    Color base = scene->lyrics_fullscreen ? 0x173e44 : 0x172e32;
    canvas->gradient(canvas->context, panel, 0, base, 0x101b1e);

    if (!scene->lyrics_fullscreen) {
        canvas->box(canvas->context, (Rect){panel.x, panel.y, 1, panel.h}, 0, DARK_BORDER);
    }

    canvas->text(canvas->context, "Lyrics", (Rect){panel.x + 18, panel.y + 11, panel.w - 100, 26}, 15,
                 TEXT_BOLD | TEXT_VCENTER, DARK_TEXT);
    canvas->text(canvas->context, lyrics_track(&view->playback) ? view->playback.title : "Now playing",
                 (Rect){list.x, panel.y + 46, list.w, 27}, 19, TEXT_BOLD, DARK_TEXT);
    canvas->text(canvas->context, view->playback.artist, (Rect){list.x, panel.y + 75, list.w, 20}, 13, 0,
                 DARK_MUTED);

    const HitId buttons[] = {HIT_LYRICS_FULLSCREEN, HIT_PANEL_CLOSE};
    const Icon icons[] = {scene->lyrics_fullscreen ? ICON_COLLAPSE : ICON_EXPAND, ICON_CLOSE};

    for (unsigned i = 0; i < 2; ++i) {
        HitId id = buttons[i];
        Rect button = scene->hits[id].rect;
        canvas_icon(canvas, icons[i], (Rect){button.x + 4, button.y + 4, 18, 18},
                    color_mix(DARK_MUTED, DARK_TEXT, scene->focus == id ? 1 : scene->hover[id]));
    }

    if (has_lyrics && list.h > 0 && list.w > 0) {
        if (scene->lyric_layout_width != list.w) {
            float y = 0;

            for (unsigned i = 0; i < lyrics->count; ++i) {
                scene->lyric_y[i] = y;
                const char *line = lyrics->text + lyrics->lines[i].text;
                wrap(canvas, line, list, size, synced, 0, &y);
                y += gap;
            }

            scene->lyric_y[lyrics->count] = y;
            scene->lyric_layout_width = list.w;
            scene->lyric_active = -2;
            lyrics_scene_layout(scene, view);
        }

        canvas->clip(canvas->context, list, 0);

        for (unsigned i = 0; i < lyrics->count; ++i) {
            float top = scene->lyric_y[i] - scene->panel_scroll.offset;

            if (top >= list.h || scene->lyric_y[i + 1] <= scene->panel_scroll.offset) {
                continue;
            }

            HitId hit = (HitId)(HIT_LYRIC_FIRST + i);
            bool current = synced && (int)i == scene->lyric_active;
            bool hover = scene->hits[hit].enabled && (scene->hot == hit || scene->focus == hit);
            Color color = current || !synced ? DARK_TEXT : hover ? 0xe1e9e9 : 0x93abad;
            const char *line = lyrics->text + lyrics->lines[i].text;
            wrap(canvas, current && !*line ? "…" : line, list, size, synced, color, &top);
        }

        canvas->unclip(canvas->context);
    } else if (!has_lyrics) {
        const char *message = !lyrics_track(&view->playback)              ? "Play a song to see its lyrics."
                              : scene->lyric_state == LYRICS_INSTRUMENTAL ? "This track is instrumental."
                              : scene->lyric_state == LYRICS_MISSING      ? "No lyrics found for this song."
                              : scene->lyric_state == LYRICS_ERROR ? "Couldn't load lyrics. Reopen to retry."
                                                                   : "Looking up lyrics…";
        float y = 8;
        wrap(canvas, message, list, 16, false, DARK_TEXT, &y);
    }

    canvas->text(canvas->context,
                 scene->lyric_state == LYRICS_PLAIN ? "Unsynced · LRCLIB" : "Lyrics by LRCLIB",
                 (Rect){list.x, panel.y + panel.h - 29, 150, 18}, 11, 0, DARK_MUTED);
    Hit follow = scene->hits[HIT_LYRICS_FOLLOW];

    if (follow.enabled) {
        canvas->box(canvas->context, follow.rect, 12, DARK_HOVER);
        canvas->text(canvas->context, "Follow playback", follow.rect, 11,
                     TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER, DARK_TEXT);
    }
}
