#include "win32_canvas.h"
#include "artwork_cache.h"
#include "core.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

static HRGN create_round_clip_region(Rect rect, float radius, float scale) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return CreateRectRgn(0, 0, 0, 0);
    }

    float rx = rect.x * scale;
    float ry = rect.y * scale;
    float rw = rect.w * scale;
    float rh = rect.h * scale;
    int left = floor_int(rx);
    int top = floor_int(ry);
    int right = ceil_int(rx + rw);
    int bottom = ceil_int(ry + rh);

    if (radius <= 0.0f) {
        return CreateRectRgn(left, top, right, bottom);
    }

    float r = radius * scale;

    if (r > rw * 0.5f) {
        r = rw * 0.5f;
    }

    if (r > rh * 0.5f) {
        r = rh * 0.5f;
    }

    int corner = (int)(r * 2.0f + 0.5f);

    if (corner <= 0) {
        return CreateRectRgn(left, top, right, bottom);
    }

    return CreateRoundRectRgn(left, top, right + 1, bottom + 1, corner, corner);
}

static void intersect_active_clips(const WindowsCanvas *backend, HRGN target, float target_scale) {
    unsigned count = backend->clip_depth < 16 ? backend->clip_depth : 16;

    for (unsigned i = 0; i < count; ++i) {
        if (backend->clips[i].radius <= 0.0f && backend->clips[i].rect.w > 0.0f &&
            backend->clips[i].rect.h > 0.0f) {
            continue;
        }

        HRGN clip_rgn =
            create_round_clip_region(backend->clips[i].rect, backend->clips[i].radius, target_scale);
        CombineRgn(target, target, clip_rgn, RGN_AND);
        DeleteObject(clip_rgn);
    }
}

static void occlude_text(WindowsCanvas *backend, Rect rect, float radius) {
    if (!backend->text_target || !backend->text_batch->count) {
        return;
    }

    float scale = backend->text_target->scale;
    HRGN region = create_round_clip_region(rect, radius, scale);
    RECT clip = {0};
    int box_kind = GetClipBox(backend->paint, &clip);

    if (box_kind == NULLREGION || box_kind == ERROR) {
        DeleteObject(region);
        return;
    }

    float ratio = scale / backend->scale;
    HRGN visible = CreateRectRgn(floor_int((float)clip.left * ratio), floor_int((float)clip.top * ratio),
                                 ceil_int((float)clip.right * ratio), ceil_int((float)clip.bottom * ratio));
    CombineRgn(region, region, visible, RGN_AND);
    intersect_active_clips(backend, region, scale);

    for (unsigned i = 0; i < backend->text_batch->count; ++i) {
        CombineRgn(backend->text_batch->entries[i].clip, backend->text_batch->entries[i].clip, region,
                   RGN_DIFF);
    }

    DeleteObject(visible);
    DeleteObject(region);
}

static bool dib_surface(const WindowsCanvas *backend, RasterSurface *surface) {
    if (!backend->pixels) {
        return false;
    }

    RECT clip = {0};
    int box_kind = GetClipBox(backend->paint, &clip);

    if (box_kind == NULLREGION || box_kind == ERROR) {
        clip = (RECT){0, 0, 0, 0};
    }

    GdiFlush();

    *surface = (RasterSurface){
        .pixels = backend->pixels,
        .width = backend->width,
        .height = backend->height,
        .clip = {clip.left, clip.top, clip.right, clip.bottom},
        .scale = backend->scale,
        .round_clips = backend->clips,
        .round_clip_depth = backend->clip_depth < RASTER_CLIP_LIMIT ? backend->clip_depth : RASTER_CLIP_LIMIT,
    };

    return true;
}

static COLORREF color(Color value) {
    return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255);
}

static int pixel(WindowsCanvas *backend, float value) {
    return (int)(value * backend->scale + 0.5f);
}

static RECT bounds(WindowsCanvas *backend, Rect rect) {
    return (RECT){pixel(backend, rect.x), pixel(backend, rect.y), pixel(backend, rect.x + rect.w),
                  pixel(backend, rect.y + rect.h)};
}

