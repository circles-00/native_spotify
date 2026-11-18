#include "windows_canvas.h"
#include "artwork.h"
#include "core.h"

#include <stdio.h>
#include <wchar.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_BATCH_LIMIT 256

typedef struct {
    char value[1024];
    Rect rect;
    float size;
    unsigned style;
    Color color;
    HRGN clip;
} WindowsText;

struct WindowsTextBatch {
    WindowsText entries[TEXT_BATCH_LIMIT];
    unsigned count;
};

static void occlude_text(WindowsCanvas *backend, Rect rect, float radius) {
    if (!backend->text_target || !backend->text_batch->count) {
        return;
    }

    float scale = backend->text_target->scale;
    int corner = (int)(radius * 2 * scale + .5f);
    HRGN region = corner > 0 ? CreateRoundRectRgn((int)(rect.x * scale), (int)(rect.y * scale),
                                                  (int)((rect.x + rect.w) * scale + .5f) + 1,
                                                  (int)((rect.y + rect.h) * scale + .5f) + 1, corner, corner)
                             : CreateRectRgn((int)(rect.x * scale), (int)(rect.y * scale),
                                             (int)((rect.x + rect.w) * scale + .5f),
                                             (int)((rect.y + rect.h) * scale + .5f));
    RECT clip;
    GetClipBox(backend->paint, &clip);
    float ratio = scale / backend->scale;
    HRGN visible = CreateRectRgn((int)(clip.left * ratio), (int)(clip.top * ratio),
                                 (int)(clip.right * ratio + .5f), (int)(clip.bottom * ratio + .5f));
    CombineRgn(region, region, visible, RGN_AND);

    for (unsigned i = 0; i < backend->text_batch->count; ++i) {
        CombineRgn(backend->text_batch->entries[i].clip, backend->text_batch->entries[i].clip, region,
                   RGN_DIFF);
    }

    DeleteObject(visible);
    DeleteObject(region);
}

static bool dib_surface(HDC paint, uint32_t **pixels, int *width, int *height, RECT *clip) {
    DIBSECTION dib = {0};

    if (GetObjectW(GetCurrentObject(paint, OBJ_BITMAP), sizeof(dib), &dib) != sizeof(dib) ||
        !dib.dsBm.bmBits || dib.dsBmih.biBitCount != 32 || dib.dsBmih.biHeight >= 0) {
        return false;
    }

    *pixels = dib.dsBm.bmBits;
    *width = dib.dsBmih.biWidth;
    *height = -dib.dsBmih.biHeight;
    GetClipBox(paint, clip);
    GdiFlush();
    return true;
}

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

static float clip_coverage(const WindowsCanvas *backend, float px, float py) {
    float coverage = 1.0f;

    for (unsigned i = 0; i < backend->clip_depth; ++i) {
        Rect c = backend->clips[i].rect;
        float r = backend->clips[i].radius * backend->scale;

        if (r <= 0) {
            continue;
        }

        float rw = c.w * backend->scale;
        float rh = c.h * backend->scale;

        if (r > rw * .5f) {
            r = rw * .5f;
        }
        if (r > rh * .5f) {
            r = rh * .5f;
        }

        float dist = round_rect_sdf(px, py, (c.x + c.w * .5f) * backend->scale,
                                    (c.y + c.h * .5f) * backend->scale, rw * .5f - r, rh * .5f - r, r);
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

static COLORREF color(Color value) {
    return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255);
}

static int pixel(WindowsCanvas *backend, float value) {
    return (int)(value * backend->scale + .5f);
}

static RECT bounds(WindowsCanvas *backend, Rect rect) {
    return (RECT){pixel(backend, rect.x), pixel(backend, rect.y), pixel(backend, rect.x + rect.w),
                  pixel(backend, rect.y + rect.h)};
}

static void draw_aa_round_rect(const WindowsCanvas *backend, uint32_t *pixels, int width, int height,
                               RECT clip, float rx, float ry, float rw, float rh, float r, Color top,
                               Color bottom) {
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

    if (left < clip.left) {
        left = clip.left;
    }
    if (top_y < clip.top) {
        top_y = clip.top;
    }
    if (right > clip.right) {
        right = clip.right;
    }
    if (bottom_y > clip.bottom) {
        bottom_y = clip.bottom;
    }
    if (left < 0) {
        left = 0;
    }
    if (top_y < 0) {
        top_y = 0;
    }
    if (right > width) {
        right = width;
    }
    if (bottom_y > height) {
        bottom_y = height;
    }

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

            if (backend->clip_depth) {
                float c_cov = clip_coverage(backend, px, py);
                if (c_cov < coverage) {
                    coverage = c_cov;
                }
            }

            blend_pixel(&pixels[y * width + x], red, green, blue, coverage);
        }
    }
}

