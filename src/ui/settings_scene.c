#include "settings_scene.h"
#include "dark_palette.h"

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

static Rect settings_card_rect(const Scene *scene) {
    float x = scene->content.x + 24;
    float y = scene->content.y - scene->main_scroll.offset;
    float card_w = scene->content.w - 48;

    if (card_w > 760) {
        card_w = 760;
    }

    if (card_w < 260) {
        card_w = 260;
    }

    return (Rect){x, y + 136, card_w, 88};
}

void settings_scene_layout(Scene *scene, const View *view) {
    (void)view;

    scroll_layout(&scene->main_scroll, 260, scene->content.h);
    Rect card = settings_card_rect(scene);
    Rect toggle = {card.x + card.w - 64, card.y + (card.h - 24) * 0.5f, 44, 24};
    bool visible = card.y + card.h > scene->content.y && card.y < scene->content.y + scene->content.h;
    scene->hits[HIT_SETTING_SEARCH_RADIO] = (Hit){toggle, visible};
}

void settings_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    float x = scene->content.x + 24;
    float y = scene->content.y - scene->main_scroll.offset;
    float width = scene->content.w - 48;

    label(canvas, "Settings", (Rect){x, y + 36, width, 46}, 32, true, DARK_TEXT);
    label(canvas, "Playback", (Rect){x, y + 102, width, 24}, 16, true, DARK_TEXT);

    Rect card = settings_card_rect(scene);
    canvas->box(canvas->context, card, 8,
                color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_SETTING_SEARCH_RADIO] * 0.35f));

    float text_w = card.w - 100;

    if (text_w < 120) {
        text_w = 120;
    }

    label(canvas, "Autoplay song radio from search", (Rect){card.x + 20, card.y + 16, text_w, 22}, 14, true,
          DARK_TEXT);
    label(canvas, "Start song radio in your queue when playing from search results with Repeat off",
          (Rect){card.x + 20, card.y + 42, text_w, 18}, 12, false, DARK_MUTED);
    label(canvas, "or when Repeat is turned off during that song.",
          (Rect){card.x + 20, card.y + 60, text_w, 18}, 12, false, DARK_MUTED);

    Rect toggle = scene->hits[HIT_SETTING_SEARCH_RADIO].rect;
    bool enabled = view->settings.search_radio;
    Color track = enabled ? color_mix(DARK_ACCENT, 0x3be477, scene->hover[HIT_SETTING_SEARCH_RADIO])
                          : color_mix(0x535353, 0x6a6a6a, scene->hover[HIT_SETTING_SEARCH_RADIO]);
    canvas->box(canvas->context, toggle, toggle.h * 0.5f, track);

    float knob_x = enabled ? toggle.x + toggle.w - 21 : toggle.x + 3;
    canvas->ellipse(canvas->context, (Rect){knob_x, toggle.y + 3, 18, 18}, true, 0, 0xffffffu);
}
