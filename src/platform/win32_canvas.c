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

static bool dib_surface(const WindowsCanvas *backend, RasterSurface *surface) {
    DIBSECTION dib = {0};

    if (GetObjectW(GetCurrentObject(backend->paint, OBJ_BITMAP), sizeof(dib), &dib) != sizeof(dib) ||
        !dib.dsBm.bmBits || dib.dsBmih.biBitCount != 32 || dib.dsBmih.biHeight >= 0) {
        return false;
    }

    RECT clip;
    GetClipBox(backend->paint, &clip);
    GdiFlush();

    *surface = (RasterSurface){
        .pixels = dib.dsBm.bmBits,
        .width = dib.dsBmih.biWidth,
        .height = -dib.dsBmih.biHeight,
        .clip = {clip.left, clip.top, clip.right, clip.bottom},
        .scale = backend->scale,
        .round_clips = backend->clips,
        .round_clip_depth = backend->clip_depth < 8 ? backend->clip_depth : 8,
    };

    return true;
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

static HFONT canvas_font(WindowsCanvas *backend, float size, bool bold) {
    unsigned slot = (unsigned)size;

    if (slot >= 80) {
        slot = 79;
    }

    unsigned weight = bold ? 1 : 0;

    if (!backend->fonts[slot][weight]) {
        backend->fonts[slot][weight] =
            CreateFontW(-pixel(backend, size), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                        backend->native_text ? 6 : ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Fira Sans");
    }

    return backend->fonts[slot][weight];
}

static void box(void *context, Rect rect, float radius, Color value) {
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    WindowsCanvas *backend = context;
    occlude_text(backend, rect, radius);

    if (radius > 0 || backend->clip_depth > 0) {
        RasterSurface surface;

        if (dib_surface(backend, &surface)) {
            canvas_raster_round_rect(&surface, rect, radius, value, value);
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
    RasterSurface surface;

    if (dib_surface(backend, &surface)) {
        canvas_raster_round_rect(&surface, rect, radius, top, bottom);
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
            IntersectClipRect(backend->paint, area.left, area.top, area.right, area.bottom);
            DrawTextW(backend->paint, utf16, -1, &area,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            RestoreDC(backend->paint, saved);
            SelectObject(backend->paint, previous);
            return;
        }
    }

    bool single_line = (style & TEXT_VCENTER) || rect.h <= size * 2;
    DrawTextW(backend->paint, utf16, -1, &area,
              (style & TEXT_CENTER ? DT_CENTER : DT_LEFT) |
                  ((style & TEXT_VCENTER) && !center_ink ? DT_VCENTER : 0) | DT_NOPREFIX | DT_END_ELLIPSIS |
                  DT_NOCLIP | (single_line ? DT_SINGLELINE : DT_WORDBREAK));
    SelectObject(backend->paint, previous);
}

static float measure(void *context, const char *value, float size) {
    WindowsCanvas *backend = context;

    if (!value || !value[0] || backend->scale <= 0) {
        return 0;
    }

    wchar_t utf16[1024];

    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, utf16, 1024)) {
        return 0;
    }

    HGDIOBJ previous = SelectObject(backend->paint, canvas_font(backend, size, false));
    SIZE extent = {0};
    BOOL ok = GetTextExtentPoint32W(backend->paint, utf16, (int)wcslen(utf16), &extent);
    SelectObject(backend->paint, previous);

    if (!ok) {
        return 0;
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
    if (!count || count > 24) {
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
    RECT area = bounds(backend, rect);

    if (area.right > area.left && area.bottom > area.top &&
        area.right - area.left == area.bottom - area.top) {
        RasterSurface surface;

        if (dib_surface(backend, &surface)) {
            if (fill) {
                canvas_raster_round_rect(&surface, rect, rect.w * .5f, value, value);
            } else {
                canvas_raster_stroke_circle(&surface, rect, width, value);
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
    RasterSurface surface;

    if (dib_surface(backend, &surface)) {
        canvas_raster_image(&surface, image, rect);
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