static void box(void *context, Rect rect, float radius, Color value) {
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    WindowsCanvas *backend = context;
    occlude_text(backend, rect, radius);

    if (radius > 0 || backend->clip_depth > 0) {
        uint32_t *pixels = NULL;
        int width = 0, height = 0;
        RECT clip;

        if (dib_surface(backend->paint, &pixels, &width, &height, &clip)) {
            draw_aa_round_rect(backend, pixels, width, height, clip, rect.x * backend->scale,
                               rect.y * backend->scale, rect.w * backend->scale, rect.h * backend->scale,
                               radius * backend->scale, value, value);
            return;
        }
    }

    RECT area = bounds(backend, rect);
    HBRUSH brush = CreateSolidBrush(color(value));
    HGDIOBJ old_brush = SelectObject(backend->paint, brush);
    HGDIOBJ old_pen = SelectObject(backend->paint, GetStockObject(NULL_PEN));
    int corner = pixel(backend, radius * 2);
    RoundRect(backend->paint, area.left, area.top, area.right + (corner ? 1 : 0),
              area.bottom + (corner ? 1 : 0), corner, corner);

    SelectObject(backend->paint, old_pen);
    SelectObject(backend->paint, old_brush);
    DeleteObject(brush);
}

static void gradient(void *context, Rect rect, float radius, Color top, Color bottom) {
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    WindowsCanvas *backend = context;
    occlude_text(backend, rect, radius);
    uint32_t *pixels = NULL;
    int width = 0, height = 0;
    RECT clip;

    if (dib_surface(backend->paint, &pixels, &width, &height, &clip)) {
        draw_aa_round_rect(backend, pixels, width, height, clip, rect.x * backend->scale,
                           rect.y * backend->scale, rect.w * backend->scale, rect.h * backend->scale,
                           radius * backend->scale, top, bottom);
        return;
    }

    RECT area = bounds(backend, rect);
    int saved = SaveDC(backend->paint);
    HRGN region = CreateRoundRectRgn(area.left, area.top, area.right + 1, area.bottom + 1,
                                     pixel(backend, radius * 2), pixel(backend, radius * 2));
    ExtSelectClipRgn(backend->paint, region, RGN_AND);

    TRIVERTEX vertices[2] = {{area.left, area.top, (COLOR16)((top >> 16 & 255) << 8),
                              (COLOR16)((top >> 8 & 255) << 8), (COLOR16)((top & 255) << 8), 0xff00},
                             {area.right, area.bottom, (COLOR16)((bottom >> 16 & 255) << 8),
                              (COLOR16)((bottom >> 8 & 255) << 8), (COLOR16)((bottom & 255) << 8), 0xff00}};
    GRADIENT_RECT span = {0, 1};
    GradientFill(backend->paint, vertices, 2, &span, 1, GRADIENT_FILL_RECT_V);

    RestoreDC(backend->paint, saved);
    DeleteObject(region);
}

