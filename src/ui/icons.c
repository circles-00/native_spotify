#include "canvas.h"

#include <math.h>

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

static float stroke_weight(Rect rect) {
    float width = rect.w / 13.5f;

    return width < 1.35f ? 1.35f : width;
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

    canvas->path(canvas->context, points, count, closed, fill, stroke_weight(rect), color);
}

void canvas_song_play(Canvas *canvas, Rect art, float radius, float hover, bool playable) {
    if (!playable || hover <= .05f) {
        return;
    }

    unsigned opacity = (unsigned)(160 * (hover < 1 ? hover : 1));
    float size = art.w >= 64 ? 32 : 23;
    Rect icon = {art.x + (art.w - size) / 2 - size / 12, art.y + (art.h - size) / 2, size, size};
    canvas->box(canvas->context, art, radius, opacity << 24);
    canvas_icon(canvas, ICON_PLAY, icon, 0xffffff);
}

void canvas_icon(Canvas *canvas, Icon icon, Rect rect, Color color) {
    float weight = stroke_weight(rect);

    switch (icon) {
    case ICON_HOME:
        path(canvas, rect,
             (Point[]){{3.5f, 10},
                       {12, 3.5f},
                       {20.5f, 10},
                       {20.5f, 20.5f},
                       {14.5f, 20.5f},
                       {14.5f, 14},
                       {9.5f, 14},
                       {9.5f, 20.5f},
                       {3.5f, 20.5f}},
             9, true, false, color);
        return;

    case ICON_SEARCH:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .13f, rect.y + rect.h * .13f, rect.w * .56f, rect.h * .56f},
                        false, weight, color);
        path(canvas, rect, (Point[]){{15.5f, 15.5f}, {21.5f, 21.5f}}, 2, false, false, color);
        return;

    case ICON_LIBRARY:
        path(canvas, rect, (Point[]){{4.5f, 3.5f}, {4.5f, 20.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{10, 3.5f}, {10, 20.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{14.5f, 4.5f}, {20.5f, 6.5f}, {20.5f, 20.5f}, {14.5f, 18.5f}}, 4, true,
             false, color);
        return;

    case ICON_HEART:
    case ICON_HEART_OUTLINE:
        path(canvas, rect,
             (Point[]){{12, 21}, {3.8f, 13.2f}, {2.3f, 10.9f}, {2.2f, 8}, {3.4f, 5.6f},
                       {5.7f, 4.1f}, {8.3f, 4.1f}, {10.4f, 5.1f}, {12, 7}, {13.6f, 5.1f},
                       {15.7f, 4.1f}, {18.3f, 4.1f}, {20.6f, 5.6f}, {21.8f, 8}, {21.7f, 10.9f},
                       {20.2f, 13.2f}},
             16, true, icon == ICON_HEART, color);
        return;

    case ICON_ALBUM:
        canvas->ellipse(canvas->context, (Rect){rect.x + 1.5f, rect.y + 1.5f, rect.w - 3, rect.h - 3}, false,
                        weight, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .38f, rect.y + rect.h * .38f, rect.w * .24f, rect.h * .24f},
                        true, 1, color);
        return;

    case ICON_PLAYLIST:
        path(canvas, rect, (Point[]){{3.5f, 5.5f}, {19.5f, 5.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{3.5f, 11.5f}, {19.5f, 11.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{3.5f, 17.5f}, {12.5f, 17.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{17.5f, 19}, {17.5f, 13.5f}, {21, 14.5f}}, 3, false, false, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .59f, rect.y + rect.h * .72f, rect.w * .17f, rect.h * .17f},
                        true, 1, color);
        return;

    case ICON_QUEUE:
        path(canvas, rect, (Point[]){{3.5f, 4.5f}, {20.5f, 4.5f}, {20.5f, 10}, {3.5f, 10}}, 4, true, false,
             color);
        path(canvas, rect, (Point[]){{3.5f, 15}, {20.5f, 15}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{3.5f, 20}, {16, 20}}, 2, false, false, color);
        return;

    case ICON_PLAY:
        path(canvas, rect, (Point[]){{7.5f, 3.5f}, {20.5f, 12}, {7.5f, 20.5f}}, 3, true, true, color);
        return;

    case ICON_RADIO:
        path(canvas, rect, (Point[]){{4.8f, 4.8f}, {3.1f, 7}, {2.1f, 9.5f}, {2, 12}, {2.1f, 14.5f},
                                     {3.1f, 17}, {4.8f, 19.2f}}, 7, false, false, color);
        path(canvas, rect, (Point[]){{19.2f, 4.8f}, {20.9f, 7}, {21.9f, 9.5f}, {22, 12}, {21.9f, 14.5f},
                                     {20.9f, 17}, {19.2f, 19.2f}}, 7, false, false, color);
        path(canvas, rect, (Point[]){{8, 8}, {6.8f, 9.8f}, {6.4f, 12}, {6.8f, 14.2f}, {8, 16}}, 5, false,
             false, color);
        path(canvas, rect, (Point[]){{16, 8}, {17.2f, 9.8f}, {17.6f, 12}, {17.2f, 14.2f}, {16, 16}}, 5,
             false, false, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .415f, rect.y + rect.h * .415f, rect.w * .17f, rect.h * .17f},
                        true, 1, color);
        return;

    case ICON_LOGIN:
    case ICON_LOGOUT:
        path(canvas, rect, (Point[]){{11, 4}, {4, 4}, {4, 20}, {11, 20}}, 4, false, false, color);
        path(canvas, rect, (Point[]){{9, 12}, {21, 12}}, 2, false, false, color);
        path(canvas, rect, icon == ICON_LOGIN ? (Point[]){{13, 8}, {9, 12}, {13, 16}}
                                             : (Point[]){{17, 8}, {21, 12}, {17, 16}},
             3, false, false, color);
        return;

    case ICON_SPEAKER:
        path(canvas, rect, (Point[]){{5.5f, 2.5f}, {18.5f, 2.5f}, {18.5f, 21.5f}, {5.5f, 21.5f}}, 4, true, false,
             color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .435f, rect.y + rect.h * .21f, rect.w * .13f, rect.h * .13f},
                        true, 1, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .33f, rect.y + rect.h * .47f, rect.w * .34f, rect.h * .34f},
                        false, weight, color);
        return;

    case ICON_PHONE:
        path(canvas, rect, (Point[]){{7, 2.5f}, {17, 2.5f}, {17, 21.5f}, {7, 21.5f}}, 4, true, false, color);
        path(canvas, rect, (Point[]){{10.5f, 18}, {13.5f, 18}}, 2, false, false, color);
        return;

    case ICON_MUTE:
        path(canvas, rect, (Point[]){{3.5f, 9}, {7.5f, 9}, {13, 4.5f}, {13, 19.5f}, {7.5f, 15}, {3.5f, 15}},
             6, true, false, color);
        path(canvas, rect, (Point[]){{16, 9.5f}, {21, 14.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{21, 9.5f}, {16, 14.5f}}, 2, false, false, color);
        return;

    case ICON_EXTERNAL_LINK:
        path(canvas, rect, (Point[]){{11, 5}, {4, 5}, {4, 20}, {19, 20}, {19, 13}}, 5, false, false, color);
        path(canvas, rect, (Point[]){{14, 4}, {20, 4}, {20, 10}}, 3, false, false, color);
        path(canvas, rect, (Point[]){{12, 12}, {20, 4}}, 2, false, false, color);
        return;

    case ICON_PAUSE:
        canvas->box(canvas->context,
                    (Rect){rect.x + rect.w * .25f, rect.y + rect.h * .18f, rect.w * .17f, rect.h * .64f}, 1,
                    color);
        canvas->box(canvas->context,
                    (Rect){rect.x + rect.w * .58f, rect.y + rect.h * .18f, rect.w * .17f, rect.h * .64f}, 1,
                    color);
        return;

    case ICON_PREVIOUS:
    case ICON_NEXT: {
        bool previous = icon == ICON_PREVIOUS;

        path(canvas, rect,
             previous ? (Point[]){{18, 5}, {7.5f, 12}, {18, 19}} : (Point[]){{6, 5}, {16.5f, 12}, {6, 19}}, 3,
             true, true, color);
        path(canvas, rect, previous ? (Point[]){{5.5f, 5}, {5.5f, 19}} : (Point[]){{18.5f, 5}, {18.5f, 19}},
             2, false, false, color);
        return;
    }

    case ICON_VOLUME:
        path(canvas, rect, (Point[]){{3.5f, 9}, {7.5f, 9}, {13, 4.5f}, {13, 19.5f}, {7.5f, 15}, {3.5f, 15}},
             6, true, false, color);
        path(canvas, rect, (Point[]){{16.5f, 8}, {19.5f, 10.5f}, {19.5f, 13.5f}, {16.5f, 16}}, 4, false,
             false, color);
        return;

    case ICON_DEVICE:
        path(canvas, rect, (Point[]){{3.5f, 4}, {20.5f, 4}, {20.5f, 16}, {3.5f, 16}}, 4, true, false, color);
        path(canvas, rect, (Point[]){{12, 16}, {12, 20.5f}, {7.5f, 20.5f}, {16.5f, 20.5f}}, 4, false, false,
             color);
        return;

    case ICON_CHEVRON_LEFT:
    case ICON_CHEVRON_RIGHT:
        path(canvas, rect,
             icon == ICON_CHEVRON_LEFT ? (Point[]){{15, 4.5f}, {7.5f, 12}, {15, 19.5f}}
                                       : (Point[]){{9, 4.5f}, {16.5f, 12}, {9, 19.5f}},
             3, false, false, color);
        return;

    case ICON_REFRESH:
        path(canvas, rect,
             (Point[]){{20.5f, 8},
                       {16.5f, 3.5f},
                       {8, 3.5f},
                       {3.5f, 8},
                       {3.5f, 16},
                       {8, 20.5f},
                       {16.5f, 20.5f},
                       {20.5f, 16}},
             8, false, false, color);
        path(canvas, rect, (Point[]){{15.5f, 8}, {20.5f, 8}, {20.5f, 3}}, 3, false, false, color);
        return;

    case ICON_LINK:
        path(canvas, rect, (Point[]){{10, 4}, {20, 4}, {20, 14}}, 3, false, false, color);
        path(canvas, rect, (Point[]){{20, 4}, {9, 15}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{4.5f, 8.5f}, {4.5f, 19.5f}, {15.5f, 19.5f}}, 3, false, false, color);
        return;

    case ICON_MORE:
        for (unsigned i = 0; i < 3; ++i) {
            canvas->ellipse(canvas->context,
                            (Rect){rect.x + rect.w * (.14f + .30f * i), rect.y + rect.h * .44f, rect.w * .12f,
                                   rect.h * .12f},
                            true, 1, color);
        }

        return;

    case ICON_CLOCK:
        canvas->ellipse(canvas->context, (Rect){rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4}, false,
                        weight, color);
        path(canvas, rect, (Point[]){{12, 7}, {12, 12}, {15.5f, 14}}, 3, false, false, color);
        return;

    case ICON_CHECK:
        path(canvas, rect, (Point[]){{4.5f, 12.5f}, {10, 18}, {20, 6}}, 3, false, false, color);
        return;

    case ICON_PLUS:
        path(canvas, rect, (Point[]){{12, 4.5f}, {12, 19.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{4.5f, 12}, {19.5f, 12}}, 2, false, false, color);
        return;

    case ICON_CLOSE:
        path(canvas, rect, (Point[]){{5.5f, 5.5f}, {18.5f, 18.5f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{18.5f, 5.5f}, {5.5f, 18.5f}}, 2, false, false, color);
        return;

    case ICON_SHUFFLE:
        path(canvas, rect, (Point[]){{3.5f, 6.5f}, {7.5f, 6.5f}, {16.5f, 17.5f}, {20.5f, 17.5f}}, 4, false,
             false, color);
        path(canvas, rect, (Point[]){{3.5f, 17.5f}, {7.5f, 17.5f}, {16.5f, 6.5f}, {20.5f, 6.5f}}, 4, false,
             false, color);
        path(canvas, rect, (Point[]){{18, 4}, {20.5f, 6.5f}, {18, 9}}, 3, false, false, color);
        path(canvas, rect, (Point[]){{18, 15}, {20.5f, 17.5f}, {18, 20}}, 3, false, false, color);
        return;

    case ICON_REPEAT:
        path(canvas, rect, (Point[]){{4, 13}, {4, 7.5f}, {6.5f, 5}, {19.5f, 5}}, 4, false, false, color);
        path(canvas, rect, (Point[]){{17, 2.5f}, {19.5f, 5}, {17, 7.5f}}, 3, false, false, color);
        path(canvas, rect, (Point[]){{20, 11}, {20, 16.5f}, {17.5f, 19}, {4.5f, 19}}, 4, false, false, color);
        path(canvas, rect, (Point[]){{7, 16.5f}, {4.5f, 19}, {7, 21.5f}}, 3, false, false, color);
        return;

    case ICON_FRIENDS:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .1f, rect.y + rect.h * .12f, rect.w * .3f, rect.h * .3f},
                        false, weight, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .6f, rect.y + rect.h * .12f, rect.w * .3f, rect.h * .3f},
                        false, weight, color);
        path(canvas, rect, (Point[]){{2, 21}, {2, 16}, {5, 13}, {8, 13}, {11, 16}, {11, 21}}, 6, false, false,
             color);
        path(canvas, rect, (Point[]){{13, 21}, {13, 16}, {16, 13}, {19, 13}, {22, 16}, {22, 21}}, 6, false,
             false, color);
        return;

    case ICON_USER:
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .32f, rect.y + rect.h * .15f, rect.w * .36f, rect.h * .36f},
                        false, weight, color);
        path(canvas, rect, (Point[]){{4, 21}, {5, 17}, {8, 14}, {16, 14}, {19, 17}, {20, 21}}, 6, false,
             false, color);
        return;

    case ICON_SETTINGS:
        path(canvas, rect, (Point[]){{10.14f, 5.05f},  {10.35f, 2.14f},  {13.65f, 2.14f},  {13.86f, 5.05f},
                                     {17.09f, 6.91f},  {19.72f, 5.64f},  {21.37f, 8.50f},  {18.95f, 10.14f},
                                     {18.95f, 13.86f}, {21.37f, 15.50f}, {19.72f, 18.36f}, {17.09f, 17.09f},
                                     {13.86f, 18.95f}, {13.65f, 21.86f}, {10.35f, 21.86f}, {10.14f, 18.95f},
                                     {6.91f, 17.09f},  {4.28f, 18.36f},  {2.63f, 15.50f},  {5.05f, 13.86f},
                                     {5.05f, 10.14f},  {2.63f, 8.50f},   {4.28f, 5.64f},   {6.91f, 6.91f}},
             24, true, false, color);
        canvas->ellipse(canvas->context,
                        (Rect){rect.x + rect.w * .37f, rect.y + rect.h * .37f, rect.w * .26f, rect.h * .26f},
                        false, weight, color);
        return;

    case ICON_LYRICS:
        canvas->ellipse(
            canvas->context,
            (Rect){rect.x + rect.w * .475f, rect.y + rect.h * .142f, rect.w * .383f, rect.h * .383f}, false,
            weight, color);
        path(canvas, rect, (Point[]){{12.75f, 4.75f}, {19.25f, 11.25f}}, 2, false, false, color);
        path(canvas, rect, (Point[]){{11.46f, 8.72f}, {3.70f, 17.70f}, {6.30f, 20.30f}, {15.28f, 12.54f}}, 4,
             false, false, color);
        return;
    case ICON_EXPAND:
    case ICON_COLLAPSE:
        for (int y = -1; y <= 1; y += 2) {
            for (int x = -1; x <= 1; x += 2) {
                float corner = icon == ICON_EXPAND ? 8 : 3;
                float edge = icon == ICON_EXPAND ? 3 : 8;
                path(canvas, rect,
                     (Point[]){{12 + x * edge, 12 + y * corner},
                               {12 + x * corner, 12 + y * corner},
                               {12 + x * corner, 12 + y * edge}},
                     3, false, false, color);
            }
        }
        return;
    }
}

