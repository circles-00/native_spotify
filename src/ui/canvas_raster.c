#include "canvas_raster.h"

static float fast_sqrt(float squared, float hint) {
    if (squared <= 0) {
        return 0;
    }

    float guess = hint > 1 ? hint : 1;

    for (unsigned step = 0; step < 5; ++step) {
        guess = .5f * (guess + squared / guess);
    }

    return guess;
}

static void blend_pixel(uint32_t *pixel, uint32_t red, uint32_t green, uint32_t blue, float coverage) {
    if (coverage <= 0) {
        return;
    }

    if (coverage >= 1) {
        *pixel = (red << 16) | (green << 8) | blue;
        return;
    }

    uint32_t current = *pixel;
    uint32_t alpha = (uint32_t)(coverage * 255 + .5f);
    uint32_t out_r = (((current >> 16) & 255) * (255 - alpha) + red * alpha) / 255;
    uint32_t out_g = (((current >> 8) & 255) * (255 - alpha) + green * alpha) / 255;
    uint32_t out_b = ((current & 255) * (255 - alpha) + blue * alpha) / 255;
    *pixel = (out_r << 16) | (out_g << 8) | out_b;
}

static float round_rect_sdf(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float dx = (px > cx ? px - cx : cx - px) - hx;
    float dy = (py > cy ? py - cy : cy - py) - hy;

    if (dx > 0 && dy > 0) {
        return fast_sqrt(dx * dx + dy * dy, r) - r;
    }

    return (dx > dy ? dx : dy) - r;
}

static float clip_coverage(const RasterSurface *surface, float px, float py) {
    float coverage = 1.0f;

    for (unsigned i = 0; i < surface->round_clip_depth; ++i) {
        Rect c = surface->round_clips[i].rect;
        float r = surface->round_clips[i].radius * surface->scale;

        if (r <= 0) {
            continue;
        }

        float rw = c.w * surface->scale;
        float rh = c.h * surface->scale;

        if (r > rw * .5f) {
            r = rw * .5f;
        }

        if (r > rh * .5f) {
            r = rh * .5f;
        }

        float dist = round_rect_sdf(px, py, (c.x + c.w * .5f) * surface->scale,
                                    (c.y + c.h * .5f) * surface->scale, rw * .5f - r, rh * .5f - r, r);
        float c_cov = .5f - dist;

        if (c_cov < coverage) {
            coverage = c_cov;
        }

        if (coverage <= 0) {
            return 0;
        }
    }

    return coverage;
}

static void clamp_bounds(const RasterSurface *surface, int *left, int *top, int *right, int *bottom) {
    if (*left < surface->clip.left) {
        *left = surface->clip.left;
    }

    if (*top < surface->clip.top) {
        *top = surface->clip.top;
    }

    if (*right > surface->clip.right) {
        *right = surface->clip.right;
    }

    if (*bottom > surface->clip.bottom) {
        *bottom = surface->clip.bottom;
    }

    if (*left < 0) {
        *left = 0;
    }

    if (*top < 0) {
        *top = 0;
    }

    if (*right > surface->width) {
        *right = surface->width;
    }

    if (*bottom > surface->height) {
        *bottom = surface->height;
    }
}

void canvas_raster_round_rect(const RasterSurface *surface, Rect rect, float radius, Color top,
                              Color bottom) {
    if (!surface || !surface->pixels || rect.w <= 0 || rect.h <= 0) {
        return;
    }

    float rx = rect.x * surface->scale;
    float ry = rect.y * surface->scale;
    float rw = rect.w * surface->scale;
    float rh = rect.h * surface->scale;
    float r = radius * surface->scale;

    if (r < 0) {
        r = 0;
    }

    if (r > rw * .5f) {
        r = rw * .5f;
    }

    if (r > rh * .5f) {
        r = rh * .5f;
    }

    int left = (int)(rx - 1);
    int right = (int)(rx + rw + 2);
    int top_y = (int)(ry - 1);
    int bottom_y = (int)(ry + rh + 2);
    clamp_bounds(surface, &left, &top_y, &right, &bottom_y);

    float cx = rx + rw * .5f;
    float cy = ry + rh * .5f;
    float hx = rw * .5f - r;
    float hy = rh * .5f - r;

    for (int y = top_y; y < bottom_y; ++y) {
        float py = (float)y + .5f;
        float t = rh > 1 ? (py - ry) / rh : 0;
        Color row_color = top == bottom ? top : color_mix(top, bottom, t);
        uint32_t red = (row_color >> 16) & 255;
        uint32_t green = (row_color >> 8) & 255;
        uint32_t blue = row_color & 255;

        for (int x = left; x < right; ++x) {
            float px = (float)x + .5f;
            float distance = round_rect_sdf(px, py, cx, cy, hx, hy, r);
            float coverage = .5f - distance;

            if (coverage <= 0) {
                continue;
            }

            if (surface->round_clip_depth) {
                float c_cov = clip_coverage(surface, px, py);

                if (c_cov < coverage) {
                    coverage = c_cov;
                }
            }

            blend_pixel(&surface->pixels[y * surface->width + x], red, green, blue, coverage);
        }
    }
}