static void center_text(HDC paint, const wchar_t *text, RECT *area, bool horizontal) {
    TEXTMETRICW metrics;
    GetTextMetricsW(paint, &metrics);
    int top = INT_MAX;
    int bottom = INT_MIN;
    int left = INT_MAX;
    int right = INT_MIN;
    int advance = 0;
    MAT2 transform = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

    for (const wchar_t *p = text; *p; ++p) {
        GLYPHMETRICS glyph;

        if (GetGlyphOutlineW(paint, *p, GGO_METRICS, &glyph, 0, NULL, &transform) == GDI_ERROR) {
            area->top += (area->bottom - area->top - metrics.tmHeight) / 2;
            return;
        }

        if (!glyph.gmBlackBoxY) {
            advance += glyph.gmCellIncX;
            continue;
        }

        int start = metrics.tmAscent - glyph.gmptGlyphOrigin.y;
        if (start < top) {
            top = start;
        }
        if (start + (int)glyph.gmBlackBoxY > bottom) {
            bottom = start + (int)glyph.gmBlackBoxY;
        }

        int x = advance + glyph.gmptGlyphOrigin.x;
        if (x < left) {
            left = x;
        }
        if (x + (int)glyph.gmBlackBoxX > right) {
            right = x + (int)glyph.gmBlackBoxX;
        }
        advance += glyph.gmCellIncX;
    }

    if (top != INT_MAX) {
        area->top += (area->bottom - area->top - (bottom - top)) / 2 - top;
    }

    SIZE logical;

    if (horizontal && left != INT_MAX && GetTextExtentPoint32W(paint, text, (int)wcslen(text), &logical) &&
        logical.cx == advance) {
        int shift = (logical.cx - left - right) / 2;
        area->left += shift;
        area->right += shift;
    }
}

static void text(void *context, const char *value, Rect rect, float size, unsigned style, Color value_color) {
    WindowsCanvas *backend = context;

    if (backend->text_target) {
        if (backend->text_batch->count == TEXT_BATCH_LIMIT) {
            return;
        }

        WindowsText *command = &backend->text_batch->entries[backend->text_batch->count++];
        copy_utf8(command->value, sizeof(command->value), value);
        command->rect = rect;
        command->size = size;
        command->style = style;
        command->color = value_color;
        RECT clip;
        GetClipBox(backend->paint, &clip);
        float ratio = backend->text_target->scale / backend->scale;
        command->clip = CreateRectRgn((int)(clip.left * ratio), (int)(clip.top * ratio),
                                      (int)(clip.right * ratio + .5f), (int)(clip.bottom * ratio + .5f));
        return;
    }
    bool bold = (style & TEXT_BOLD) != 0;
    wchar_t utf16[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, utf16, 1024)) {
        return;
    }

    unsigned slot = (unsigned)size;
    if (slot >= 80) {
        slot = 79;
    }

    if (!backend->fonts[slot][bold ? 1 : 0]) {
        backend->fonts[slot][bold ? 1 : 0] =
            CreateFontW(-pixel(backend, size), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                        backend->native_text ? 6 : ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Fira Sans");
    }

    HGDIOBJ previous = SelectObject(backend->paint, backend->fonts[slot][bold ? 1 : 0]);
    SetTextColor(backend->paint, color(value_color));
    SetBkMode(backend->paint, TRANSPARENT);
    RECT area = bounds(backend, rect);
    bool center_ink =
        (style & (TEXT_CENTER | TEXT_VCENTER)) == (TEXT_CENTER | TEXT_VCENTER) && (style & TEXT_BOLD);

    if (center_ink) {
        center_text(backend->paint, utf16, &area, true);
    }

    bool single_line = (style & TEXT_VCENTER) || rect.h <= size * 2;
    DrawTextW(backend->paint, utf16, -1, &area,
              (style & TEXT_CENTER ? DT_CENTER : DT_LEFT) |
                  ((style & TEXT_VCENTER) && !center_ink ? DT_VCENTER : 0) | DT_NOPREFIX | DT_END_ELLIPSIS |
                  DT_NOCLIP | (single_line ? DT_SINGLELINE : DT_WORDBREAK));
    SelectObject(backend->paint, previous);
}

static HPEN pen(WindowsCanvas *backend, float width, Color value) {
    LOGBRUSH brush = {BS_SOLID, color(value), 0};
    unsigned thickness = (unsigned)pixel(backend, width);
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, thickness ? thickness : 1,
                        &brush, 0, NULL);
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

