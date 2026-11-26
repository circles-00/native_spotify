#define _POSIX_C_SOURCE 200809L
#include "linux_canvas.h"
#include "artwork_cache.h"
#include "embedded_fonts.h"
#include "platform.h"

#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void color(cairo_t *paint, Color value) {
    cairo_set_source_rgb(paint, ((value >> 16) & 255) / 255.0, ((value >> 8) & 255) / 255.0,
                         (value & 255) / 255.0);
}

static void rounded(cairo_t *paint, Rect rect, float radius) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        cairo_new_sub_path(paint);
        cairo_rectangle(paint, rect.x, rect.y, 0.0, 0.0);
        return;
    }

    radius = canvas_corner_radius(rect, radius);

    if (radius == 0.0f) {
        cairo_new_sub_path(paint);
        cairo_rectangle(paint, rect.x, rect.y, rect.w, rect.h);
        return;
    }

    static const double half_pi = 1.5707963267948966;
    static const double pi = 3.1415926535897932;
    static const double three_half_pi = 4.7123889803846899;

    cairo_new_sub_path(paint);
    cairo_arc(paint, rect.x + rect.w - radius, rect.y + radius, radius, -half_pi, 0.0);
    cairo_arc(paint, rect.x + rect.w - radius, rect.y + rect.h - radius, radius, 0.0, half_pi);
    cairo_arc(paint, rect.x + radius, rect.y + rect.h - radius, radius, half_pi, pi);
    cairo_arc(paint, rect.x + radius, rect.y + radius, radius, pi, three_half_pi);
    cairo_close_path(paint);
}

static void box(void *context, Rect rect, float radius, Color value) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    LinuxCanvas *backend = context;
    rounded(backend->paint, rect, radius);

    if (value >> 24) {
        cairo_set_source_rgba(backend->paint, ((value >> 16) & 255) / 255.0, ((value >> 8) & 255) / 255.0,
                              (value & 255) / 255.0, (value >> 24) / 255.0);
    } else {
        color(backend->paint, value);
    }

    cairo_fill(backend->paint);
}

static void gradient(void *context, Rect rect, float radius, Color top, Color bottom) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    LinuxCanvas *backend = context;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(rect.x, rect.y, rect.x, rect.y + rect.h);

    if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(pattern);
        return;
    }

    cairo_pattern_add_color_stop_rgb(pattern, 0.0, ((top >> 16) & 255) / 255.0, ((top >> 8) & 255) / 255.0,
                                     (top & 255) / 255.0);
    cairo_pattern_add_color_stop_rgb(pattern, 1.0, ((bottom >> 16) & 255) / 255.0,
                                     ((bottom >> 8) & 255) / 255.0, (bottom & 255) / 255.0);

    rounded(backend->paint, rect, radius);
    cairo_set_source(backend->paint, pattern);
    cairo_fill(backend->paint);
    cairo_pattern_destroy(pattern);
}