static float segment_dist_sq(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax;
    float vy = by - ay;
    float len_sq = vx * vx + vy * vy;
    float t = len_sq > 1e-4f ? ((px - ax) * vx + (py - ay) * vy) / len_sq : 0;

    if (t < 0) {
        t = 0;
    }

    if (t > 1) {
        t = 1;
    }

    float dx = px - (ax + t * vx);
    float dy = py - (ay + t * vy);
    return dx * dx + dy * dy;
}

void canvas_raster_path(const RasterSurface *surface, const Point *source, unsigned count, bool closed,
                        bool fill, float width, Color value) {
    if (!surface || !surface->pixels || !source || !count || count > 24) {
        return;
    }

    Point pts[25];
    float min_x = 1e9f;
    float min_y = 1e9f;
    float max_x = -1e9f;
    float max_y = -1e9f;

    for (unsigned i = 0; i < count; ++i) {
        pts[i] = (Point){source[i].x * surface->scale, source[i].y * surface->scale};

        if (pts[i].x < min_x) {
            min_x = pts[i].x;
        }

        if (pts[i].y < min_y) {
            min_y = pts[i].y;
        }

        if (pts[i].x > max_x) {
            max_x = pts[i].x;
        }

        if (pts[i].y > max_y) {
            max_y = pts[i].y;
        }
    }

    if (closed) {
        pts[count] = pts[0];
    }

    float scaled_width = width * surface->scale;
    float half = fill ? 0 : (scaled_width > 1.2f ? scaled_width * .5f : .6f);
    int left = (int)(min_x - half - 2);
    int right = (int)(max_x + half + 3);
    int top = (int)(min_y - half - 2);
    int bottom = (int)(max_y + half + 3);
    clamp_bounds(surface, &left, &top, &right, &bottom);

    uint32_t red = (value >> 16) & 255;
    uint32_t green = (value >> 8) & 255;
    uint32_t blue = value & 255;
    unsigned segments = closed || fill ? count : count - 1;

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            float px = (float)x + .5f;
            float py = (float)y + .5f;
            float min_sq = 1e9f;
            bool inside = false;

            for (unsigned i = 0; i < segments; ++i) {
                Point a = pts[i];
                Point b = pts[(i + 1) % count];
                float d_sq = segment_dist_sq(px, py, a.x, a.y, b.x, b.y);

                if (d_sq < min_sq) {
                    min_sq = d_sq;
                }

                if (fill && ((a.y > py) != (b.y > py)) &&
                    (px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x)) {
                    inside = !inside;
                }
            }

            float dist = fast_sqrt(min_sq, half + 1);
            float coverage = fill ? .5f + (inside ? dist : -dist) : half + .5f - dist;

            if (coverage <= 0) {
                continue;
            }

            if (surface->round_clip_depth) {
                float c_cov = clip_coverage(surface, px, py);

                if (c_cov < coverage) {
                    coverage = c_cov;
                }
            }

            blend_pixel(&surface->pixels[y * surface->width + x], red, green, blue, coverage);
        }
    }
}