static void path(void *context, const Point *source, unsigned count, bool closed, bool fill, float width,
                 Color value) {
    if (!count || count > 24) {
        return;
    }

    WindowsCanvas *backend = context;
    uint32_t *pixels = NULL;
    int surf_w = 0, surf_h = 0;
    RECT clip;

    if (dib_surface(backend->paint, &pixels, &surf_w, &surf_h, &clip)) {
        Point pts[25];
        float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;

        for (unsigned i = 0; i < count; ++i) {
            pts[i] = (Point){source[i].x * backend->scale, source[i].y * backend->scale};
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

        float half = fill ? 0 : (width * backend->scale > 1.2f ? width * backend->scale * .5f : .6f);
        int left = (int)(min_x - half - 2);
        int right = (int)(max_x + half + 3);
        int top = (int)(min_y - half - 2);
        int bottom = (int)(max_y + half + 3);

        if (left < clip.left) {
            left = clip.left;
        }
        if (top < clip.top) {
            top = clip.top;
        }
        if (right > clip.right) {
            right = clip.right;
        }
        if (bottom > clip.bottom) {
            bottom = clip.bottom;
        }
        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
        if (right > surf_w) {
            right = surf_w;
        }
        if (bottom > surf_h) {
            bottom = surf_h;
        }

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
                if (backend->clip_depth) {
                    float c_cov = clip_coverage(backend, px, py);
                    if (c_cov < coverage) {
                        coverage = c_cov;
                    }
                }
                blend_pixel(&pixels[y * surf_w + x], red, green, blue, coverage);
            }
        }
        return;
    }

    POINT points[25];

    for (unsigned i = 0; i < count; ++i) {
        points[i] = (POINT){pixel(backend, source[i].x), pixel(backend, source[i].y)};
    }
    if (closed) {
        points[count] = points[0];
    }

    HPEN stroke = pen(backend, width, value);
    HBRUSH brush = CreateSolidBrush(color(value));
    HGDIOBJ previous_pen = SelectObject(backend->paint, fill ? GetStockObject(NULL_PEN) : stroke);
    HGDIOBJ previous_brush = SelectObject(backend->paint, brush);

    if (fill) {
        Polygon(backend->paint, points, (int)count);
    }
    if (!fill) {
        Polyline(backend->paint, points, (int)(count + (closed ? 1 : 0)));
    }

    SelectObject(backend->paint, previous_brush);
    SelectObject(backend->paint, previous_pen);
    DeleteObject(stroke);
    DeleteObject(brush);
}

