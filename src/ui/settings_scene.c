#include "settings_scene.h"
#include "dark_palette.h"

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

static Rect settings_card_rect(const Scene *scene, float top) {
    float x = scene->content.x + 24;
    float y = scene->content.y - scene->main_scroll.offset;
    float card_w = scene->content.w - 48;

    if (card_w > 760) {
        card_w = 760;
    }

    if (card_w < 260) {
        card_w = 260;
    }

    return (Rect){x, y + top, card_w, 88};
}

void settings_scene_layout(Scene *scene, const View *view) {
    scroll_layout(&scene->main_scroll, 630, scene->content.h);
    Rect radio_card = settings_card_rect(scene, 136);
    Rect toggle = {radio_card.x + radio_card.w - 64, radio_card.y + (radio_card.h - 24) * 0.5f, 44, 24};
    bool radio_visible =
        radio_card.y + radio_card.h > scene->content.y && radio_card.y < scene->content.y + scene->content.h;
    scene->hits[HIT_SETTING_SEARCH_RADIO] = (Hit){toggle, radio_visible};

    static const HitId home_ids[4] = {HIT_SETTING_REGEN_MIXES, HIT_SETTING_REGEN_PLAYLISTS,
                                      HIT_SETTING_REGEN_TRACKS, HIT_SETTING_REGEN_ALL};

    for (unsigned i = 0; i < 4; ++i) {
        Rect card = settings_card_rect(scene, 286.0f + i * 76.0f);
        card.h = 66;
        float btn_w = i == 3 ? 132.0f : 112.0f;
        Rect btn = {card.x + card.w - btn_w - 20.0f, card.y + (card.h - 32.0f) * 0.5f, btn_w, 32.0f};
        bool visible = card.y + card.h > scene->content.y && card.y < scene->content.y + scene->content.h;
        scene->hits[home_ids[i]] = (Hit){btn, visible && view->authenticated};
    }
}

void settings_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    float x = scene->content.x + 24;
    float y = scene->content.y - scene->main_scroll.offset;
    float width = scene->content.w - 48;

    label(canvas, "Settings", (Rect){x, y + 36, width, 46}, 32, true, DARK_TEXT);
    label(canvas, "Playback", (Rect){x, y + 102, width, 24}, 16, true, DARK_TEXT);

    Rect card = settings_card_rect(scene, 136);
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

    label(canvas, "Home", (Rect){x, y + 252, width, 24}, 16, true, DARK_TEXT);

    static const HitId home_ids[4] = {HIT_SETTING_REGEN_MIXES, HIT_SETTING_REGEN_PLAYLISTS,
                                      HIT_SETTING_REGEN_TRACKS, HIT_SETTING_REGEN_ALL};
    static const char *home_titles[4] = {"Daily mixes", "Suggested playlists", "Recommended songs",
                                         "All Home suggestions"};
    static const char *home_subs[4] = {"Rebuild client-made mixes from your recent artists.",
                                       "Rebuild Spotify playlist suggestions from your recent artists.",
                                       "Rebuild recommended songs from your recent listens.",
                                       "Drop the 24-hour cache and regenerate every Home shelf."};
    static const char *home_btns[4] = {"Regenerate", "Regenerate", "Regenerate", "Regenerate all"};

    for (unsigned i = 0; i < 4; ++i) {
        Rect row = settings_card_rect(scene, 286.0f + i * 76.0f);
        row.h = 66;
        float hover = scene->hover[home_ids[i]];
        canvas->box(canvas->context, row, 8, color_mix(DARK_TILE, DARK_HOVER, hover * 0.35f));

        float row_text_w = row.w - (i == 3 ? 184.0f : 164.0f);

        if (row_text_w < 120) {
            row_text_w = 120;
        }

        label(canvas, home_titles[i], (Rect){row.x + 20, row.y + 13, row_text_w, 20}, 14, true, DARK_TEXT);
        label(canvas, home_subs[i], (Rect){row.x + 20, row.y + 36, row_text_w, 18}, 12, false, DARK_MUTED);

        Rect btn = scene->hits[home_ids[i]].rect;
        Color pill_fill = i == 3 ? color_mix(DARK_ACCENT, 0x3be477, hover)
                                 : color_mix(DARK_CONTROL, 0x454545, hover);
        Color pill_text = i == 3 ? DARK_BODY : DARK_TEXT;
        canvas->box(canvas->context, btn, btn.h * 0.5f, pill_fill);
        canvas->text(canvas->context, home_btns[i], btn, 12, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                     pill_text);
    }
}