void canvas_raster_stroke_circle(const RasterSurface *surface, Rect rect, float width, Color value) {
    if (!surface || !surface->pixels || rect.w <= 0 || rect.h <= 0) {
        return;
    }

    float cx = (rect.x + rect.w * .5f) * surface->scale;
    float cy = (rect.y + rect.h * .5f) * surface->scale;
    float radius = rect.w * .5f * surface->scale;
    float thickness = width * surface->scale > 1 ? width * surface->scale : 1;
    float half = thickness * .5f;

    int left = (int)(cx - radius - half - 1);
    int right = (int)(cx + radius + half + 2);
    int top = (int)(cy - radius - half - 1);
    int bottom = (int)(cy + radius + half + 2);
    clamp_bounds(surface, &left, &top, &right, &bottom);

    uint32_t red = (value >> 16) & 255;
    uint32_t green = (value >> 8) & 255;
    uint32_t blue = value & 255;

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            float dx = (float)x + .5f - cx;
            float dy = (float)y + .5f - cy;
            float distance = fast_sqrt(dx * dx + dy * dy, radius);
            float ring = distance > radius ? distance - radius : radius - distance;
            float coverage = half + .5f - ring;

            if (coverage <= 0) {
                continue;
            }

            if (surface->round_clip_depth) {
                float c_cov = clip_coverage(surface, (float)x + .5f, (float)y + .5f);

                if (c_cov < coverage) {
                    coverage = c_cov;
                }
            }

            blend_pixel(&surface->pixels[y * surface->width + x], red, green, blue, coverage);
        }
    }
}

void canvas_raster_image(const RasterSurface *surface, const Image *image, Rect rect) {
    if (!surface || !surface->pixels || !image || !image->pixels || !image->width || !image->height ||
        rect.w <= 0 || rect.h <= 0) {
        return;
    }

    float rx = rect.x * surface->scale;
    float ry = rect.y * surface->scale;
    float rw = rect.w * surface->scale;
    float rh = rect.h * surface->scale;

    if (rw <= 0 || rh <= 0) {
        return;
    }

    int left = (int)rx;
    int right = (int)(rx + rw + .5f);
    int top = (int)ry;
    int bottom = (int)(ry + rh + .5f);
    clamp_bounds(surface, &left, &top, &right, &bottom);

    for (int y = top; y < bottom; ++y) {
        float py = (float)y + .5f;
        float v = (py - ry) / rh * (float)image->height - .5f;
        int sy0 = (int)v;

        if (sy0 < 0) {
            sy0 = 0;
        }

        if ((unsigned)sy0 >= image->height) {
            sy0 = (int)image->height - 1;
        }

        int sy1 = (unsigned)(sy0 + 1) < image->height ? sy0 + 1 : sy0;
        float fy = v - (float)sy0;

        if (fy < 0) {
            fy = 0;
        }

        for (int x = left; x < right; ++x) {
            float px = (float)x + .5f;
            float cov = clip_coverage(surface, px, py);

            if (cov <= 0) {
                continue;
            }

            float u = (px - rx) / rw * (float)image->width - .5f;
            int sx0 = (int)u;

            if (sx0 < 0) {
                sx0 = 0;
            }

            if ((unsigned)sx0 >= image->width) {
                sx0 = (int)image->width - 1;
            }

            int sx1 = (unsigned)(sx0 + 1) < image->width ? sx0 + 1 : sx0;
            float fx = u - (float)sx0;

            if (fx < 0) {
                fx = 0;
            }

            uint32_t p00 = image->pixels[(unsigned)sy0 * image->width + (unsigned)sx0];
            uint32_t p10 = image->pixels[(unsigned)sy0 * image->width + (unsigned)sx1];
            uint32_t p01 = image->pixels[(unsigned)sy1 * image->width + (unsigned)sx0];
            uint32_t p11 = image->pixels[(unsigned)sy1 * image->width + (unsigned)sx1];

            float w00 = (1 - fx) * (1 - fy);
            float w10 = fx * (1 - fy);
            float w01 = (1 - fx) * fy;
            float w11 = fx * fy;

            uint32_t r = (uint32_t)(((p00 >> 16) & 255) * w00 + ((p10 >> 16) & 255) * w10 +
                                    ((p01 >> 16) & 255) * w01 + ((p11 >> 16) & 255) * w11 + .5f);
            uint32_t g = (uint32_t)(((p00 >> 8) & 255) * w00 + ((p10 >> 8) & 255) * w10 +
                                    ((p01 >> 8) & 255) * w01 + ((p11 >> 8) & 255) * w11 + .5f);
            uint32_t b = (uint32_t)((p00 & 255) * w00 + (p10 & 255) * w10 + (p01 & 255) * w01 +
                                    (p11 & 255) * w11 + .5f);
            blend_pixel(&surface->pixels[y * surface->width + x], r, g, b, cov);
        }
    }
}
