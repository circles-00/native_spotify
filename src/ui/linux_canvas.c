#define _POSIX_C_SOURCE 200809L
#include "linux_canvas.h"
#include "artwork.h"

#include <fontconfig/fontconfig.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void color(cairo_t *paint, Color value) {
    cairo_set_source_rgb(paint, ((value >> 16) & 255) / 255.0, ((value >> 8) & 255) / 255.0,
                         (value & 255) / 255.0);
}

static void rounded(cairo_t *paint, Rect rect, float radius) {
    if (radius > rect.h / 2) {
        radius = rect.h / 2;
    }
    if (radius > rect.w / 2) {
        radius = rect.w / 2;
    }

    cairo_new_sub_path(paint);
    cairo_arc(paint, rect.x + rect.w - radius, rect.y + radius, radius, -1.570796327, 0);
    cairo_arc(paint, rect.x + rect.w - radius, rect.y + rect.h - radius, radius, 0, 1.570796327);
    cairo_arc(paint, rect.x + radius, rect.y + rect.h - radius, radius, 1.570796327, 3.141592654);
    cairo_arc(paint, rect.x + radius, rect.y + radius, radius, 3.141592654, 4.712388980);
    cairo_close_path(paint);
}

static void box(void *context, Rect rect, float radius, Color value) {
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    LinuxCanvas *backend = context;
    rounded(backend->paint, rect, radius);
    color(backend->paint, value);
    cairo_fill(backend->paint);
}

static void gradient(void *context, Rect rect, float radius, Color top, Color bottom) {
    LinuxCanvas *backend = context;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(rect.x, rect.y, rect.x, rect.y + rect.h);

    cairo_pattern_add_color_stop_rgb(pattern, 0, ((top >> 16) & 255) / 255.0, ((top >> 8) & 255) / 255.0,
                                     (top & 255) / 255.0);
    cairo_pattern_add_color_stop_rgb(pattern, 1, ((bottom >> 16) & 255) / 255.0,
                                     ((bottom >> 8) & 255) / 255.0, (bottom & 255) / 255.0);

    rounded(backend->paint, rect, radius);
    cairo_set_source(backend->paint, pattern);
    cairo_fill(backend->paint);
    cairo_pattern_destroy(pattern);
}

