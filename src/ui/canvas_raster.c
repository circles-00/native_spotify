#include "canvas_raster.h"

static int floor_int(float value) {
    if (value <= -1e6f) {
        return -1000000;
    }

    if (value >= 1e6f) {
        return 1000000;
    }

    int whole = (int)value;

    if ((float)whole > value) {
        --whole;
    }

    return whole;
}

static int ceil_int(float value) {
    if (value <= -1e6f) {
        return -1000000;
    }

    if (value >= 1e6f) {
        return 1000000;
    }

    int whole = (int)value;

    if ((float)whole < value) {
        ++whole;
    }

    return whole;
}

static float fast_sqrt(float squared, float hint) {
    if (!(squared > 0.0f)) {
        return 0.0f;
    }

    if (squared > 1e30f) {
        return 1e15f;
    }

    float guess = hint > 1.0f ? hint : 1.0f;

    for (unsigned step = 0; step < 32 && guess * guess > squared * 4.0f; ++step) {
        guess *= 0.5f;
    }

    for (unsigned step = 0; step < 32 && guess * guess < squared * 0.25f; ++step) {
        guess *= 2.0f;
    }

    for (unsigned step = 0; step < 5; ++step) {
        guess = 0.5f * (guess + squared / guess);
    }

    return guess;
}

static void blend_pixel(uint32_t *pixel, uint32_t red, uint32_t green, uint32_t blue, float coverage) {
    if (coverage <= 0.0f) {
        return;
    }

    if (coverage >= 1.0f) {
        *pixel = (red << 16) | (green << 8) | blue;
        return;
    }

    uint32_t current = *pixel;
    uint32_t alpha = (uint32_t)(coverage * 255.0f + 0.5f);
    uint32_t inv = 255 - alpha;
    uint32_t out_r = (((current >> 16) & 255) * inv + red * alpha + 127) / 255;
    uint32_t out_g = (((current >> 8) & 255) * inv + green * alpha + 127) / 255;
    uint32_t out_b = ((current & 255) * inv + blue * alpha + 127) / 255;
    *pixel = (out_r << 16) | (out_g << 8) | out_b;
}

static float interval_overlap(float x, float y, float a, float b) {
    float left = x > a ? x : a;
    float right = y < b ? y : b;
    return right > left ? right - left : 0.0f;
}

static float round_rect_coverage(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float dx = px > cx ? px - cx : cx - px;
    float dy = py > cy ? py - cy : cy - py;

    if (dx - 0.5f >= hx + r || dy - 0.5f >= hy + r) {
        return 0.0f;
    }

    if (r == 0.0f || dx + 0.5f <= hx || dy + 0.5f <= hy) {
        return interval_overlap(px - 0.5f, px + 0.5f, cx - hx - r, cx + hx + r) *
               interval_overlap(py - 0.5f, py + 0.5f, cy - hy - r, cy + hy + r);
    }

    float far_x = dx + 0.5f - hx;
    float far_y = dy + 0.5f - hy;
    float r2 = r * r;

    if (far_x * far_x + far_y * far_y <= r2) {
        return 1.0f;
    }

    float near_x = far_x > 1.0f ? far_x - 1.0f : 0.0f;
    float near_y = far_y > 1.0f ? far_y - 1.0f : 0.0f;

    if (near_x * near_x + near_y * near_y >= r2) {
        return 0.0f;
    }

    // Sample only at the arc; integrate horizontal spans so thin edge pixels aren't quantized to steps.
    float top = py - 0.5f > cy - hy - r ? py - 0.5f : cy - hy - r;
    float bottom = py + 0.5f < cy + hy + r ? py + 0.5f : cy + hy + r;
    float stripe = (bottom - top) / 16.0f;
    float area = 0.0f;

    for (unsigned i = 0; i < 16; ++i) {
        float y = top + ((float)i + 0.5f) * stripe - cy;
        float edge = (y < 0 ? -y : y) - hy;
        float half_width = hx + (edge > 0.0f ? fast_sqrt(r2 - edge * edge, r) : r);
        area += interval_overlap(px - 0.5f, px + 0.5f, cx - half_width, cx + half_width);
    }

    return area * stripe;
}

