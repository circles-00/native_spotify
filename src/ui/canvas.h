#ifndef NATIVE_CANVAS_H
#define NATIVE_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

/* RGB; box colors may specify opacity in the high byte (zero means opaque). */
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
    ICON_HEART_OUTLINE,
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
    ICON_SETTINGS,
    ICON_LYRICS,
    ICON_EXPAND,
    ICON_COLLAPSE,
    ICON_RADIO,
    ICON_LOGIN,
    ICON_LOGOUT,
    ICON_EXTERNAL_LINK,
    ICON_SPEAKER,
    ICON_PHONE,
    ICON_MUTE
} Icon;

typedef struct {
    void *context;
    void (*box)(void *, Rect, float, Color);
    void (*gradient)(void *, Rect, float, Color, Color);
    void (*text)(void *, const char *, Rect, float, unsigned, Color);
    void (*path)(void *, const Point *, unsigned, bool, bool, float, Color);
    void (*ellipse)(void *, Rect, bool, float, Color);
    float (*measure)(void *, const char *, float);
    bool (*image)(void *, const char *, Rect, float);
    void (*clip)(void *, Rect, float);
    void (*unclip)(void *);
    bool (*retain)(void *, Rect);
    void (*replay)(void *, Rect, float);
    void (*release)(void *);
} Canvas;

enum { TEXT_BOLD = 1, TEXT_INPUT = 2, TEXT_VCENTER = 4, TEXT_CENTER = 8 };

static inline float canvas_corner_radius(Rect bounds, float radius) {
    float limit = (bounds.w < bounds.h ? bounds.w : bounds.h) * 0.5f;
    return radius > 0.0f && limit > 0.0f ? (radius < limit ? radius : limit) : 0.0f;
}

static inline Rect canvas_cover_rect(Rect bounds, unsigned width, unsigned height) {
    float sx = bounds.w / width;
    float sy = bounds.h / height;
    float scale = sx > sy ? sx : sy;
    float w = width * scale;
    float h = height * scale;
    return (Rect){bounds.x + (bounds.w - w) * 0.5f, bounds.y + (bounds.h - h) * 0.5f, w, h};
}

static inline bool canvas_artwork(Canvas *canvas, const char *url, Rect bounds, float radius) {
    return url && url[0] && bounds.w > 0.0f && bounds.h > 0.0f &&
           canvas->image(canvas->context, url, bounds, radius);
}

Color color_mix(Color from, Color to, float amount);
void canvas_icon(Canvas *canvas, Icon icon, Rect rect, Color color);
void canvas_brand_logo(Canvas *canvas, Rect rect);
void canvas_brand_icon_pixels(unsigned size, uint32_t *argb);
void canvas_song_play(Canvas *canvas, Rect art, float radius, float hover, bool playable);

#endif
