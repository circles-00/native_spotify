#include "canvas.h"

Color color_mix(Color from, Color to, float amount) {
    if (amount < 0) {
        amount = 0;
    }
    if (amount > 1) {
        amount = 1;
    }

    unsigned red =
        (unsigned)((float)((from >> 16) & 255) * (1 - amount) + (float)((to >> 16) & 255) * amount);
    unsigned green =
        (unsigned)((float)((from >> 8) & 255) * (1 - amount) + (float)((to >> 8) & 255) * amount);
    unsigned blue = (unsigned)((float)(from & 255) * (1 - amount) + (float)(to & 255) * amount);

    return (red << 16) | (green << 8) | blue;
}

static void path(Canvas *canvas, Rect rect, const Point *source, unsigned count, bool closed, bool fill,
                 Color color) {
    Point points[24];

    if (count > 24) {
        return;
    }

    for (unsigned i = 0; i < count; ++i) {
        points[i] = (Point){rect.x + source[i].x * rect.w / 24, rect.y + source[i].y * rect.h / 24};
    }

    canvas->path(canvas->context, points, count, closed, fill, rect.w / 12, color);
}

void canvas_icon(Canvas *canvas, Icon icon, Rect rect, Color color) {
    switch (icon) {
    case ICON_HOME:
        path(canvas, rect,
             (Point[]){{3, 10}, {12, 3}, {21, 10}, {21, 21}, {15, 21}, {15, 14}, {9, 14}, {9, 21}, {3, 21}},
             9, true, false, color);
        return;
    case ICON_SEARCH:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .12f, rect.y + rect.h * .12f, rect.w * .58f, rect.h * .58f},
                        false, rect.w / 12, color);
        path(canvas, rect, (Point[]){{16, 16}, {22, 22}}, 2, false, false, color);
        return;
    case ICON_LIBRARY:
        path(canvas, rect, (Point[]){{4, 3}, {4, 21}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{10, 3}, {10, 21}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{15, 4}, {21, 6}, {21, 21}, {15, 19}}, 4, true, false, color);
        return;
    case ICON_HEART:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w / 12, rect.y + rect.h / 8, rect.w * 5 / 12, rect.h * 5 / 12},
                        true, 1, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w / 2, rect.y + rect.h / 8, rect.w * 5 / 12, rect.h * 5 / 12},
                        true, 1, color);
        path(canvas, rect, (Point[]){{2, 8}, {12, 21}, {22, 8}}, 3, true, true, color);
        return;
    case ICON_ALBUM:
        canvas->ellipse(canvas->context, (Rect){rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2}, false,
                        rect.w / 12, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .38f, rect.y + rect.h * .38f, rect.w * .24f, rect.h * .24f},
                        true, 1, color);
        return;
    case ICON_PLAYLIST:
        path(canvas, rect, (Point[]){{3, 5}, {20, 5}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{3, 11}, {20, 11}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{3, 17}, {12, 17}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{18, 14}, {18, 22}, {22, 18}}, 3, false, false, color);
        return;
    case ICON_QUEUE:
        path(canvas, rect, (Point[]){{3, 4}, {21, 4}, {21, 10}, {3, 10}}, 4, true, false, color);
        path(canvas, rect, (Point[]){{3, 15}, {21, 15}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{3, 21}, {16, 21}}, 2, false, false, color);
        return;
    case ICON_PLAY:
        path(canvas, rect, (Point[]){{7, 3}, {21, 12}, {7, 21}}, 3, true, true, color);
        return;
    case ICON_PAUSE:
        canvas->box(canvas->context,
                    (Rect){rect.x + rect.w * .25f, rect.y + rect.h * .16f, rect.w * .17f, rect.h * .68f}, 0,
                    color);
        canvas->box(canvas->context,
                    (Rect){rect.x + rect.w * .58f, rect.y + rect.h * .16f, rect.w * .17f, rect.h * .68f}, 0,
                    color);
        return;
    case ICON_PREVIOUS:
    case ICON_NEXT: {
        bool previous = icon == ICON_PREVIOUS;
        path(canvas, rect,
             previous ? (Point[]){{18, 4}, {6, 12}, {18, 20}} : (Point[]){{6, 4}, {18, 12}, {6, 20}}, 3, true,
             true, color);
        path(canvas, rect, previous ? (Point[]){{5, 4}, {5, 20}} : (Point[]){{19, 4}, {19, 20}}, 2, false,
             false, color);
        return;
    }
    case ICON_VOLUME:
        path(canvas, rect, (Point[]){{3, 9}, {7, 9}, {13, 4}, {13, 20}, {7, 15}, {3, 15}}, 6, true, false,
             color);
        path(canvas, rect, (Point[]){{17, 7}, {20, 10}, {20, 14}, {17, 17}}, 4, false, false, color);
        return;
    case ICON_DEVICE:
        path(canvas, rect, (Point[]){{3, 3}, {21, 3}, {21, 16}, {3, 16}}, 4, true, false, color);
        path(canvas, rect, (Point[]){{12, 16}, {12, 21}, {7, 21}, {17, 21}}, 4, false, false, color);
        return;
    case ICON_CHEVRON_LEFT:
    case ICON_CHEVRON_RIGHT:
        path(canvas, rect,
             icon == ICON_CHEVRON_LEFT ? (Point[]){{15, 4}, {7, 12}, {15, 20}}
                                       : (Point[]){{9, 4}, {17, 12}, {9, 20}},
             3, false, false, color);
        return;
    case ICON_REFRESH:
        path(canvas, rect, (Point[]){{21, 8}, {17, 3}, {8, 3}, {3, 8}, {3, 16}, {8, 21}, {17, 21}, {21, 16}},
             8, false, false, color);
        path(canvas, rect, (Point[]){{15, 8}, {21, 8}, {21, 2}}, 3, false, false, color);
        return;
    case ICON_LINK:
        path(canvas, rect, (Point[]){{9, 3}, {21, 3}, {21, 15}}, 3, false, false, color);
        path(canvas, rect, (Point[]){{21, 3}, {8, 16}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{4, 8}, {4, 21}, {17, 21}}, 3, false, false, color);
        return;
    case ICON_MORE:
        for (unsigned i = 0; i < 3; ++i) {
            canvas->ellipse(canvas->context,
                            (Rect){rect.x + rect.w * (.12f + .32f * i), rect.y + rect.h * .43f, rect.w * .12f,
                                   rect.h * .12f},
                            true, 1, color);
        }
        return;
    case ICON_CLOCK:
        canvas->ellipse(canvas->context, (Rect){rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4}, false, 1.5f,
                        color);
        path(canvas, rect, (Point[]){{12, 6}, {12, 12}, {16, 14}}, 3, false, false, color);
        return;
    case ICON_CHECK:
        path(canvas, rect, (Point[]){{4, 12}, {10, 18}, {21, 5}}, 3, false, false, color);
        return;
    case ICON_PLUS:
        path(canvas, rect, (Point[]){{12, 4}, {12, 20}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{4, 12}, {20, 12}}, 2, false, false, color);
        return;
    case ICON_CLOSE:
        path(canvas, rect, (Point[]){{5, 5}, {19, 19}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{19, 5}, {5, 19}}, 2, false, false, color);
        return;
    case ICON_SHUFFLE:
        path(canvas, rect, (Point[]){{3, 5}, {7, 5}, {17, 19}, {21, 19}}, 4, false, false, color);
        path(canvas, rect, (Point[]){{3, 19}, {7, 19}, {17, 5}, {21, 5}}, 4, false, false, color);
        path(canvas, rect, (Point[]){{18, 2}, {21, 5}, {18, 8}}, 3, false, false, color);
        path(canvas, rect, (Point[]){{18, 16}, {21, 19}, {18, 22}}, 3, false, false, color);
        return;
    case ICON_REPEAT:
        path(canvas, rect, (Point[]){{4, 14}, {4, 7}, {7, 4}, {20, 4}, {17, 1}}, 5, false, false, color);
        path(canvas, rect, (Point[]){{20, 10}, {20, 17}, {17, 20}, {4, 20}, {7, 23}}, 5, false, false, color);
        path(canvas, rect, (Point[]){{17, 7}, {20, 4}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{7, 17}, {4, 20}}, 2, false, false, color);
        return;
    case ICON_FRIENDS:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .1f, rect.y + rect.h * .12f, rect.w * .3f, rect.h * .3f},
                        false, rect.w / 12, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .6f, rect.y + rect.h * .12f, rect.w * .3f, rect.h * .3f},
                        false, rect.w / 12, color);
        path(canvas, rect, (Point[]){{2, 21}, {2, 16}, {5, 13}, {8, 13}, {11, 16}, {11, 21}}, 6, false, false,
             color);
        path(canvas, rect, (Point[]){{13, 21}, {13, 16}, {16, 13}, {19, 13}, {22, 16}, {22, 21}}, 6, false,
             false, color);
        return;
    case ICON_USER:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .32f, rect.y + rect.h * .15f, rect.w * .36f, rect.h * .36f},
                        false, rect.w / 12, color);
        path(canvas, rect, (Point[]){{4, 21}, {5, 17}, {8, 14}, {16, 14}, {19, 17}, {20, 21}}, 6, false,
             false, color);
        return;
    case ICON_LYRICS:
        path(canvas, rect, (Point[]){{4, 4}, {20, 4}, {20, 16}, {10, 16}, {4, 21}, {4, 4}}, 6, false, false,
             color);
        path(canvas, rect, (Point[]){{8, 8}, {16, 8}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{8, 12}, {14, 12}}, 2, false, false, color);
        return;
    }
}