typedef struct {
    float cx, cy, hx, hy, radius;
    float aspect;
} RoundShape;

static RoundShape round_shape(Rect rect, float radius, float scale) {
    float r = canvas_corner_radius(rect, radius);
    return (RoundShape){(rect.x + rect.w * 0.5f) * scale,
                        (rect.y + rect.h * 0.5f) * scale,
                        (rect.w * 0.5f - r) * scale,
                        (rect.h * 0.5f - r) * scale,
                        r * scale,
                        1.0f};
}

static float span_half_width(RoundShape shape, float y) {
    float dy = y - shape.cy;
    float edge = (dy < 0 ? -dy : dy) - shape.hy;
    float radius = shape.radius;
    return shape.hx +
           shape.aspect * (edge > 0.0f ? fast_sqrt(radius * radius - edge * edge, radius) : radius);
}

static float shape_coverage(RoundShape shape, float x, float y) {
    if (shape.aspect == 1.0f) {
        return round_rect_coverage(x, y, shape.cx, shape.cy, shape.hx, shape.hy, shape.radius);
    }

    float top = shape.cy - shape.radius;
    float bottom = shape.cy + shape.radius;
    top = top > y - 0.5f ? top : y - 0.5f;
    bottom = bottom < y + 0.5f ? bottom : y + 0.5f;

    if (top >= bottom) {
        return 0.0f;
    }

    float step = (bottom - top) / 16.0f;
    float area = 0.0f;

    for (unsigned row = 0; row < 16; ++row) {
        float half = span_half_width(shape, top + ((float)row + 0.5f) * step);
        area += interval_overlap(x - 0.5f, x + 0.5f, shape.cx - half, shape.cx + half);
    }

    return area * step;
}

static float masked_coverage(const RasterSurface *surface, const RoundShape *shape, float px, float py) {
    float coverage = shape ? shape_coverage(*shape, px, py) : 1.0f;

    if (!surface->round_clip_depth || coverage <= 0.0f) {
        return coverage;
    }

    RoundShape partials[RASTER_CLIP_LIMIT + 1];
    unsigned count = 0;

    if (shape && coverage < 1.0f) {
        partials[count++] = *shape;
    }

    for (unsigned i = 0; i < surface->round_clip_depth && i < RASTER_CLIP_LIMIT; ++i) {
        const RasterRoundClip *clip = &surface->round_clips[i];

        if (clip->rect.w <= 0.0f || clip->rect.h <= 0.0f) {
            return 0.0f;
        }

        RoundShape mask = round_shape(clip->rect, clip->radius, surface->scale);
        float c_cov = shape_coverage(mask, px, py);

        if (c_cov <= 0.0f) {
            return 0.0f;
        }

        if (c_cov < 1.0f) {
            coverage = c_cov;
            partials[count++] = mask;
        }
    }

    if (count < 2) {
        return coverage;
    }

    // Intersect the subpixel spans, not the averaged alphas of separate edges.
    float top = py - 0.5f;
    float bottom = py + 0.5f;

    for (unsigned i = 0; i < count; ++i) {
        RoundShape m = partials[i];
        float first = m.cy - m.hy - m.radius;
        float last = m.cy + m.hy + m.radius;
        top = top > first ? top : first;
        bottom = bottom < last ? bottom : last;
    }

    if (top >= bottom) {
        return 0.0f;
    }

    float step = (bottom - top) / 16.0f;
    float area = 0.0f;

    for (unsigned row = 0; row < 16; ++row) {
        float y = top + ((float)row + 0.5f) * step;
        float left = px - 0.5f;
        float right = px + 0.5f;

        for (unsigned i = 0; i < count; ++i) {
            RoundShape m = partials[i];
            float half = span_half_width(m, y);
            left = left > m.cx - half ? left : m.cx - half;
            right = right < m.cx + half ? right : m.cx + half;
        }

        if (left < right) {
            area += right - left;
        }
    }

    return area * step;
}

