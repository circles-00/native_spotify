#include "scroll.h"

static float limit(const Scroll *scroll, float offset) {
    float maximum = scroll->extent > scroll->viewport ? scroll->extent - scroll->viewport : 0;
    return offset < 0 ? 0 : offset > maximum ? maximum : offset;
}

void scroll_layout(Scroll *scroll, float extent, float viewport) {
    scroll->extent = extent > 0 ? extent : 0;
    scroll->viewport = viewport > 0 ? viewport : 0;
    scroll->offset = limit(scroll, scroll->offset);
    scroll->target = limit(scroll, scroll->target);
    scroll->from = limit(scroll, scroll->from);
    scroll->active = scroll->active && scroll->offset != scroll->target;
}

void scroll_to(Scroll *scroll, float offset, uint64_t now, bool smooth) {
    if (scroll->active && now > scroll->started_ms) {
        scroll_tick(scroll, now);
    }

    scroll->from = scroll->offset;
    scroll->target = limit(scroll, offset);
    scroll->started_ms = now;
    scroll->active = smooth && scroll->offset != scroll->target;

    if (!scroll->active) {
        scroll->offset = scroll->target;
    }
}

bool scroll_tick(Scroll *scroll, uint64_t now) {
    if (!scroll->active) {
        return false;
    }

    float progress = now > scroll->started_ms ? (float)(now - scroll->started_ms) / 100.0f : 0;

    if (progress >= 1) {
        scroll->offset = scroll->target;
        scroll->active = false;
        return false;
    }

    float inv = 1.0f - progress;
    float ease = 1.0f - inv * inv * inv;
    scroll->offset = limit(scroll, scroll->from + (scroll->target - scroll->from) * ease);
    return true;
}

Rect scroll_thumb(const Scroll *scroll, Rect track) {
    if (scroll->extent <= scroll->viewport || scroll->viewport <= 0 || track.h <= 0) {
        return (Rect){0};
    }

    float height = track.h * scroll->viewport / scroll->extent;

    if (height < 24) {
        height = track.h < 24 ? track.h : 24;
    }

    return (Rect){track.x,
                  track.y + (track.h - height) * scroll->offset / (scroll->extent - scroll->viewport),
                  track.w, height};
}

void scroll_drag(Scroll *scroll, Rect track, float pointer, float grab, uint64_t now) {
    Rect thumb = scroll_thumb(scroll, track);
    float distance = track.h - thumb.h;

    if (distance <= 0 || thumb.h <= 0) {
        return;
    }

    scroll_to(scroll, (pointer - track.y - grab) / distance * (scroll->extent - scroll->viewport), now,
              false);
}
