#include "collection_scene.h"
#include "artwork_cache.h"
#include "dark_palette.h"
#include "skeleton.h"

#include <stdio.h>
#include <string.h>

static void label(Canvas *canvas, const char *value, Rect rect, float size, bool bold, Color color) {
    canvas->text(canvas->context, value, rect, size, bold ? TEXT_BOLD : 0, color);
}

static void equalizer(Canvas *canvas, float x, float y) {
    canvas->box(canvas->context, (Rect){x, y + 6, 2.5f, 8}, 1, 0x1ed760);
    canvas->box(canvas->context, (Rect){x + 4.5f, y + 1, 2.5f, 13}, 1, 0x1ed760);
    canvas->box(canvas->context, (Rect){x + 9, y + 4, 2.5f, 10}, 1, 0x1ed760);
}

static void format_commas(uint64_t value, char *out, size_t capacity) {
    char digits[32];
    int len = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);

    if (len <= 0 || !capacity) {
        if (capacity) {
            out[0] = 0;
        }

        return;
    }

    size_t pos = 0;

    for (int i = 0; i < len && pos + 1 < capacity; ++i) {
        if (i > 0 && (len - i) % 3 == 0 && pos + 2 < capacity) {
            out[pos++] = ',';
        }

        out[pos++] = digits[i];
    }

    out[pos] = 0;
}

static float hero_height(const Scene *scene) {
    return scene->section == BROWSE_ARTIST_ALBUMS ? 248 : scene->content.w >= 800 ? 228 : 204;
}

static unsigned popular_count(const Scene *scene, const View *view) {
    return scene->popular_expanded || view->details.popular_count < 5 ? view->details.popular_count : 5;
}

static float releases_y(const Scene *scene, const View *view) {
    if (scene->section == BROWSE_PROFILE_PLAYLISTS) {
        return hero_height(scene) + 28;
    }

    return 248 + 124 + (popular_count(scene, view) ? popular_count(scene, view) * 48 + 56 : 64);
}

static void place(Scene *scene, HitId id, Rect rect, bool enabled) {
    scene->hits[id] = (Hit){rect, enabled && rect.y + rect.h > scene->content.y &&
                                      rect.y < scene->content.y + scene->content.h};
}

