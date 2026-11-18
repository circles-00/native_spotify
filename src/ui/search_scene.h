#ifndef NATIVE_SEARCH_SCENE_H
#define NATIVE_SEARCH_SCENE_H

#include "scene.h"

void search_scene_layout(Scene *scene, const View *view);
void search_scene_draw(Scene *scene, const View *view, Canvas *canvas);
const CatalogItem *search_scene_item(const Scene *scene, const View *view, HitId id);

#endif
