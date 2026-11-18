#ifndef NATIVE_LINUX_CANVAS_H
#define NATIVE_LINUX_CANVAS_H

#include "canvas.h"
#include <pango/pangocairo.h>

typedef struct {
    cairo_t *paint;
    PangoLayout *layout;
    PangoFontDescription *fonts[2];
    cairo_surface_t *retained;
} LinuxCanvas;

void linux_canvas_begin(LinuxCanvas *backend, cairo_t *paint, Canvas *canvas);
void linux_canvas_end(LinuxCanvas *backend);
void linux_canvas_destroy(LinuxCanvas *backend);
void linux_load_font(void);

#endif
