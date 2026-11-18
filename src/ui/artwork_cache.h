#ifndef NATIVE_ARTWORK_CACHE_H
#define NATIVE_ARTWORK_CACHE_H

#include "artwork.h"

void artwork_reset(unsigned session_revision);
void artwork_begin_frame(void);
void artwork_pin(const char *url);
bool artwork_visit(const char *url, void (*paint)(void *, const Image *), void *context);
bool artwork_color(const char *url, uint32_t *color);

#endif