void collection_scene_layout(Scene *scene, const View *view) {
    bool album = scene->section == BROWSE_ALBUM_TRACKS;
    bool profile = scene->section == BROWSE_PROFILE_PLAYLISTS;
    float width = scene->content.w - 48;
    unsigned columns = scene_columns(scene->content.w);
    float card = (width - (columns - 1) * 16) / columns;
    float rows_y = album ? hero_height(scene) + 112 : releases_y(scene, view) + (profile ? 40 : 80);
    float row_height = album ? 48 : card + 72;
    unsigned display_count = skeleton_count(&view->list, album ? 5 : columns);
    float rows_height = (album ? display_count : (display_count + columns - 1) / columns) * row_height;
    float footer = album ? 156 : profile ? card + 140 : 32;
    scroll_layout(&scene->main_scroll, rows_y + rows_height + footer, scene->content.h);
    scene->main_origin = rows_y;
    scene->main_row_height = row_height;
    scene->main_columns = album ? 1 : columns;
    float row_offset = scene->main_scroll.offset > rows_y ? scene->main_scroll.offset - rows_y : 0;
    scene->first_row = (unsigned)(row_offset / row_height) * scene->main_columns;

    if (scene->first_row > view->list.count) {
        scene->first_row = view->list.count;
    }

    float x = scene->content.x + 24;
    float y = scene->content.y - scene->main_scroll.offset;
    float hero = hero_height(scene);

    if (!profile) {
        place(scene, HIT_DETAIL_PLAY, (Rect){x, y + hero + 16, 52, 52},
              spotify_id_valid(view->details.item.id));
        place(scene, HIT_DETAIL_SHUFFLE, (Rect){x + 70, y + hero + 28, 28, 28},
              view->playback.has_device && !view->playback.restricted);
        place(scene, HIT_DETAIL_SAVE, (Rect){x + 114, y + hero + 25, album ? 34 : 112, 34},
              view->details.loaded && !view->details.saving);
        place(scene, HIT_DETAIL_LINK, (Rect){x + (album ? 166 : 244), y + hero + 28, 28, 28},
              spotify_id_valid(view->details.item.id));
    }

    if (!album && !profile) {
        place(scene, HIT_ARTIST_LIKES, (Rect){x + 282, y + hero + 25, width - 282, 34},
              spotify_id_valid(view->list.request.id));
    }

    if (album) {
        float cover_side = hero - 48;
        float left = x + cover_side + 24;
        place(scene, HIT_DETAIL_ARTIST, (Rect){left, y + hero - 54, width - cover_side - 24, 22},
              spotify_id_valid(view->details.artist.id));
    }

    if (!album && !profile) {
        for (unsigned i = 0; i < popular_count(scene, view); ++i) {
            place(scene, (HitId)(HIT_POPULAR_FIRST + i), (Rect){x - 8, y + 372 + i * 48, width + 16, 48},
                  view->details.popular[i].playable);
        }

        if (view->details.popular_count > 5) {
            place(scene, HIT_DETAIL_ALL, (Rect){x, y + 378 + popular_count(scene, view) * 48, 96, 26}, true);
        }

        for (unsigned i = 0; i < 3; ++i) {
            place(scene, (HitId)(HIT_DISC_ALL + i),
                  (Rect){x + i * 108, y + releases_y(scene, view) + 34, 100, 30}, true);
        }
    }

    if (profile) {
        place(scene, HIT_PROFILE_FOLLOWING, (Rect){x + hero - 24, y + hero - 48, width - hero + 24, 24},
              true);
        float following_y = y + rows_y + rows_height + 64;

        for (unsigned i = 0; i < view->profile.following_count && i < columns; ++i) {
            place(scene, (HitId)(HIT_PROFILE_ARTIST_FIRST + i),
                  (Rect){x + i * (card + 16), following_y, card, card + 72}, true);
        }
    }

    unsigned shown = 0;

    for (unsigned slot = 0; slot < ROW_SLOTS && scene->first_row + slot < display_count; ++slot) {
        unsigned index = scene->first_row + slot;
        Rect rect = {x + (album ? -8 : (index % columns) * (card + 16)),
                     y + rows_y + (album ? index : index / columns) * row_height, album ? width + 16 : card,
                     album ? 48 : card + 66};

        if (rect.y >= scene->content.y + scene->content.h) {
            break;
        }

        place(scene, (HitId)(HIT_ROW_FIRST + slot), rect, index < view->list.count);
        shown = slot + 1;
    }

    if (view->list.more && !view->list.loading && scene->first_row + shown + ROW_SLOTS >= view->list.count) {
        app_load_more();
    }

    app_viewport(scene->first_row);
}

static void cover(Canvas *canvas, const char *url, Rect rect, bool circle) {
    float radius = circle ? rect.w / 2 : (rect.w >= 120 ? 6.0f : 4.0f);

    if (!canvas_artwork(canvas, url, rect, radius)) {
        canvas->box(canvas->context, rect, radius, 0x343b3a);
        canvas_icon(canvas, circle ? ICON_USER : ICON_ALBUM,
                    (Rect){rect.x + rect.w * .3f, rect.y + rect.h * .3f, rect.w * .4f, rect.h * .4f},
                    0xb3b3b3);
    }
}