static float clip_coverage(const RasterSurface *surface, float px, float py) {
    return masked_coverage(surface, NULL, px, py);
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
    if (!surface || !surface->pixels || surface->scale <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    float rx = rect.x * surface->scale;
    float ry = rect.y * surface->scale;
    float rw = rect.w * surface->scale;
    float rh = rect.h * surface->scale;
    RoundShape shape = round_shape(rect, radius, surface->scale);

    int left = floor_int(rx - 0.5f);
    int right = ceil_int(rx + rw + 0.5f);
    int top_y = floor_int(ry - 0.5f);
    int bottom_y = ceil_int(ry + rh + 0.5f);
    clamp_bounds(surface, &left, &top_y, &right, &bottom_y);
    float opacity = top == bottom && top >> 24 ? (float)(top >> 24) / 255.0f : 1.0f;

    for (int y = top_y; y < bottom_y; ++y) {
        float py = (float)y + 0.5f;
        float t = rh > 1.0f ? (py - ry) / rh : 0.0f;
        Color row_color = top == bottom ? top : color_mix(top, bottom, t);
        uint32_t red = (row_color >> 16) & 255;
        uint32_t green = (row_color >> 8) & 255;
        uint32_t blue = row_color & 255;

        for (int x = left; x < right; ++x) {
            float px = (float)x + 0.5f;
            float coverage = masked_coverage(surface, &shape, px, py);

            if (coverage <= 0.0f) {
                continue;
            }

            blend_pixel(&surface->pixels[y * surface->width + x], red, green, blue, coverage * opacity);
        }
    }
}

static float segment_dist_sq(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax;
    float vy = by - ay;
    float len_sq = vx * vx + vy * vy;
    float t = len_sq > 1e-4f ? ((px - ax) * vx + (py - ay) * vy) / len_sq : 0.0f;

    if (t < 0.0f) {
        t = 0.0f;
    }

    if (t > 1.0f) {
        t = 1.0f;
    }

    float dx = px - (ax + t * vx);
    float dy = py - (ay + t * vy);
    return dx * dx + dy * dy;
}

void canvas_raster_path(const RasterSurface *surface, const Point *source, unsigned count, bool closed,
                        bool fill, float width, Color value) {
    if (!surface || !surface->pixels || surface->scale <= 0.0f || !source || !count || count > 24) {
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
    float half = fill ? 0.0f : (scaled_width > 1.2f ? scaled_width * 0.5f : 0.6f);
    float edge_limit = half + 0.6f;
    float edge_limit_sq = edge_limit * edge_limit;

    int left = floor_int(min_x - half - 1.5f);
    int right = ceil_int(max_x + half + 1.5f);
    int top = floor_int(min_y - half - 1.5f);
    int bottom = ceil_int(max_y + half + 1.5f);
    clamp_bounds(surface, &left, &top, &right, &bottom);

    uint32_t red = (value >> 16) & 255;
    uint32_t green = (value >> 8) & 255;
    uint32_t blue = value & 255;
    unsigned segments = closed || fill ? count : count - 1;

    for (int y = top; y < bottom; ++y) {
        float py = (float)y + 0.5f;

        for (int x = left; x < right; ++x) {
            float px = (float)x + 0.5f;
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

            float coverage;

            if (min_sq > edge_limit_sq) {
                if (!fill || !inside) {
                    continue;
                }

                coverage = 1.0f;
            } else {
                float dist = fast_sqrt(min_sq, half + 1.0f);
                coverage = fill ? 0.5f + (inside ? dist : -dist) : half + 0.5f - dist;
            }

            if (coverage <= 0.0f) {
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

void canvas_raster_ellipse(const RasterSurface *surface, Rect rect, bool fill, float width, Color value) {
    if (!surface || !surface->pixels || surface->scale <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f ||
        (!fill && width <= 0.0f)) {
        return;
    }

    float cx = (rect.x + rect.w * 0.5f) * surface->scale;
    float cy = (rect.y + rect.h * 0.5f) * surface->scale;
    float half = fill ? 0.0f : width * surface->scale * 0.5f;
    float rx = rect.w * surface->scale * 0.5f;
    float ry = rect.h * surface->scale * 0.5f;
    RoundShape outer = {cx, cy, 0, 0, ry + half, (rx + half) / (ry + half)};
    bool has_inner = !fill && rx > half && ry > half;
    RoundShape inner = {0};

    if (has_inner) {
        inner = (RoundShape){cx, cy, 0, 0, ry - half, (rx - half) / (ry - half)};
    }

    int left = floor_int(cx - rx - half);
    int right = ceil_int(cx + rx + half);
    int top = floor_int(cy - ry - half);
    int bottom = ceil_int(cy + ry + half);
    clamp_bounds(surface, &left, &top, &right, &bottom);

    uint32_t red = (value >> 16) & 255;
    uint32_t green = (value >> 8) & 255;
    uint32_t blue = value & 255;

    for (int y = top; y < bottom; ++y) {
        float py = (float)y + 0.5f;

        for (int x = left; x < right; ++x) {
            float px = (float)x + 0.5f;
            float coverage = masked_coverage(surface, &outer, px, py);

            if (coverage > 0.0f && has_inner) {
                coverage -= masked_coverage(surface, &inner, px, py);
            }

            if (coverage <= 0.0f) {
                continue;
            }

            blend_pixel(&surface->pixels[y * surface->width + x], red, green, blue, coverage);
        }
    }
}

static float sample_row_box(const uint32_t *row, unsigned width, float u0, float u1, float *out_r,
                            float *out_g, float *out_b) {
    float sum_r = 0.0f;
    float sum_g = 0.0f;
    float sum_b = 0.0f;
    float sum_w = 0.0f;

    if (u0 < 0.0f) {
        float end = u1 < 0.0f ? u1 : 0.0f;
        float w = end - u0;
        uint32_t p = row[0];
        sum_r += (float)((p >> 16) & 255) * w;
        sum_g += (float)((p >> 8) & 255) * w;
        sum_b += (float)(p & 255) * w;
        sum_w += w;
        u0 = end;
    }

    if (u1 > (float)width) {
        float start = u0 > (float)width ? u0 : (float)width;
        float w = u1 - start;
        uint32_t p = row[width - 1];
        sum_r += (float)((p >> 16) & 255) * w;
        sum_g += (float)((p >> 8) & 255) * w;
        sum_b += (float)(p & 255) * w;
        sum_w += w;
        u1 = start;
    }

    if (u1 > u0) {
        int sx0 = floor_int(u0);
        int sx1 = floor_int(u1 - 1e-5f);

        if (sx0 < 0) {
            sx0 = 0;
        }

        if (sx1 >= (int)width) {
            sx1 = (int)width - 1;
        }

        if (sx1 < sx0) {
            sx1 = sx0;
        }

        if (sx0 == sx1) {
            float w = u1 - u0;
            uint32_t p = row[sx0];
            sum_r += (float)((p >> 16) & 255) * w;
            sum_g += (float)((p >> 8) & 255) * w;
            sum_b += (float)(p & 255) * w;
            sum_w += w;
        } else {
            float w_left = (float)(sx0 + 1) - u0;
            uint32_t p_left = row[sx0];
            sum_r += (float)((p_left >> 16) & 255) * w_left;
            sum_g += (float)((p_left >> 8) & 255) * w_left;
            sum_b += (float)(p_left & 255) * w_left;

            uint32_t mid_r = 0;
            uint32_t mid_g = 0;
            uint32_t mid_b = 0;

            for (int sx = sx0 + 1; sx < sx1; ++sx) {
                uint32_t p = row[sx];
                mid_r += (p >> 16) & 255;
                mid_g += (p >> 8) & 255;
                mid_b += p & 255;
            }

            sum_r += (float)mid_r;
            sum_g += (float)mid_g;
            sum_b += (float)mid_b;

            float w_right = u1 - (float)sx1;
            uint32_t p_right = row[sx1];
            sum_r += (float)((p_right >> 16) & 255) * w_right;
            sum_g += (float)((p_right >> 8) & 255) * w_right;
            sum_b += (float)(p_right & 255) * w_right;

            sum_w += u1 - u0;
        }
    }

    *out_r = sum_r;
    *out_g = sum_g;
    *out_b = sum_b;
    return sum_w;
}

static float sample_row_linear(const uint32_t *row, unsigned width, float u_center, float *out_r,
                               float *out_g, float *out_b) {
    float u = u_center - 0.5f;
    int sx0 = floor_int(u);
    float fx = u - (float)sx0;

    if (sx0 < 0) {
        sx0 = 0;
        fx = 0.0f;
    }

    if ((unsigned)sx0 >= width) {
        sx0 = (int)width - 1;
        fx = 0.0f;
    }

    int sx1 = (unsigned)(sx0 + 1) < width ? sx0 + 1 : sx0;
    uint32_t p0 = row[sx0];
    uint32_t p1 = row[sx1];
    float w0 = 1.0f - fx;

    *out_r = (float)((p0 >> 16) & 255) * w0 + (float)((p1 >> 16) & 255) * fx;
    *out_g = (float)((p0 >> 8) & 255) * w0 + (float)((p1 >> 8) & 255) * fx;
    *out_b = (float)(p0 & 255) * w0 + (float)(p1 & 255) * fx;
    return 1.0f;
}

static uint32_t clamp_byte(double value) {
    if (value <= 0.0) {
        return 0;
    }

    if (value >= 255.0) {
        return 255;
    }

    return (uint32_t)(value + 0.5);
}

static void raster_image_bilinear(const RasterSurface *surface, const Image *image, const RoundShape *frame,
                                  float rx, float ry, float rw, float rh, int left, int top, int right,
                                  int bottom) {
    float inv_x = (float)image->width / rw;
    float inv_y = (float)image->height / rh;

    for (int y = top; y < bottom; ++y) {
        float py = (float)y + 0.5f;
        float v = (py - ry) * inv_y - 0.5f;
        int sy0 = floor_int(v);
        float fy = v - (float)sy0;

        if (sy0 < 0) {
            sy0 = 0;
            fy = 0.0f;
        }

        if ((unsigned)sy0 >= image->height) {
            sy0 = (int)image->height - 1;
            fy = 0.0f;
        }

        int sy1 = (unsigned)(sy0 + 1) < image->height ? sy0 + 1 : sy0;
        const uint32_t *row0 = image->pixels + (unsigned)sy0 * image->width;
        const uint32_t *row1 = image->pixels + (unsigned)sy1 * image->width;

        for (int x = left; x < right; ++x) {
            float px = (float)x + 0.5f;
            float cov = masked_coverage(surface, frame, px, py);

            if (cov <= 0.0f) {
                continue;
            }

            float u = (px - rx) * inv_x - 0.5f;
            int sx0 = floor_int(u);
            float fx = u - (float)sx0;

            if (sx0 < 0) {
                sx0 = 0;
                fx = 0.0f;
            }

            if ((unsigned)sx0 >= image->width) {
                sx0 = (int)image->width - 1;
                fx = 0.0f;
            }

            int sx1 = (unsigned)(sx0 + 1) < image->width ? sx0 + 1 : sx0;

            uint32_t p00 = row0[sx0];
            uint32_t p10 = row0[sx1];
            uint32_t p01 = row1[sx0];
            uint32_t p11 = row1[sx1];

            float w00 = (1.0f - fx) * (1.0f - fy);
            float w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy;
            float w11 = fx * fy;

            uint32_t r = (uint32_t)(((p00 >> 16) & 255) * w00 + ((p10 >> 16) & 255) * w10 +
                                    ((p01 >> 16) & 255) * w01 + ((p11 >> 16) & 255) * w11 + 0.5f);
            uint32_t g = (uint32_t)(((p00 >> 8) & 255) * w00 + ((p10 >> 8) & 255) * w10 +
                                    ((p01 >> 8) & 255) * w01 + ((p11 >> 8) & 255) * w11 + 0.5f);
            uint32_t b = (uint32_t)((p00 & 255) * w00 + (p10 & 255) * w10 + (p01 & 255) * w01 +
                                    (p11 & 255) * w11 + 0.5f);
            blend_pixel(&surface->pixels[y * surface->width + x], r, g, b, cov);
        }
    }
}

static float sample_row_any(const uint32_t *row, unsigned width, bool area_x, float u0, float u1,
                            float u_center, float *out_r, float *out_g, float *out_b) {
    if (area_x) {
        return sample_row_box(row, width, u0, u1, out_r, out_g, out_b);
    }

    return sample_row_linear(row, width, u_center, out_r, out_g, out_b);
}

typedef struct {
    bool area_x;
    bool area_y;
    float v0;
    float v1;
    float w_top;
    float w_bot;
    int sy0;
    int sy1;
    float fy;
} AreaRowSpan;

static AreaRowSpan compute_area_row(const Image *image, int y, float ry, float inv_y, bool area_x,
                                    bool area_y) {
    AreaRowSpan span = {.area_x = area_x, .area_y = area_y};

    if (!area_y) {
        float py = (float)y + 0.5f;
        float v = (py - ry) * inv_y - 0.5f;
        span.sy0 = floor_int(v);
        span.fy = v - (float)span.sy0;

        if (span.sy0 < 0) {
            span.sy0 = 0;
            span.fy = 0.0f;
        }

        if ((unsigned)span.sy0 >= image->height) {
            span.sy0 = (int)image->height - 1;
            span.fy = 0.0f;
        }

        span.sy1 = (unsigned)(span.sy0 + 1) < image->height ? span.sy0 + 1 : span.sy0;
        return span;
    }

    float v0 = ((float)y - ry) * inv_y;
    float v1 = ((float)(y + 1) - ry) * inv_y;

    if (v0 < 0.0f) {
        float end = v1 < 0.0f ? v1 : 0.0f;
        span.w_top = end - v0;
        v0 = end;
    }

    if (v1 > (float)image->height) {
        float start = v0 > (float)image->height ? v0 : (float)image->height;
        span.w_bot = v1 - start;
        v1 = start;
    }

    span.v0 = v0;
    span.v1 = v1;

    if (v1 > v0) {
        span.sy0 = floor_int(v0);
        span.sy1 = floor_int(v1 - 1e-5f);

        if (span.sy0 < 0) {
            span.sy0 = 0;
        }

        if (span.sy1 >= (int)image->height) {
            span.sy1 = (int)image->height - 1;
        }

        if (span.sy1 < span.sy0) {
            span.sy1 = span.sy0;
        }
    }

    return span;
}

static void sample_area_pixel(const Image *image, const AreaRowSpan *span, float u0, float u1, float u_center,
                              uint32_t *out_r, uint32_t *out_g, uint32_t *out_b) {
    double sum_r = 0.0;
    double sum_g = 0.0;
    double sum_b = 0.0;
    double sum_w = 0.0;

    if (!span->area_y) {
        const uint32_t *row0 = image->pixels + (unsigned)span->sy0 * image->width;
        const uint32_t *row1 = image->pixels + (unsigned)span->sy1 * image->width;
        float r0 = 0.0f;
        float g0 = 0.0f;
        float b0 = 0.0f;
        float r1 = 0.0f;
        float g1 = 0.0f;
        float b1 = 0.0f;
        float w0 = sample_row_box(row0, image->width, u0, u1, &r0, &g0, &b0);
        float w1 = sample_row_box(row1, image->width, u0, u1, &r1, &g1, &b1);
        float wy0 = 1.0f - span->fy;

        sum_r = (double)r0 * wy0 + (double)r1 * span->fy;
        sum_g = (double)g0 * wy0 + (double)g1 * span->fy;
        sum_b = (double)b0 * wy0 + (double)b1 * span->fy;
        sum_w = (double)w0 * wy0 + (double)w1 * span->fy;
    } else {
        if (span->w_top > 0.0f) {
            float rr = 0.0f;
            float rg = 0.0f;
            float rb = 0.0f;
            float rw_sum =
                sample_row_any(image->pixels, image->width, span->area_x, u0, u1, u_center, &rr, &rg, &rb);
            sum_r += (double)rr * span->w_top;
            sum_g += (double)rg * span->w_top;
            sum_b += (double)rb * span->w_top;
            sum_w += (double)rw_sum * span->w_top;
        }

        if (span->v1 > span->v0) {
            for (int sy = span->sy0; sy <= span->sy1; ++sy) {
                float top_e = span->v0 > (float)sy ? span->v0 : (float)sy;
                float bot_e = span->v1 < (float)(sy + 1) ? span->v1 : (float)(sy + 1);
                float wy = span->sy0 == span->sy1 ? (span->v1 - span->v0) : (bot_e - top_e);
                const uint32_t *row = image->pixels + (unsigned)sy * image->width;
                float rr = 0.0f;
                float rg = 0.0f;
                float rb = 0.0f;
                float rw_sum =
                    sample_row_any(row, image->width, span->area_x, u0, u1, u_center, &rr, &rg, &rb);
                sum_r += (double)rr * wy;
                sum_g += (double)rg * wy;
                sum_b += (double)rb * wy;
                sum_w += (double)rw_sum * wy;
            }
        }

        if (span->w_bot > 0.0f) {
            const uint32_t *row = image->pixels + (image->height - 1) * image->width;
            float rr = 0.0f;
            float rg = 0.0f;
            float rb = 0.0f;
            float rw_sum = sample_row_any(row, image->width, span->area_x, u0, u1, u_center, &rr, &rg, &rb);
            sum_r += (double)rr * span->w_bot;
            sum_g += (double)rg * span->w_bot;
            sum_b += (double)rb * span->w_bot;
            sum_w += (double)rw_sum * span->w_bot;
        }
    }

    if (sum_w <= 0.0) {
        *out_r = 0;
        *out_g = 0;
        *out_b = 0;
        return;
    }

    *out_r = clamp_byte(sum_r / sum_w);
    *out_g = clamp_byte(sum_g / sum_w);
    *out_b = clamp_byte(sum_b / sum_w);
}

void canvas_raster_image(const RasterSurface *surface, const Image *image, Rect rect, float radius) {
    if (!surface || !surface->pixels || surface->scale <= 0.0f || !image || !image->pixels || !image->width ||
        !image->height || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    RoundShape frame = round_shape(rect, radius, surface->scale);
    Rect cover = canvas_cover_rect(rect, image->width, image->height);
    float rx = cover.x * surface->scale;
    float ry = cover.y * surface->scale;
    float rw = cover.w * surface->scale;
    float rh = cover.h * surface->scale;

    if (rw <= 0.0f || rh <= 0.0f) {
        return;
    }

    int left = floor_int(rect.x * surface->scale);
    int right = ceil_int((rect.x + rect.w) * surface->scale);
    int top = floor_int(rect.y * surface->scale);
    int bottom = ceil_int((rect.y + rect.h) * surface->scale);
    clamp_bounds(surface, &left, &top, &right, &bottom);

    if (left >= right || top >= bottom) {
        return;
    }

    bool area_x = rw < (float)image->width;
    bool area_y = rh < (float)image->height;

    if (!area_x && !area_y) {
        raster_image_bilinear(surface, image, &frame, rx, ry, rw, rh, left, top, right, bottom);
        return;
    }

    float inv_x = (float)image->width / rw;
    float inv_y = (float)image->height / rh;

    for (int y = top; y < bottom; ++y) {
        float py = (float)y + 0.5f;
        AreaRowSpan span = compute_area_row(image, y, ry, inv_y, area_x, area_y);

        for (int x = left; x < right; ++x) {
            float px = (float)x + 0.5f;
            float cov = masked_coverage(surface, &frame, px, py);

            if (cov <= 0.0f) {
                continue;
            }

            float u0 = ((float)x - rx) * inv_x;
            float u1 = ((float)(x + 1) - rx) * inv_x;
            float u_center = (px - rx) * inv_x;
            uint32_t out_r = 0;
            uint32_t out_g = 0;
            uint32_t out_b = 0;
            sample_area_pixel(image, &span, u0, u1, u_center, &out_r, &out_g, &out_b);
            blend_pixel(&surface->pixels[y * surface->width + x], out_r, out_g, out_b, cov);
        }
    }
}
