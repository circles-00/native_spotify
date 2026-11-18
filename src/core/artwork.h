#ifndef NATIVE_ARTWORK_H
#define NATIVE_ARTWORK_H

#include "yyjson.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTWORK_URL_CAPACITY 256
#define ARTWORK_SIDE_LIMIT 160
#define ARTWORK_SOURCE_LIMIT 2048
#define ARTWORK_HTTP_LIMIT (1024 * 1024)

typedef struct {
    unsigned width;
    unsigned height;
    uint32_t *pixels;
} Image;

bool artwork_url_valid(const char *url);
void artwork_url_parse(yyjson_val *images, char out[ARTWORK_URL_CAPACITY]);
bool image_dimensions_valid(unsigned width, unsigned height);
bool platform_image_decode(const void *data, size_t size, Image *image);
bool artwork_fetch(const char *url, Image *image, unsigned *retry_after);

#endif
