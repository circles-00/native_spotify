#ifndef NATIVE_ARTWORK_H
#define NATIVE_ARTWORK_H

#include "yyjson.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTWORK_URL_CAPACITY 512
#define ARTWORK_CACHE_LIMIT 64
#define ARTWORK_SIDE_LIMIT 320
#define ARTWORK_SOURCE_LIMIT 2048

typedef struct {
    unsigned width;
    unsigned height;
    uint32_t *pixels;
} Image;

bool artwork_url_valid(const char *url);
void artwork_url_parse(yyjson_val *images, char out[ARTWORK_URL_CAPACITY]);
bool image_dimensions_valid(unsigned width, unsigned height);
bool platform_image_decode(const void *data, size_t size, Image *image);

void artwork_reset(unsigned session_revision);
void artwork_begin_frame(void);
void artwork_pin(const char *url);
bool artwork_visit(const char *url, void (*paint)(void *, const Image *), void *context);
bool artwork_color(const char *url, uint32_t *color);

#endif
