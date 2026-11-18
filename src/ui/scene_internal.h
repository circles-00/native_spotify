#ifndef NATIVE_SCENE_INTERNAL_H
#define NATIVE_SCENE_INTERNAL_H

#include "scene.h"

float scene_player_text_limit(const Scene *scene, bool has_like);
Rect scene_player_like_rect(const Scene *scene, float text_width);
void scene_player_update_text_hits(Scene *scene, const View *view, float title_w, float artist_w);
void scene_open_section(Scene *scene, BrowseKind kind);
void scene_open_collection(Scene *scene, const CatalogItem *item);

#endif