static void draw_aa_stroke_circle(uint32_t *pixels, int width, int height, RECT clip, float cx, float cy,
                                  float radius, float thickness, Color value) {
    float half = thickness * .5f;
    int left = (int)(cx - radius - half - 1);
    int right = (int)(cx + radius + half + 2);
    int top = (int)(cy - radius - half - 1);
    int bottom = (int)(cy + radius + half + 2);

    if (left < clip.left) {
        left = clip.left;
    }
    if (top < clip.top) {
        top = clip.top;
    }
    if (right > clip.right) {
        right = clip.right;
    }
    if (bottom > clip.bottom) {
        bottom = clip.bottom;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > width) {
        right = width;
    }
    if (bottom > height) {
        bottom = height;
    }

    uint32_t red = (value >> 16) & 255;
    uint32_t green = (value >> 8) & 255;
    uint32_t blue = value & 255;
    uint32_t solid = (red << 16) | (green << 8) | blue;

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            float dx = (float)x + .5f - cx;
            float dy = (float)y + .5f - cy;
            float squared = dx * dx + dy * dy;
            float distance = 0;

            if (squared > 0) {
                float guess = radius > 1 ? radius : 1;
                for (unsigned step = 0; step < 5; ++step) {
                    guess = .5f * (guess + squared / guess);
                }
                distance = guess;
            }

            float ring = distance > radius ? distance - radius : radius - distance;
            float coverage = half + .5f - ring;

            if (coverage <= 0) {
                continue;
            }
            if (coverage >= 1) {
                pixels[y * width + x] = solid;
                continue;
            }

            uint32_t current = pixels[y * width + x];
            uint32_t alpha = (uint32_t)(coverage * 255 + .5f);
            uint32_t out_r = (((current >> 16) & 255) * (255 - alpha) + red * alpha) / 255;
            uint32_t out_g = (((current >> 8) & 255) * (255 - alpha) + green * alpha) / 255;
            uint32_t out_b = ((current & 255) * (255 - alpha) + blue * alpha) / 255;
            pixels[y * width + x] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

static void ellipse(void *context, Rect rect, bool fill, float width, Color value) {
    WindowsCanvas *backend = context;
    RECT area = bounds(backend, rect);

    if (area.right > area.left && area.bottom > area.top &&
        area.right - area.left == area.bottom - area.top) {
        uint32_t *pixels = NULL;
        int surf_w = 0, surf_h = 0;
        RECT clip;

        if (dib_surface(backend->paint, &pixels, &surf_w, &surf_h, &clip)) {
            if (fill) {
                draw_aa_round_rect(backend, pixels, surf_w, surf_h, clip, rect.x * backend->scale,
                                   rect.y * backend->scale, rect.w * backend->scale, rect.h * backend->scale,
                                   rect.w * .5f * backend->scale, value, value);
            } else {
                draw_aa_stroke_circle(pixels, surf_w, surf_h, clip, (rect.x + rect.w * .5f) * backend->scale,
                                      (rect.y + rect.h * .5f) * backend->scale, rect.w * .5f * backend->scale,
                                      width * backend->scale > 1 ? width * backend->scale : 1, value);
            }
            return;
        }
    }

    HPEN stroke = pen(backend, width, value);
    HBRUSH brush = CreateSolidBrush(color(value));
    HGDIOBJ previous_pen = SelectObject(backend->paint, fill ? GetStockObject(NULL_PEN) : stroke);
    HGDIOBJ previous_brush = SelectObject(backend->paint, fill ? brush : GetStockObject(NULL_BRUSH));

    Ellipse(backend->paint, area.left, area.top, area.right, area.bottom);

    SelectObject(backend->paint, previous_brush);
    SelectObject(backend->paint, previous_pen);
    DeleteObject(stroke);
    DeleteObject(brush);
}

typedef struct {
    WindowsCanvas *backend;
    Rect rect;
} ImagePaint;

static void paint_image(void *context, const Image *image) {
    ImagePaint *target = context;
    WindowsCanvas *backend = target->backend;
    float scale = target->rect.w / image->width;

    if (scale * image->height > target->rect.h) {
        scale = target->rect.h / image->height;
    }

    Rect rect = {target->rect.x + (target->rect.w - image->width * scale) / 2,
                 target->rect.y + (target->rect.h - image->height * scale) / 2, image->width * scale,
                 image->height * scale};
    occlude_text(backend, rect, 0);
    uint32_t *pixels = NULL;
    int surf_w = 0, surf_h = 0;
    RECT clip;

    if (dib_surface(backend->paint, &pixels, &surf_w, &surf_h, &clip)) {
        float rx = rect.x * backend->scale;
        float ry = rect.y * backend->scale;
        float rw = rect.w * backend->scale;
        float rh = rect.h * backend->scale;
        int left = (int)rx;
        int right = (int)(rx + rw + .5f);
        int top = (int)ry;
        int bottom = (int)(ry + rh + .5f);

        if (left < clip.left) {
            left = clip.left;
        }
        if (top < clip.top) {
            top = clip.top;
        }
        if (right > clip.right) {
            right = clip.right;
        }
        if (bottom > clip.bottom) {
            bottom = clip.bottom;
        }
        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
        if (right > surf_w) {
            right = surf_w;
        }
        if (bottom > surf_h) {
            bottom = surf_h;
        }

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
                float cov = clip_coverage(backend, px, py);
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
                blend_pixel(&pixels[y * surf_w + x], r, g, b, cov);
            }
        }
        return;
    }

    RECT area = bounds(target->backend, rect);
    BITMAPINFO bitmap = {0};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = (LONG)image->width;
    bitmap.bmiHeader.biHeight = -(LONG)image->height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    int saved = SaveDC(target->backend->paint);
    SetStretchBltMode(target->backend->paint, HALFTONE);
    SetBrushOrgEx(target->backend->paint, 0, 0, NULL);
    StretchDIBits(target->backend->paint, area.left, area.top, area.right - area.left, area.bottom - area.top,
                  0, 0, (int)image->width, (int)image->height, image->pixels, &bitmap, DIB_RGB_COLORS,
                  SRCCOPY);
    RestoreDC(target->backend->paint, saved);
}

