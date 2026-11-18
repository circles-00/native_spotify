#ifndef NATIVE_CANVAS_RASTER_H
#define NATIVE_CANVAS_RASTER_H

#include "artwork.h"
#include "canvas.h"

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} RasterClipRect;

typedef struct {
    Rect rect;
    float radius;
} RasterRoundClip;

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    RasterClipRect clip;
    float scale;
    const RasterRoundClip *round_clips;
    unsigned round_clip_depth;
} RasterSurface;

void canvas_raster_round_rect(const RasterSurface *surface, Rect rect, float radius, Color top, Color bottom);
void canvas_raster_path(const RasterSurface *surface, const Point *points, unsigned count, bool closed,
                        bool fill, float width, Color value);
void canvas_raster_stroke_circle(const RasterSurface *surface, Rect rect, float width, Color value);
void canvas_raster_image(const RasterSurface *surface, const Image *image, Rect rect);

#endif