static HFONT canvas_font(WindowsCanvas *backend, float size, bool bold) {
    unsigned slot = (unsigned)(size + 0.5f);

    if (slot >= 80) {
        slot = 79;
    }

    unsigned weight = bold ? 1 : 0;

    if (!backend->fonts[slot][weight]) {
        backend->fonts[slot][weight] = CreateFontW(
            -pixel(backend, size), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
            backend->native_text ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Inter");
    }

    return backend->fonts[slot][weight];
}

static void box(void *context, Rect rect, float radius, Color value) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    WindowsCanvas *backend = context;
    occlude_text(backend, rect, radius);

    if (radius > 0.0f || backend->clip_depth > 0 || value >> 24) {
        RasterSurface surface;

        if (dib_surface(backend, &surface)) {
            canvas_raster_round_rect(&surface, rect, radius, value, value);
            return;
        }
    }

    RECT area = bounds(backend, rect);
    HBRUSH brush = CreateSolidBrush(color(value));

    if (!brush) {
        return;
    }

    if (radius <= 0.0f && !backend->clip_depth) {
        FillRect(backend->paint, &area, brush);
    }

    DeleteObject(brush);
}

static void gradient(void *context, Rect rect, float radius, Color top, Color bottom) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    WindowsCanvas *backend = context;
    occlude_text(backend, rect, radius);
    RasterSurface surface;

    if (dib_surface(backend, &surface)) {
        canvas_raster_round_rect(&surface, rect, radius, top, bottom);
    }
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
        logical.cx == advance && logical.cx <= area->right - area->left) {
        int shift = (logical.cx - left - right) / 2;
        area->left += shift;
        area->right += shift;
    }
}

static void draw_multiline_text(HDC paint, const wchar_t *utf16, RECT area, unsigned style) {
    UINT align = (style & TEXT_CENTER) ? DT_CENTER : DT_LEFT;
    RECT calc = area;
    DrawTextW(paint, utf16, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

    if (calc.bottom <= area.bottom) {
        DrawTextW(paint, utf16, -1, &area, align | DT_NOPREFIX | DT_NOCLIP | DT_WORDBREAK);
        return;
    }

    int avail_w = area.right - area.left;

    if (avail_w <= 0) {
        return;
    }

    TEXTMETRICW metrics = {0};
    GetTextMetricsW(paint, &metrics);
    int line_h = metrics.tmHeight > 0 ? metrics.tmHeight : 1;
    int max_lines = (area.bottom - area.top) / line_h;

    if (max_lines < 1) {
        max_lines = 1;
    }

    const wchar_t *cursor = utf16;

    for (int line = 0; line < max_lines - 1 && *cursor; ++line) {
        int seg_len = 0;

        while (cursor[seg_len] && cursor[seg_len] != L'\n' && cursor[seg_len] != L'\r') {
            ++seg_len;
        }

        int fit = 0;
        SIZE extent = {0};

        if (seg_len > 0) {
            GetTextExtentExPointW(paint, cursor, seg_len, avail_w, &fit, NULL, &extent);
        }

        if (fit == 0 && seg_len > 0) {
            fit = 1;
        }

        int draw_len = fit;
        const wchar_t *next = cursor + fit;

        if (fit < seg_len) {
            int split = fit;

            while (split > 0 && cursor[split] != L' ') {
                --split;
            }

            if (split > 0) {
                draw_len = split;
                next = cursor + split + 1;

                while (*next == L' ') {
                    ++next;
                }
            }
        } else {
            if (*next == L'\r') {
                ++next;
            }

            if (*next == L'\n') {
                ++next;
            }
        }

        if (draw_len > 0) {
            RECT line_rect = {area.left, area.top + line * line_h, area.right,
                              area.top + (line + 1) * line_h};
            DrawTextW(paint, cursor, draw_len, &line_rect, align | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        }

        cursor = next;
    }

    if (*cursor) {
        wchar_t tail[1024];
        size_t out = 0;
        bool truncated_nl = false;

        for (const wchar_t *p = cursor; *p && out + 4 < 1024; ++p) {
            if (*p == L'\r') {
                continue;
            }

            if (*p == L'\n') {
                truncated_nl = p[1] != 0;
                break;
            }

            tail[out++] = *p;
        }

        if (truncated_nl) {
            tail[out++] = L'.';
            tail[out++] = L'.';
            tail[out++] = L'.';
        }

        tail[out] = 0;
        RECT last_rect = {area.left, area.top + (max_lines - 1) * line_h, area.right,
                          area.top + max_lines * line_h};
        DrawTextW(paint, tail, -1, &last_rect,
                  align | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_NOCLIP);
    }
}

static void text(void *context, const char *value, Rect rect, float size, unsigned style, Color value_color) {
    if (!value || !value[0] || rect.w <= 0.0f || size <= 0.0f) {
        return;
    }

    WindowsCanvas *backend = context;

    if (backend->text_target) {
        if (backend->text_batch->count == TEXT_BATCH_LIMIT) {
            return;
        }

        RECT clip = {0};
        int box_kind = GetClipBox(backend->paint, &clip);

        if (box_kind == NULLREGION || box_kind == ERROR) {
            return;
        }

        WindowsText *command = &backend->text_batch->entries[backend->text_batch->count++];
        copy_utf8(command->value, sizeof(command->value), value);
        command->rect = rect;
        command->size = size;
        command->style = style;
        command->color = value_color;

        float ratio = backend->text_target->scale / backend->scale;
        command->clip =
            CreateRectRgn(floor_int((float)clip.left * ratio), floor_int((float)clip.top * ratio),
                          ceil_int((float)clip.right * ratio), ceil_int((float)clip.bottom * ratio));
        intersect_active_clips(backend, command->clip, backend->text_target->scale);
        return;
    }

    bool bold = (style & TEXT_BOLD) != 0;
    wchar_t utf16[1024];

    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, utf16, 1024)) {
        return;
    }

    HGDIOBJ previous = SelectObject(backend->paint, canvas_font(backend, size, bold));
    SetTextColor(backend->paint, color(value_color));
    SetBkMode(backend->paint, TRANSPARENT);
    RECT area = bounds(backend, rect);
    bool center_ink =
        (style & (TEXT_CENTER | TEXT_VCENTER)) == (TEXT_CENTER | TEXT_VCENTER) && (style & TEXT_BOLD);

    if (center_ink) {
        center_text(backend->paint, utf16, &area, true);
    }

    if (style & TEXT_INPUT) {
        SIZE extent = {0};

        if (GetTextExtentPoint32W(backend->paint, utf16, (int)wcslen(utf16), &extent) &&
            extent.cx > area.right - area.left) {
            int saved = SaveDC(backend->paint);
            IntersectClipRect(backend->paint, area.left, area.top - 2, area.right, area.bottom + 2);
            DrawTextW(backend->paint, utf16, -1, &area,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            RestoreDC(backend->paint, saved);
            SelectObject(backend->paint, previous);
            return;
        }
    }

    bool single_line = (style & TEXT_VCENTER) || rect.h <= size * 2.0f;

    if (!single_line) {
        draw_multiline_text(backend->paint, utf16, area, style);
        SelectObject(backend->paint, previous);
        return;
    }

    DrawTextW(backend->paint, utf16, -1, &area,
              (style & TEXT_CENTER ? DT_CENTER : DT_LEFT) |
                  ((style & TEXT_VCENTER) && !center_ink ? DT_VCENTER : 0) | DT_NOPREFIX | DT_END_ELLIPSIS |
                  DT_NOCLIP | DT_SINGLELINE);
    SelectObject(backend->paint, previous);
}

static float measure(void *context, const char *value, float size) {
    WindowsCanvas *backend = context;

    if (!value || !value[0] || size <= 0.0f || backend->scale <= 0.0f) {
        return 0.0f;
    }

    wchar_t utf16[1024];

    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, utf16, 1024)) {
        return 0.0f;
    }

    HGDIOBJ previous = SelectObject(backend->paint, canvas_font(backend, size, false));
    SIZE extent = {0};
    BOOL ok = GetTextExtentPoint32W(backend->paint, utf16, (int)wcslen(utf16), &extent);
    SelectObject(backend->paint, previous);

    if (!ok) {
        return 0.0f;
    }

    return (float)extent.cx / backend->scale;
}

