#ifndef NATIVE_WIN32_CANVAS_H
#define NATIVE_WIN32_CANVAS_H

#include "canvas.h"
#include "canvas_raster.h"
#include <windows.h>

typedef struct WindowsTextBatch WindowsTextBatch;

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous;
    uint32_t *pixels;
    int width, height;
} WindowsBuffer;

typedef struct WindowsCanvas {
    HDC paint;
    uint32_t *pixels;
    int width, height;
    float scale;
    HFONT fonts[80][2];
    HDC retained;
    HBITMAP retained_image;
    HGDIOBJ retained_original;
    int retained_width, retained_height;
    struct WindowsCanvas *text_target;
    WindowsTextBatch *text_batch;
    bool native_text;
    HDC frame;
    HBITMAP frame_image;
    HGDIOBJ frame_original;
    int frame_width, frame_height;
    RasterRoundClip clips[RASTER_CLIP_LIMIT];
    unsigned clip_depth;
} WindowsCanvas;

bool windows_buffer_resize(WindowsBuffer *buffer, HDC compatible, int width, int height);
void windows_buffer_destroy(WindowsBuffer *buffer);
void windows_canvas_begin(WindowsCanvas *backend, const WindowsBuffer *buffer, float scale, Canvas *canvas);
bool windows_canvas_paint_image(WindowsCanvas *backend, const Image *image, Rect rect, float radius);
void windows_canvas_destroy(WindowsCanvas *backend);
void windows_canvas_defer_text(WindowsCanvas *backend, WindowsCanvas *target);
void windows_canvas_flush_text(WindowsCanvas *backend);
bool windows_canvas_present(WindowsCanvas *backend, HDC destination, int width, int height);
void windows_load_font(void);

#endif
