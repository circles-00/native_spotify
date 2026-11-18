#ifndef NATIVE_CANVAS_H
#define NATIVE_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t Color;
typedef struct {
    float x, y, w, h;
} Rect;
typedef struct {
    float x, y;
} Point;

typedef enum {
    ICON_HOME,
    ICON_SEARCH,
    ICON_LIBRARY,
    ICON_HEART,
    ICON_ALBUM,
    ICON_PLAYLIST,
    ICON_QUEUE,
    ICON_PLAY,
    ICON_PAUSE,
    ICON_PREVIOUS,
    ICON_NEXT,
    ICON_VOLUME,
    ICON_DEVICE,
    ICON_CHEVRON_LEFT,
    ICON_CHEVRON_RIGHT,
    ICON_LINK,
    ICON_REFRESH,
    ICON_MORE,
    ICON_CLOCK,
    ICON_CHECK,
    ICON_PLUS,
    ICON_CLOSE,
    ICON_SHUFFLE,
    ICON_REPEAT,
    ICON_FRIENDS,
    ICON_USER,
    ICON_LYRICS
} Icon;

typedef struct {
    void *context;
    void (*box)(void *, Rect, float, Color);
    void (*gradient)(void *, Rect, float, Color, Color);
    void (*text)(void *, const char *, Rect, float, unsigned, Color);
    void (*path)(void *, const Point *, unsigned, bool, bool, float, Color);
    void (*ellipse)(void *, Rect, bool, float, Color);
    float (*measure)(void *, const char *, float);
    bool (*image)(void *, const char *, Rect);
    void (*clip)(void *, Rect, float);
    void (*unclip)(void *);
    bool (*retain)(void *, Rect);
    void (*replay)(void *, Rect, float);
    void (*release)(void *);
} Canvas;

enum { TEXT_BOLD = 1, TEXT_INPUT = 2, TEXT_VCENTER = 4, TEXT_CENTER = 8 };

Color color_mix(Color from, Color to, float amount);
void canvas_icon(Canvas *canvas, Icon icon, Rect rect, Color color);

#endif
