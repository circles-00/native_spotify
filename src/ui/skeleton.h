#ifndef NATIVE_SKELETON_H
#define NATIVE_SKELETON_H

#include "scene.h"
#include "dark_palette.h"

static inline unsigned skeleton_count(const ListView *list, unsigned placeholders) {
    return list->count + (list->loading && (!list->loaded || list->more) ? placeholders : 0);
}

static inline void skeleton_block(Canvas *canvas, Rect rect, float radius) {
    if (rect.w > 0 && rect.h > 0) {
        canvas->box(canvas->context, rect, radius, DARK_HOVER);
    }
}

static inline void skeleton_row(Canvas *canvas, Rect rect, float cover_x, float cover_size, float text_x) {
    float middle = rect.y + rect.h / 2;

    if (cover_size) {
        skeleton_block(canvas, (Rect){rect.x + cover_x, middle - cover_size / 2, cover_size, cover_size}, 4);
    }

    float width = (rect.w - text_x) * .54f;
    skeleton_block(canvas, (Rect){rect.x + text_x, middle - 13, width, 11}, 4);
    skeleton_block(canvas, (Rect){rect.x + text_x, middle + 6, width * .64f, 8}, 4);
}

static inline void skeleton_card(Canvas *canvas, Rect rect, bool circle, float cover_size) {
    canvas->box(canvas->context, rect, 8, DARK_TILE);
    float side = cover_size ? cover_size : rect.w - 24;
    skeleton_block(canvas, (Rect){rect.x + 12, rect.y + 12, side, side}, circle ? side / 2 : 6);
    skeleton_block(canvas, (Rect){rect.x + 12, rect.y + side + 24, (rect.w - 24) * .76f, 12}, 4);
    skeleton_block(canvas, (Rect){rect.x + 12, rect.y + side + 45, (rect.w - 24) * .54f, 9}, 4);
}

#endif