static void text(void *context, const char *value, Rect rect, float size, unsigned style, Color value_color) {
    if (!value || !value[0] || rect.w <= 0.0f || size <= 0.0f) {
        return;
    }

    LinuxCanvas *backend = context;
    PangoFontDescription *font = backend->fonts[(style & TEXT_BOLD) ? 1 : 0];
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(backend->layout, font);
    pango_layout_set_text(backend->layout, value, -1);
    pango_layout_set_width(backend->layout, (int)(rect.w * PANGO_SCALE + 0.5f));

    bool single_line = (style & TEXT_VCENTER) || rect.h <= size * 2.0f;
    pango_layout_set_height(backend->layout, single_line ? -1 : (int)(rect.h * PANGO_SCALE + 0.5f));
    pango_layout_set_single_paragraph_mode(backend->layout, single_line);
    pango_layout_set_ellipsize(backend->layout,
                               (style & TEXT_INPUT) ? PANGO_ELLIPSIZE_START : PANGO_ELLIPSIZE_END);
    pango_layout_set_wrap(backend->layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(backend->layout, style & TEXT_CENTER ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);

    PangoRectangle ink_raw;
    PangoRectangle logical_raw;
    pango_layout_get_extents(backend->layout, &ink_raw, &logical_raw);

    double ink_x = (double)ink_raw.x / PANGO_SCALE;
    double ink_y = (double)ink_raw.y / PANGO_SCALE;
    double ink_w = (double)ink_raw.width / PANGO_SCALE;
    double ink_h = (double)ink_raw.height / PANGO_SCALE;
    double logical_y = (double)logical_raw.y / PANGO_SCALE;
    double logical_h = (double)logical_raw.height / PANGO_SCALE;

    bool center_ink =
        (style & (TEXT_CENTER | TEXT_VCENTER)) == (TEXT_CENTER | TEXT_VCENTER) && (style & TEXT_BOLD);

    double offset_x = (style & TEXT_CENTER) ? (rect.w - ink_w) * 0.5 - ink_x : 0.0;
    double offset_y = 0.0;

    if (style & TEXT_VCENTER) {
        offset_y = center_ink ? (rect.h - ink_h) * 0.5 - ink_y : (rect.h - logical_h) * 0.5 - logical_y;
    }

    color(backend->paint, value_color);
    cairo_move_to(backend->paint, rect.x + offset_x, rect.y + offset_y);
    pango_cairo_show_layout(backend->paint, backend->layout);
}

static float measure(void *context, const char *value, float size) {
    if (!value || !value[0] || size <= 0.0f) {
        return 0.0f;
    }

    LinuxCanvas *backend = context;
    pango_font_description_set_absolute_size(backend->fonts[0], size * PANGO_SCALE);
    pango_layout_set_font_description(backend->layout, backend->fonts[0]);
    pango_layout_set_text(backend->layout, value, -1);
    pango_layout_set_width(backend->layout, -1);
    pango_layout_set_height(backend->layout, -1);
    pango_layout_set_single_paragraph_mode(backend->layout, TRUE);
    pango_layout_set_ellipsize(backend->layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_alignment(backend->layout, PANGO_ALIGN_LEFT);

    PangoRectangle logical;
    pango_layout_get_extents(backend->layout, NULL, &logical);
    return (float)((double)logical.width / PANGO_SCALE);
}

static void path(void *context, const Point *points, unsigned count, bool closed, bool fill, float width,
                 Color value) {
    if (!points || !count) {
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
    cairo_set_line_width(backend->paint, width > 0.0f ? width : 1.0f);
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
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    LinuxCanvas *backend = context;
    cairo_new_path(backend->paint);
    cairo_save(backend->paint);
    cairo_translate(backend->paint, rect.x + rect.w * 0.5, rect.y + rect.h * 0.5);
    cairo_scale(backend->paint, rect.w * 0.5, rect.h * 0.5);
    cairo_arc(backend->paint, 0.0, 0.0, 1.0, 0.0, 6.283185307179586);
    cairo_restore(backend->paint);

    color(backend->paint, value);
    cairo_set_line_width(backend->paint, width > 0.0f ? width : 1.0f);

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
    float radius;
} ImagePaint;

void linux_canvas_paint_image(LinuxCanvas *backend, const Image *image, Rect rect, float radius) {
    if (!image || !image->pixels || !image->width || !image->height || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    Rect cover = canvas_cover_rect(rect, image->width, image->height);
    cairo_surface_t *surface =
        cairo_image_surface_create_for_data((unsigned char *)image->pixels, CAIRO_FORMAT_RGB24,
                                            (int)image->width, (int)image->height, (int)image->width * 4);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }

    cairo_t *paint = backend->paint;
    cairo_pattern_t *source = cairo_pattern_create_for_surface(surface);
    cairo_matrix_t map;
    double sx = image->width / (double)cover.w;
    double sy = image->height / (double)cover.h;
    cairo_matrix_init(&map, sx, 0.0, 0.0, sy, -cover.x * sx, -cover.y * sy);
    cairo_pattern_set_matrix(source, &map);
    cairo_pattern_set_filter(source, CAIRO_FILTER_GOOD);
    cairo_pattern_set_extend(source, CAIRO_EXTEND_PAD);

    cairo_save(paint);
    cairo_set_source(paint, source);
    rounded(paint, rect, radius);
    cairo_fill(paint);
    cairo_restore(paint);
    cairo_pattern_destroy(source);
    cairo_surface_destroy(surface);
}

static void paint_image(void *context, const Image *image) {
    ImagePaint *target = context;
    linux_canvas_paint_image(target->backend, image, target->rect, target->radius);
}

static bool image(void *context, const char *url, Rect rect, float radius) {
    ImagePaint target = {context, rect, radius};
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

    if ((int)rect.w <= 0 || (int)rect.h <= 0) {
        return false;
    }

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
    cairo_set_antialias(paint, CAIRO_ANTIALIAS_BEST);

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
        backend->fonts[0] = pango_font_description_from_string("Inter");
        backend->fonts[1] = pango_font_description_from_string("Inter Bold");
    }

    *canvas = (Canvas){backend, box,  gradient, text,   path,   ellipse, measure,
                       image,   clip, unclip,   retain, replay, release};
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

static bool ensure_embedded_font(const char *directory, const char *name, const unsigned char *data,
                                 size_t size, char *out, size_t capacity) {
    int length = snprintf(out, capacity, "%s/%s", directory, name);

    if (length <= 0 || (size_t)length >= capacity) {
        return false;
    }

    struct stat info;

    if (stat(out, &info) == 0 && info.st_size == (off_t)size) {
        return true;
    }

    char temp[PLATFORM_PATH_CAPACITY];
    int temp_length = snprintf(temp, sizeof(temp), "%s.tmp.%ld", out, (long)getpid());

    if (temp_length <= 0 || (size_t)temp_length >= sizeof(temp)) {
        return false;
    }

    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (fd < 0) {
        return false;
    }

    size_t written = 0;

    while (written < size) {
        ssize_t chunk = write(fd, data + written, size - written);

        if (chunk <= 0) {
            close(fd);
            unlink(temp);
            return false;
        }

        written += (size_t)chunk;
    }

    if (close(fd) != 0) {
        unlink(temp);
        return false;
    }

    if (rename(temp, out) != 0) {
        unlink(temp);
        return false;
    }

    return true;
}

void linux_load_font(void) {
    char base[PLATFORM_PATH_CAPACITY];

    if (!platform_data_dir(base, sizeof(base))) {
        return;
    }

    char directory[PLATFORM_PATH_CAPACITY];
    int length = snprintf(directory, sizeof(directory), "%s/fonts", base);

    if (length <= 0 || (size_t)length >= sizeof(directory) || !platform_make_dir(directory)) {
        return;
    }

    char font[PLATFORM_PATH_CAPACITY];

    if (ensure_embedded_font(directory, "Inter-Regular.ttf", inter_regular_ttf, inter_regular_ttf_size, font,
                             sizeof(font))) {
        FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)font);
    }

    if (ensure_embedded_font(directory, "Inter-Bold.ttf", inter_bold_ttf, inter_bold_ttf_size, font,
                             sizeof(font))) {
        FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)font);
    }
}
