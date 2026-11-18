#ifndef NATIVE_WIN32_CANVAS_H
#define NATIVE_WIN32_CANVAS_H

#include "canvas.h"
#include "canvas_raster.h"
#include <windows.h>

typedef struct WindowsTextBatch WindowsTextBatch;

typedef struct WindowsCanvas {
    HDC paint;
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
    RasterRoundClip clips[8];
    unsigned clip_depth;
} WindowsCanvas;

void windows_canvas_begin(WindowsCanvas *backend, HDC paint, float scale, Canvas *canvas);
void windows_canvas_destroy(WindowsCanvas *backend);
void windows_canvas_defer_text(WindowsCanvas *backend, WindowsCanvas *target);
void windows_canvas_flush_text(WindowsCanvas *backend);
bool windows_canvas_present(WindowsCanvas *backend, HDC destination, int width, int height);
void windows_load_font(void);

#endif
