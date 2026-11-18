#ifndef NATIVE_SCROLL_H
#define NATIVE_SCROLL_H

#include "canvas.h"

typedef struct {
    float offset;
    float target;
    float from;
    float extent;
    float viewport;
    uint64_t started_ms;
    bool active;
} Scroll;

void scroll_layout(Scroll *scroll, float extent, float viewport);
void scroll_to(Scroll *scroll, float offset, uint64_t now, bool smooth);
bool scroll_tick(Scroll *scroll, uint64_t now);
Rect scroll_thumb(const Scroll *scroll, Rect track);
void scroll_drag(Scroll *scroll, Rect track, float pointer, float grab, uint64_t now);

#endif
