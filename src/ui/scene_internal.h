#ifndef NATIVE_SCENE_INTERNAL_H
#define NATIVE_SCENE_INTERNAL_H

#include "scene.h"

bool scene_contains(Rect rect, float x, float y);
void scene_open_section(Scene *scene, BrowseKind kind);
void scene_open_collection(Scene *scene, const CatalogItem *item);

#endif