typedef struct {
    float cy;
    float radius;
    float span;
    float half_width;
} BrandArc;

static const BrandArc brand_arcs[] = {
    {1.13f, 1.34f, 0.44f, 0.082f},
    {1.08f, 1.04f, 0.43f, 0.068f},
    {1.04f, 0.76f, 0.41f, 0.056f},
};

void canvas_brand_icon_pixels(unsigned size, uint32_t *argb) {
    if (!size || !argb) {
        return;
    }

    float edge = 2.0f / (float)size;
    float ca = cosf(0.12f);
    float sa = sinf(0.12f);

    for (unsigned y = 0; y < size; ++y) {
        float ny = (((float)y + 0.5f) / (float)size) * 2.0f - 1.0f;

        for (unsigned x = 0; x < size; ++x) {
            float nx = (((float)x + 0.5f) / (float)size) * 2.0f - 1.0f;
            float r = sqrtf(nx * nx + ny * ny);
            float alpha = (0.92f - r) / edge + 0.5f;

            if (alpha <= 0.0f) {
                argb[y * size + x] = 0;
                continue;
            }

            if (alpha > 1.0f) {
                alpha = 1.0f;
            }

            float rx = nx * ca + ny * sa;
            float ry = -nx * sa + ny * ca;
            float stripe = 0.0f;

            for (unsigned i = 0; i < sizeof(brand_arcs) / sizeof(brand_arcs[0]); ++i) {
                BrandArc arc = brand_arcs[i];
                float dx = rx;
                float dy = ry - arc.cy;
                float theta = atan2f(dx, -dy);
                float d;

                if (fabsf(theta) <= arc.span) {
                    d = fabsf(sqrtf(dx * dx + dy * dy) - arc.radius);
                } else {
                    float end = theta > 0.0f ? arc.span : -arc.span;
                    float ex = arc.radius * sinf(end);
                    float ey = arc.cy - arc.radius * cosf(end);
                    float gx = rx - ex;
                    float gy = ry - ey;
                    d = sqrtf(gx * gx + gy * gy);
                }

                float cov = (arc.half_width - d) / edge + 0.5f;

                if (cov > 1.0f) {
                    cov = 1.0f;
                }

                if (cov > stripe) {
                    stripe = cov;
                }
            }

            unsigned rr = (unsigned)(0x7cu * (1.0f - stripe) + 255.0f * stripe + 0.5f);
            unsigned rg = (unsigned)(0x3au * (1.0f - stripe) + 255.0f * stripe + 0.5f);
            unsigned rb = (unsigned)(0xedu * (1.0f - stripe) + 255.0f * stripe + 0.5f);
            unsigned ra = (unsigned)(alpha * 255.0f + 0.5f);
            argb[y * size + x] = ((uint32_t)ra << 24) | ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | rb;
        }
    }
}