static void track(Scene *scene, const View *view, Canvas *canvas, HitId id, const CatalogItem *item,
                  unsigned index, bool popular) {
    Rect rect = scene->hits[id].rect;

    if (rect.w <= 0 || rect.y + rect.h <= scene->content.y || rect.y >= scene->content.y + scene->content.h) {
        return;
    }

    bool selected = popular ? scene->popular_selected == index : view->list.selected == (int)index;
    float hover = scene->hover[id];

    if (selected || hover > .01f) {
        canvas->box(canvas->context, rect, 6,
                    color_mix(selected ? DARK_SELECTED : DARK_BODY, DARK_HOVER, hover));
    }

    bool playing = view->playback.uri[0] && strlen(view->playback.uri) > 22 &&
                   !strcmp(view->playback.uri + strlen(view->playback.uri) - 22, item->id);

    if (playing) {
        equalizer(canvas, rect.x + 15, rect.y + 17);
    } else {
        char number[24];
        snprintf(number, sizeof(number), "%u",
                 !popular && item->track_number ? item->track_number : index + 1);
        label(canvas, number, (Rect){rect.x + 16, rect.y + 13, 28, 22}, 13, false, 0xb3b3b3);
    }

    float left = rect.x + 52;

    Rect art = scene_song_art(scene, id);
    cover(canvas, item->artwork[0] ? item->artwork : scene->collection_artwork, art, false);
    canvas_song_play(canvas, art, 4, hover, item_can_play(item));
    left += 48;

    bool has_sub = item->explicit_content || item->disc_number > 1 || item->detail[0] != 0;
    label(canvas, item->title, (Rect){left, rect.y + (has_sub ? 4 : 13), rect.x + rect.w - left - 103, 22}, 14,
          false,
          !item->playable ? 0x777777
          : playing       ? 0x1ed760
                          : 0xffffff);
    float detail_x = left;

    if (item->explicit_content) {
        canvas->box(canvas->context, (Rect){left, rect.y + 28, 13, 13}, 2, 0xa7a7a7);
        canvas->text(canvas->context, "E", (Rect){left, rect.y + 28, 13, 13}, 9,
                     TEXT_CENTER | TEXT_VCENTER | TEXT_BOLD, 0x121212);
        detail_x += 20;
    }

    char detail[320];

    if (item->disc_number > 1) {
        snprintf(detail, sizeof(detail), "Disc %u%s%s", item->disc_number, item->detail[0] ? " · " : "",
                 item->detail);
    }

    label(canvas, item->disc_number > 1 ? detail : item->detail,
          (Rect){detail_x, rect.y + 25, rect.x + rect.w - detail_x - 103, 20}, 12, false,
          color_mix(0xb3b3b3, 0xffffff, hover * .45f));

    char duration[24];
    snprintf(duration, sizeof(duration), "%u:%02u", item->duration_ms / 60000, item->duration_ms / 1000 % 60);
    label(canvas, item->duration_known ? duration : "—", (Rect){rect.x + rect.w - 64, rect.y + 13, 56, 22},
          13, false, 0xb3b3b3);
}

static void release_card(Scene *scene, Canvas *canvas, HitId id, const CatalogItem *item, bool artist) {
    Rect rect = scene->hits[id].rect;

    if (rect.w <= 0 || rect.y + rect.h <= scene->content.y || rect.y >= scene->content.y + scene->content.h) {
        return;
    }

    canvas->box(canvas->context, rect, 6, color_mix(DARK_BODY, DARK_HOVER, scene->hover[id]));
    Rect art = {rect.x + 12, rect.y + 12, rect.w - 24, rect.w - 24};
    cover(canvas, item->artwork, art, artist);
    label(canvas, item->title, (Rect){rect.x + 12, art.y + art.h + 10, rect.w - 24, 22}, 14, true, 0xffffff);

    char detail[320];
    snprintf(detail, sizeof(detail), "%s%s%s", item->release_date, item->release_date[0] ? " · " : "",
             artist                ? "Artist"
             : item->album_type[0] ? item->album_type
                                   : item->detail);
    label(canvas, detail, (Rect){rect.x + 12, art.y + art.h + 34, rect.w - 24, 22}, 12, false, 0xb3b3b3);
}