static bool image(void *context, const char *url, Rect rect) {
    ImagePaint target = {context, rect};
    return artwork_visit(url, paint_image, &target);
}

static void clip(void *context, Rect rect, float radius) {
    WindowsCanvas *backend = context;
    RECT area = bounds(backend, rect);
    SaveDC(backend->paint);
    HRGN region = CreateRectRgn(area.left, area.top, area.right + 1, area.bottom + 1);
    ExtSelectClipRgn(backend->paint, region, RGN_AND);
    DeleteObject(region);

    if (backend->clip_depth < 8) {
        backend->clips[backend->clip_depth].rect = rect;
        backend->clips[backend->clip_depth].radius = radius;
    }
    ++backend->clip_depth;
}

static void unclip(void *context) {
    WindowsCanvas *backend = context;
    RestoreDC(backend->paint, -1);

    if (backend->clip_depth > 0) {
        --backend->clip_depth;
    }
}

static void release(void *context) {
    WindowsCanvas *backend = context;

    if (backend->retained) {
        SelectObject(backend->retained, backend->retained_original);
        DeleteDC(backend->retained);
        DeleteObject(backend->retained_image);
        backend->retained = NULL;
        backend->retained_image = NULL;
    }
}

static bool retain(void *context, Rect rect) {
    WindowsCanvas *backend = context;
    release(context);
    RECT area = bounds(backend, rect);
    backend->retained_width = area.right - area.left;
    backend->retained_height = area.bottom - area.top;
    backend->retained = CreateCompatibleDC(backend->paint);
    backend->retained_image =
        CreateCompatibleBitmap(backend->paint, backend->retained_width, backend->retained_height);

    if (!backend->retained || !backend->retained_image) {
        if (backend->retained) {
            DeleteDC(backend->retained);
        }
        if (backend->retained_image) {
            DeleteObject(backend->retained_image);
        }

        backend->retained = NULL;
        backend->retained_image = NULL;
        return false;
    }

    backend->retained_original = SelectObject(backend->retained, backend->retained_image);

    if (!BitBlt(backend->retained, 0, 0, backend->retained_width, backend->retained_height, backend->paint,
                area.left, area.top, SRCCOPY)) {
        release(context);
        return false;
    }

    return true;
}

static void replay(void *context, Rect rect, float opacity) {
    WindowsCanvas *backend = context;

    if (!backend->retained) {
        return;
    }

    RECT area = bounds(backend, rect);
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)(opacity * 255 + .5f), 0};
    AlphaBlend(backend->paint, area.left, area.top, area.right - area.left, area.bottom - area.top,
               backend->retained, 0, 0, backend->retained_width, backend->retained_height, blend);
}

void windows_canvas_begin(WindowsCanvas *backend, HDC paint, float scale, Canvas *canvas) {
    backend->paint = paint;
    backend->scale = scale;
    backend->clip_depth = 0;
    *canvas = (Canvas){backend, true,  box,  gradient, text,   path,   ellipse,
                       NULL,    image, clip, unclip,   retain, replay, release};
}

static void discard_text(WindowsCanvas *backend) {
    if (backend->text_batch) {
        for (unsigned i = 0; i < backend->text_batch->count; ++i) {
            DeleteObject(backend->text_batch->entries[i].clip);
        }

        backend->text_batch->count = 0;
    }

    backend->text_target = NULL;
}