void canvas_brand_logo(Canvas *canvas, Rect rect) {
    if (!canvas || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    float side = rect.w < rect.h ? rect.w : rect.h;
    float cx = rect.x + rect.w * 0.5f;
    float cy = rect.y + rect.h * 0.5f;
    float half = side * 0.46f;
    Rect circle = {cx - half, cy - half, half * 2.0f, half * 2.0f};
    canvas->ellipse(canvas->context, circle, true, 0.0f, 0x7c3aed);

    float ca = cosf(-0.12f);
    float sa = sinf(-0.12f);

    for (unsigned i = 0; i < sizeof(brand_arcs) / sizeof(brand_arcs[0]); ++i) {
        BrandArc arc = brand_arcs[i];
        Point points[13];

        for (unsigned j = 0; j < 13; ++j) {
            float t = -arc.span + (2.0f * arc.span * (float)j) / 12.0f;
            float rx = arc.radius * sinf(t);
            float ry = arc.cy - arc.radius * cosf(t);
            float nx = rx * ca + ry * sa;
            float ny = -rx * sa + ry * ca;
            points[j] = (Point){cx + nx * (side * 0.5f), cy + ny * (side * 0.5f)};
        }

        canvas->path(canvas->context, points, 13, false, false, arc.half_width * side, 0xffffff);
    }
}