static HPEN pen(WindowsCanvas *backend, float width, Color value) {
    LOGBRUSH brush = {BS_SOLID, color(value), 0};
    unsigned thickness = (unsigned)pixel(backend, width);
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, thickness ? thickness : 1,
                        &brush, 0, NULL);
}

static void path(void *context, const Point *source, unsigned count, bool closed, bool fill, float width,
                 Color value) {
    if (!source || !count || count > 24) {
        return;
    }

    WindowsCanvas *backend = context;
    RasterSurface surface;

    if (dib_surface(backend, &surface)) {
        canvas_raster_path(&surface, source, count, closed, fill, width, value);
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

static void ellipse(void *context, Rect rect, bool fill, float width, Color value) {
    WindowsCanvas *backend = context;
    RasterSurface surface;

    if (dib_surface(backend, &surface)) {
        canvas_raster_ellipse(&surface, rect, fill, width, value);
    }
}

typedef struct {
    WindowsCanvas *backend;
    Rect rect;
    float radius;
    bool painted;
} ImagePaint;

bool windows_canvas_paint_image(WindowsCanvas *backend, const Image *image, Rect rect, float radius) {
    if (!image || !image->pixels || !image->width || !image->height || rect.w <= 0.0f || rect.h <= 0.0f) {
        return false;
    }

    RasterSurface surface;

    if (!dib_surface(backend, &surface)) {
        return false;
    }

    occlude_text(backend, rect, radius);
    canvas_raster_image(&surface, image, rect, radius);
    return true;
}

static void paint_image(void *context, const Image *image) {
    ImagePaint *target = context;
    target->painted = windows_canvas_paint_image(target->backend, image, target->rect, target->radius);
}

static bool image(void *context, const char *url, Rect rect, float radius) {
    ImagePaint target = {.backend = context, .rect = rect, .radius = radius};
    return artwork_visit(url, paint_image, &target) && target.painted;
}

static void clip(void *context, Rect rect, float radius) {
    WindowsCanvas *backend = context;
    SaveDC(backend->paint);

    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        HRGN empty = CreateRectRgn(0, 0, 0, 0);
        ExtSelectClipRgn(backend->paint, empty, RGN_AND);
        DeleteObject(empty);
    } else {
        float rx = rect.x * backend->scale;
        float ry = rect.y * backend->scale;
        float rw = rect.w * backend->scale;
        float rh = rect.h * backend->scale;
        HRGN region = CreateRectRgn(floor_int(rx), floor_int(ry), ceil_int(rx + rw), ceil_int(ry + rh));
        ExtSelectClipRgn(backend->paint, region, RGN_AND);
        DeleteObject(region);
    }

    if (backend->clip_depth < RASTER_CLIP_LIMIT) {
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

    if (backend->retained_width <= 0 || backend->retained_height <= 0) {
        return false;
    }

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
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)(opacity * 255.0f + 0.5f), 0};
    AlphaBlend(backend->paint, area.left, area.top, area.right - area.left, area.bottom - area.top,
               backend->retained, 0, 0, backend->retained_width, backend->retained_height, blend);
}