void collection_scene_draw(Scene *scene, const View *view, Canvas *canvas) {
    bool album = scene->section == BROWSE_ALBUM_TRACKS;
    bool profile = scene->section == BROWSE_PROFILE_PLAYLISTS;
    Rect bounds = scene->content;
    float x = bounds.x + 24;
    float y = bounds.y - scene->main_scroll.offset;
    float hero = hero_height(scene);
    float width = bounds.w - 48;
    const char *title = profile ? (view->profile.loaded ? view->profile.name : "Your profile")
                        : view->details.item.title[0] ? view->details.item.title
                                                      : scene->collection;
    const char *image = profile ? view->profile.artwork : view->details.item.artwork;
    Color tint = profile ? 0x405852 : 0x354c57;

    artwork_color(image, &tint);

    Color mid = color_mix(tint, DARK_BODY, .62f);
    canvas->gradient(canvas->context, (Rect){bounds.x, y, bounds.w, hero}, 0, tint, mid);
    canvas->gradient(canvas->context, (Rect){bounds.x, y + hero, bounds.w, 110}, 0, mid, DARK_BODY);

    if (album || profile) {
        float side = hero - 48;
        cover(canvas, image, (Rect){x, y + 24, side, side}, profile);
        float left = x + side + 24;
        float available = bounds.x + bounds.w - 24 - left;
        const char *kind_text = profile                                                 ? "Profile"
                                : !strcmp(view->details.item.album_type, "single")      ? "Single"
                                : !strcmp(view->details.item.album_type, "compilation") ? "Compilation"
                                                                                        : "Album";

        if (album) {
            bool long_title = strlen(title) > 28 || available < 400;
            label(canvas, kind_text, (Rect){left, y + hero - (long_title ? 156 : 138), available, 20}, 13,
                  true, 0xffffff);
            label(canvas, title,
                  (Rect){left, y + hero - (long_title ? 134 : 114), available, long_title ? 72 : 52},
                  long_title ? 30 : 42, true, 0xffffff);

            label(canvas,
                  view->details.artist.title[0] ? view->details.artist.title : view->details.item.detail,
                  scene->hits[HIT_DETAIL_ARTIST].rect, 14, true,
                  color_mix(0xffffff, 0x1ed760, scene->hover[HIT_DETAIL_ARTIST] * .35f));

            char summary[192];
            char duration[64] = {0};
            bool complete =
                view->details.duration_complete ||
                (!view->list.more && view->list.count == view->list.total && view->list.duration_known);

            if (complete) {
                collection_duration_label(view->details.duration_complete ? view->details.duration_ms
                                                                          : view->list.duration_ms,
                                          duration, sizeof(duration));
            }

            unsigned count = view->details.total_tracks ? view->details.total_tracks : view->list.total;
            snprintf(summary, sizeof(summary), "%.4s%s%u songs%s%s", view->details.item.release_date,
                     view->details.item.release_date[0] ? " · " : "", count, duration[0] ? ", " : "",
                     duration);
            if (view->details.total_tracks || view->list.loaded) {
                label(canvas, summary, (Rect){left, y + hero - 28, available, 22}, 12, false, 0xe0e0e0);
            } else if (view->list.loading) {
                skeleton_block(canvas, (Rect){left, y + hero - 22, available * .38f, 10}, 4);
            }
        }

        if (profile) {
            label(canvas, kind_text, (Rect){left, y + hero - 138, available, 20}, 13, true, 0xffffff);
            label(canvas, title, (Rect){left, y + hero - 114, available, 58},
                  strlen(title) > 28 || available < 400 ? 30 : 42, true, 0xffffff);

            char stats[192];

            if (view->profile.permission_required) {
                snprintf(stats, sizeof(stats), "Enable following permissions");
            } else if (view->profile.followers_known && view->profile.following_known) {
                snprintf(stats, sizeof(stats), "%llu followers · %u following",
                         (unsigned long long)view->profile.followers, view->profile.following_total);
            } else if (view->profile.followers_known) {
                snprintf(stats, sizeof(stats), "%llu followers · View followed artists",
                         (unsigned long long)view->profile.followers);
            } else if (view->profile.following_known) {
                snprintf(stats, sizeof(stats), "%u following", view->profile.following_total);
            } else {
                snprintf(stats, sizeof(stats), "Your public playlists and followed artists");
            }

            label(canvas, stats, scene->hits[HIT_PROFILE_FOLLOWING].rect, 13,
                  view->profile.permission_required,
                  view->profile.permission_required
                      ? 0x1ed760
                      : color_mix(0xffffff, 0x1ed760, scene->hover[HIT_PROFILE_FOLLOWING] * .35f));
        }
    }

    if (!album && !profile) {
        float side = hero - 48;
        cover(canvas, image, (Rect){x, y + 24, side, side}, true);
        float left = x + side + 24;
        float available = bounds.x + bounds.w - 24 - left;
        label(canvas, "Artist", (Rect){left, y + hero - 146, available, 22}, 13, true, 0xffffff);
        label(canvas, title, (Rect){left, y + hero - 120, available, 68}, width >= 800 ? 48 : 36, true,
              0xffffff);

        if (view->details.followers_known) {
            char count_text[32];
            char followers[96];
            format_commas(view->details.followers, count_text, sizeof(count_text));
            snprintf(followers, sizeof(followers), "%s followers", count_text);
            label(canvas, followers, (Rect){left, y + hero - 44, available, 22}, 13, false, 0xe0e0e0);
        }
    }

    if (!profile) {
        Rect play = scene->hits[HIT_DETAIL_PLAY].rect;
        canvas->box(canvas->context, play, play.h / 2,
                    color_mix(0x1ed760, 0x3be477, scene->hover[HIT_DETAIL_PLAY]));
        canvas_icon(canvas, ICON_PLAY, (Rect){play.x + 15, play.y + 15, 22, 22}, 0);

        Rect shuffle = scene->hits[HIT_DETAIL_SHUFFLE].rect;
        canvas_icon(canvas, ICON_SHUFFLE, (Rect){shuffle.x + 3, shuffle.y + 3, 22, 22},
                    view->playback.shuffle ? 0x1ed760
                                           : color_mix(0xb3b3b3, 0xffffff, scene->hover[HIT_DETAIL_SHUFFLE]));

        Rect save = scene->hits[HIT_DETAIL_SAVE].rect;

        if (album && !view->details.permission_required) {
            Color save_color =
                view->details.saved ? 0x1ed760 : color_mix(0xb3b3b3, 0xffffff, scene->hover[HIT_DETAIL_SAVE]);
            canvas->ellipse(canvas->context, (Rect){save.x + 4, save.y + 4, 26, 26}, false, 2, save_color);
            canvas_icon(canvas, view->details.saved ? ICON_CHECK : ICON_PLUS,
                        (Rect){save.x + 9, save.y + 9, 16, 16}, save_color);
        }

        if (!album || view->details.permission_required) {
            canvas->box(canvas->context, save, 17,
                        color_mix(0x303030, 0x424242, scene->hover[HIT_DETAIL_SAVE]));
            canvas->text(canvas->context,
                         view->details.permission_required ? "Authorize"
                         : view->details.saved             ? "Following"
                                                           : "Follow",
                         save, 13, TEXT_CENTER | TEXT_VCENTER | TEXT_BOLD, 0xffffff);
        }

        Rect link = scene->hits[HIT_DETAIL_LINK].rect;
        canvas_icon(canvas, ICON_LINK, (Rect){link.x + 4, link.y + 4, 20, 20},
                    color_mix(0xb3b3b3, 0xffffff, scene->hover[HIT_DETAIL_LINK]));
    }

    if (album) {
        float header = y + hero + 82;
        label(canvas, "#", (Rect){x + 8, header, 28, 22}, 12, false, 0xb3b3b3);
        label(canvas, "Title", (Rect){x + 44, header, width - 128, 22}, 12, false, 0xb3b3b3);
        canvas_icon(canvas, ICON_CLOCK, (Rect){bounds.x + bounds.w - 72, header + 2, 16, 16}, 0xb3b3b3);
        canvas->box(canvas->context, (Rect){x, header + 24, width, 1}, 0, 0x2e2e2e);
    }

    if (!album && !profile) {
        Rect liked = scene->hits[HIT_ARTIST_LIKES].rect;
        char liked_label[80];

        snprintf(liked_label, sizeof(liked_label), "Liked from artist · %u%s", view->artist_liked_count,
                 view->artist_likes_ready ? "" : "+");

        canvas_icon(canvas, view->artist_liked_count ? ICON_HEART : ICON_HEART_OUTLINE,
                    (Rect){liked.x + 4, liked.y + 8, 18, 18}, view->artist_liked_count ? 0x1ed760 : 0xb3b3b3);
        canvas->text(canvas->context, liked_label, (Rect){liked.x + 30, liked.y, liked.w - 32, liked.h}, 12,
                     TEXT_VCENTER | TEXT_BOLD, color_mix(0xb3b3b3, 0xffffff, scene->hover[HIT_ARTIST_LIKES]));

        label(canvas, "Popular", (Rect){x, y + 332, width, 30}, 21, true, 0xffffff);

        if (!view->details.popular_count) {
            label(canvas,
                  view->details.popular_loaded
                      ? "Popular tracks aren't available from Spotify for this artist."
                      : "",
                  (Rect){x, y + 372, width, 28}, 13, false, 0xb3b3b3);
        }

        for (unsigned i = 0; i < popular_count(scene, view); ++i) {
            track(scene, view, canvas, (HitId)(HIT_POPULAR_FIRST + i), &view->details.popular[i], i, true);
        }

        if (view->details.popular_count > 5) {
            label(canvas, scene->popular_expanded ? "See less" : "See more", scene->hits[HIT_DETAIL_ALL].rect,
                  13, true, color_mix(0xb3b3b3, 0xffffff, scene->hover[HIT_DETAIL_ALL]));
        }

        label(canvas, "Discography", (Rect){x, y + releases_y(scene, view), width, 30}, 21, true, 0xffffff);
        static const char *filters[] = {"All releases", "Albums", "Singles & EPs"};

        for (unsigned i = 0; i < 3; ++i) {
            Rect rect = scene->hits[HIT_DISC_ALL + i].rect;
            bool active = i == 0 ? !view->list.request.query[0]
                                 : !strcmp(view->list.request.query, i == 1 ? "album" : "single");
            Color bg = active ? DARK_TEXT : color_mix(DARK_TILE, DARK_HOVER, scene->hover[HIT_DISC_ALL + i]);
            canvas->box(canvas->context, rect, 16, bg);
            canvas->text(canvas->context, filters[i], rect, 12, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER,
                         active ? 0 : 0xffffff);
        }
    }

    if (profile) {
        label(canvas, "Public playlists", (Rect){x, y + releases_y(scene, view), width, 34}, 24, true,
              0xffffff);
    }

    for (unsigned slot = 0; slot < ROW_SLOTS; ++slot) {
        unsigned index = scene->first_row + slot;
        const CatalogItem *item = list_row(&view->list, index);

        if (!item) {
            Rect rect = scene->hits[HIT_ROW_FIRST + slot].rect;

            if (view->list.loading && index >= view->list.count && rect.w > 0 && rect.y + rect.h > bounds.y) {
                if (album) {
                    skeleton_row(canvas, rect, 0, 0, 52);
                } else {
                    skeleton_card(canvas, rect, false, 0);
                }
            }

            continue;
        }

        if (album) {
            track(scene, view, canvas, (HitId)(HIT_ROW_FIRST + slot), item, scene->first_row + slot, false);
        }

        if (!album) {
            release_card(scene, canvas, (HitId)(HIT_ROW_FIRST + slot), item, false);
        }
    }

    float footer_y = y + scene->main_scroll.extent - (album ? 136 : 20);

    if (album) {
        char release[96];
        collection_release_label(&view->details, release, sizeof(release));
        label(canvas, release, (Rect){x, footer_y, width, 20}, 12, false, 0xb3b3b3);
        label(canvas, view->details.label, (Rect){x, footer_y + 24, width, 20}, 11, false, 0xb3b3b3);
        label(canvas, view->details.copyrights, (Rect){x, footer_y + 48, width, 84}, 11, false, 0xb3b3b3);
    }

    if (profile) {
        unsigned columns = scene_columns(bounds.w);
        float card = (width - (columns - 1) * 16) / columns;
        float following_y =
            y + scene->main_origin + ((view->list.count + columns - 1) / columns) * (card + 72) + 24;
        label(canvas, "Following", (Rect){x, following_y, width, 30}, 21, true, 0xffffff);

        if (!view->profile.following_count) {
            label(canvas,
                  view->profile.permission_required
                      ? "Click 'Enable following permissions' above to authorize followed artists."
                      : "Artists you follow on Spotify will appear here.",
                  (Rect){x, following_y + 38, width, 24}, 13, false, 0xb3b3b3);
        }

        for (unsigned i = 0; i < view->profile.following_count && i < columns; ++i) {
            release_card(scene, canvas, (HitId)(HIT_PROFILE_ARTIST_FIRST + i), &view->profile.following[i],
                         true);
        }
    }
}