static void text(void *context, const char *value, Rect rect, float size, unsigned style, Color value_color) {
    LinuxCanvas *backend = context;
    PangoFontDescription *font = backend->fonts[(style & TEXT_BOLD) ? 1 : 0];
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(backend->layout, font);
    pango_layout_set_text(backend->layout, value, -1);
    pango_layout_set_width(backend->layout, (int)(rect.w * PANGO_SCALE));
    bool single_line = (style & TEXT_VCENTER) || rect.h <= size * 2;
    pango_layout_set_height(backend->layout, single_line ? -1 : (int)(rect.h * PANGO_SCALE));
    pango_layout_set_single_paragraph_mode(backend->layout, single_line);
    pango_layout_set_ellipsize(backend->layout,
                               (style & TEXT_INPUT) ? PANGO_ELLIPSIZE_START : PANGO_ELLIPSIZE_END);
    pango_layout_set_wrap(backend->layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(backend->layout, style & TEXT_CENTER ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(backend->layout, &ink, &logical);
    bool center_ink =
        (style & (TEXT_CENTER | TEXT_VCENTER)) == (TEXT_CENTER | TEXT_VCENTER) && (style & TEXT_BOLD);

    color(backend->paint, value_color);
    cairo_move_to(backend->paint, rect.x + ((style & TEXT_CENTER) ? (rect.w - ink.width) / 2 - ink.x : 0),
                  rect.y + (!(style & TEXT_VCENTER) ? 0
                            : center_ink            ? (rect.h - ink.height) / 2 - ink.y
                                                    : (rect.h - logical.height) / 2 - logical.y));
    pango_cairo_show_layout(backend->paint, backend->layout);
}

static float measure(void *context, const char *value, float size) {
    LinuxCanvas *backend = context;
    pango_font_description_set_absolute_size(backend->fonts[0], size * PANGO_SCALE);
    pango_layout_set_font_description(backend->layout, backend->fonts[0]);
    pango_layout_set_text(backend->layout, value, -1);
    pango_layout_set_width(backend->layout, -1);
    pango_layout_set_ellipsize(backend->layout, PANGO_ELLIPSIZE_NONE);
    int width;
    pango_layout_get_pixel_size(backend->layout, &width, NULL);

    return (float)width;
}

static void path(void *context, const Point *points, unsigned count, bool closed, bool fill, float width,
                 Color value) {
    if (!count) {
        return;
    }

    LinuxCanvas *backend = context;
    cairo_new_path(backend->paint);
    cairo_move_to(backend->paint, points[0].x, points[0].y);

    for (unsigned i = 1; i < count; ++i) {
        cairo_line_to(backend->paint, points[i].x, points[i].y);
    }

    if (closed) {
        cairo_close_path(backend->paint);
    }

    color(backend->paint, value);
    cairo_set_line_width(backend->paint, width);
    cairo_set_line_cap(backend->paint, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(backend->paint, CAIRO_LINE_JOIN_ROUND);

    if (fill) {
        cairo_fill(backend->paint);
    }
    if (!fill) {
        cairo_stroke(backend->paint);
    }
}

static void ellipse(void *context, Rect rect, bool fill, float width, Color value) {
    LinuxCanvas *backend = context;
    cairo_new_path(backend->paint);
    cairo_save(backend->paint);
    cairo_translate(backend->paint, rect.x + rect.w / 2, rect.y + rect.h / 2);
    cairo_scale(backend->paint, rect.w / 2, rect.h / 2);
    cairo_arc(backend->paint, 0, 0, 1, 0, 6.283185307);
    cairo_restore(backend->paint);
    color(backend->paint, value);
    cairo_set_line_width(backend->paint, width);

    if (fill) {
        cairo_fill(backend->paint);
    }
    if (!fill) {
        cairo_stroke(backend->paint);
    }
}

typedef struct {
    LinuxCanvas *backend;
    Rect rect;
} ImagePaint;

static void paint_image(void *context, const Image *image) {
    ImagePaint *target = context;
    float scale = target->rect.w / image->width;

    if (scale * image->height > target->rect.h) {
        scale = target->rect.h / image->height;
    }

    cairo_surface_t *surface =
        cairo_image_surface_create_for_data((unsigned char *)image->pixels, CAIRO_FORMAT_RGB24,
                                            (int)image->width, (int)image->height, (int)image->width * 4);
    cairo_t *paint = target->backend->paint;
    cairo_save(paint);
    cairo_translate(paint, target->rect.x + (target->rect.w - image->width * scale) / 2,
                    target->rect.y + (target->rect.h - image->height * scale) / 2);
    cairo_scale(paint, scale, scale);
    cairo_set_source_surface(paint, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(paint), CAIRO_FILTER_BILINEAR);
    cairo_rectangle(paint, 0, 0, image->width, image->height);
    cairo_fill(paint);
    cairo_restore(paint);
    cairo_surface_destroy(surface);
}

static bool image(void *context, const char *url, Rect rect) {
    ImagePaint target = {context, rect};
    return artwork_visit(url, paint_image, &target);
}

static void clip(void *context, Rect rect, float radius) {
    LinuxCanvas *backend = context;
    cairo_save(backend->paint);
    rounded(backend->paint, rect, radius);
    cairo_clip(backend->paint);
}

static void unclip(void *context) {
    LinuxCanvas *backend = context;
    cairo_restore(backend->paint);
}

static void release(void *context) {
    LinuxCanvas *backend = context;

    if (backend->retained) {
        cairo_surface_destroy(backend->retained);
        backend->retained = NULL;
    }
}

static bool retain(void *context, Rect rect) {
    LinuxCanvas *backend = context;
    release(context);
    backend->retained = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)rect.w, (int)rect.h);

    if (cairo_surface_status(backend->retained) != CAIRO_STATUS_SUCCESS) {
        release(context);
        return false;
    }

    cairo_t *paint = cairo_create(backend->retained);
    cairo_set_source_surface(paint, cairo_get_target(backend->paint), -rect.x, -rect.y);
    cairo_paint(paint);
    cairo_destroy(paint);
    return true;
}

static void replay(void *context, Rect rect, float opacity) {
    LinuxCanvas *backend = context;

    if (!backend->retained) {
        return;
    }

    cairo_set_source_surface(backend->paint, backend->retained, rect.x, rect.y);
    cairo_paint_with_alpha(backend->paint, opacity);
}

void linux_canvas_begin(LinuxCanvas *backend, cairo_t *paint, Canvas *canvas) {
    backend->paint = paint;
    if (!backend->layout) {
        backend->layout = pango_cairo_create_layout(paint);
    }

    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(paint, options);
    pango_cairo_context_set_font_options(pango_layout_get_context(backend->layout), options);
    cairo_font_options_destroy(options);

    pango_cairo_update_layout(paint, backend->layout);

    if (!backend->fonts[0]) {
        backend->fonts[0] = pango_font_description_from_string("Fira Sans");
        backend->fonts[1] = pango_font_description_from_string("Fira Sans Bold");
    }

    *canvas = (Canvas){backend, false, box,  gradient, text,   path,   ellipse,
                       measure, image, clip, unclip,   retain, replay, release};
}

void linux_canvas_end(LinuxCanvas *backend) {
    backend->paint = NULL;
}

void linux_canvas_destroy(LinuxCanvas *backend) {
    release(backend);
    if (backend->layout) {
        g_object_unref(backend->layout);
    }
    if (backend->fonts[0]) {
        pango_font_description_free(backend->fonts[0]);
    }
    if (backend->fonts[1]) {
        pango_font_description_free(backend->fonts[1]);
    }
}

void linux_load_font(void) {
    char executable[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size <= 0) {
        return;
    }

    executable[size] = 0;
    char *slash = strrchr(executable, '/');
    if (!slash) {
        return;
    }
    *slash = 0;

    char font[PATH_MAX + 32];
    snprintf(font, sizeof(font), "%s/assets/FiraSans-Regular.ttf", executable);
    FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)font);
    snprintf(font, sizeof(font), "%s/assets/FiraSans-Bold.ttf", executable);
    FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)font);
}