void windows_canvas_destroy(WindowsCanvas *backend) {
    discard_text(backend);
    free(backend->text_batch);
    backend->text_batch = NULL;

    if (backend->frame) {
        if (backend->frame_image) {
            SelectObject(backend->frame, backend->frame_original);
            DeleteObject(backend->frame_image);
        }

        DeleteDC(backend->frame);
        backend->frame = NULL;
        backend->frame_image = NULL;
        backend->frame_original = NULL;
        backend->frame_width = 0;
        backend->frame_height = 0;
    }

    release(backend);
    for (unsigned i = 0; i < 80; ++i) {
        for (unsigned weight = 0; weight < 2; ++weight) {
            if (backend->fonts[i][weight]) {
                DeleteObject(backend->fonts[i][weight]);
            }
            backend->fonts[i][weight] = NULL;
        }
    }
}

void windows_canvas_defer_text(WindowsCanvas *backend, WindowsCanvas *target) {
    if (!backend->text_batch) {
        backend->text_batch = calloc(1, sizeof(*backend->text_batch));
    }

    if (backend->text_batch) {
        backend->text_target = target;
        target->native_text = true;
    }
}

void windows_canvas_flush_text(WindowsCanvas *backend) {
    if (!backend->text_target) {
        return;
    }

    WindowsCanvas *target = backend->text_target;
    int saved = SaveDC(target->paint);

    for (unsigned i = 0; i < backend->text_batch->count; ++i) {
        WindowsText *command = &backend->text_batch->entries[i];
        SelectClipRgn(target->paint, command->clip);
        text(target, command->value, command->rect, command->size, command->style, command->color);
    }

    RestoreDC(target->paint, saved);
    discard_text(backend);
}

static bool resize_frame(WindowsCanvas *backend, HDC destination, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (!backend->frame) {
        backend->frame = CreateCompatibleDC(destination);
    }

    if (!backend->frame) {
        return false;
    }

    if (backend->frame_image && backend->frame_width == width && backend->frame_height == height) {
        return true;
    }

    HBITMAP image = CreateCompatibleBitmap(destination, width, height);

    if (!image) {
        return false;
    }

    HGDIOBJ previous = SelectObject(backend->frame, image);

    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(image);
        return false;
    }

    if (!backend->frame_image) {
        backend->frame_original = previous;
    }

    if (backend->frame_image) {
        DeleteObject(backend->frame_image);
    }

    backend->frame_image = image;
    backend->frame_width = width;
    backend->frame_height = height;
    return true;
}

bool windows_canvas_present(WindowsCanvas *backend, HDC destination, int width, int height) {
    if (!resize_frame(backend, destination, width, height)) {
        discard_text(backend);
        return false;
    }

    SetStretchBltMode(backend->frame, HALFTONE);
    SetBrushOrgEx(backend->frame, 0, 0, NULL);

    if (!StretchBlt(backend->frame, 0, 0, width, height, backend->paint, 0, 0, width * 2, height * 2,
                    SRCCOPY)) {
        discard_text(backend);
        return false;
    }

    if (backend->text_target) {
        WindowsCanvas *target = backend->text_target;
        HDC previous = target->paint;
        target->paint = backend->frame;
        windows_canvas_flush_text(backend);
        target->paint = previous;
    }

    // Never expose a background-only frame between downsampling and drawing native text.
    return BitBlt(destination, 0, 0, width, height, backend->frame, 0, 0, SRCCOPY) != 0;
}

void windows_load_font(void) {
    AddFontResourceExW(L"assets\\FiraSans-Regular.ttf", FR_PRIVATE, NULL);
    AddFontResourceExW(L"assets\\FiraSans-Bold.ttf", FR_PRIVATE, NULL);

    wchar_t path[32768];
    DWORD size = GetModuleFileNameW(NULL, path, 32768);
    if (!size || size >= 32768) {
        return;
    }

    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash - path) + 32 >= 32768) {
        return;
    }

    wcscpy(slash + 1, L"assets\\FiraSans-Regular.ttf");
    AddFontResourceExW(path, FR_PRIVATE, NULL);
    wcscpy(slash + 1, L"assets\\FiraSans-Bold.ttf");
    AddFontResourceExW(path, FR_PRIVATE, NULL);
}