bool windows_buffer_resize(WindowsBuffer *buffer, HDC compatible, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (buffer->bitmap && buffer->width == width && buffer->height == height) {
        return true;
    }

    if (!buffer->dc) {
        buffer->dc = CreateCompatibleDC(compatible);
    }

    if (!buffer->dc) {
        return false;
    }

    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(buffer->dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);

    if (!bitmap) {
        return false;
    }

    HGDIOBJ previous = SelectObject(buffer->dc, bitmap);

    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        return false;
    }

    if (buffer->bitmap) {
        DeleteObject(buffer->bitmap);
    } else {
        buffer->previous = previous;
    }

    // Capture orientation and storage at creation; GetObject can report a positive DIB height.
    buffer->bitmap = bitmap;
    buffer->pixels = pixels;
    buffer->width = width;
    buffer->height = height;
    return true;
}

void windows_buffer_destroy(WindowsBuffer *buffer) {
    if (buffer->bitmap) {
        SelectObject(buffer->dc, buffer->previous);
        DeleteObject(buffer->bitmap);
    }

    if (buffer->dc) {
        DeleteDC(buffer->dc);
    }

    *buffer = (WindowsBuffer){0};
}

void windows_canvas_begin(WindowsCanvas *backend, const WindowsBuffer *buffer, float scale, Canvas *canvas) {
    if (backend->scale != scale) {
        for (unsigned size = 0; size < 80; ++size) {
            for (unsigned weight = 0; weight < 2; ++weight) {
                if (backend->fonts[size][weight]) {
                    DeleteObject(backend->fonts[size][weight]);
                    backend->fonts[size][weight] = NULL;
                }
            }
        }
    }

    backend->paint = buffer->dc;
    backend->pixels = buffer->pixels;
    backend->width = buffer->width;
    backend->height = buffer->height;
    backend->scale = scale;
    backend->clip_depth = 0;
    *canvas = (Canvas){backend, box,  gradient, text,   path,   ellipse, measure,
                       image,   clip, unclip,   retain, replay, release};
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
    AddFontResourceExW(L"assets\\Inter-Regular.ttf", FR_PRIVATE, NULL);
    AddFontResourceExW(L"assets\\Inter-Bold.ttf", FR_PRIVATE, NULL);

    wchar_t path[32768];
    DWORD size = GetModuleFileNameW(NULL, path, 32768);

    if (!size || size >= 32768) {
        return;
    }

    wchar_t *slash = wcsrchr(path, L'\\');

    if (!slash || (size_t)(slash - path) + 32 >= 32768) {
        return;
    }

    wcscpy(slash + 1, L"assets\\Inter-Regular.ttf");
    AddFontResourceExW(path, FR_PRIVATE, NULL);
    wcscpy(slash + 1, L"assets\\Inter-Bold.ttf");
    AddFontResourceExW(path, FR_PRIVATE, NULL);
}
