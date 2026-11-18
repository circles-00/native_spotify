#ifndef NATIVE_CATALOG_H
#define NATIVE_CATALOG_H

#include "browse.h"
#include "yyjson.h"

bool catalog_item_parse(yyjson_val *value, ItemKind kind, CatalogItem *item);
bool catalog_track_has_artist(const CatalogItem *item, const char *id);
bool recent_albums_parse(const char *json, BrowsePage *page);

#endif
