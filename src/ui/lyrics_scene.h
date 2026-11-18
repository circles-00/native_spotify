#ifndef NATIVE_LYRICS_SCENE_H
#define NATIVE_LYRICS_SCENE_H

#include "scene.h"

void lyrics_scene_reset(Scene *scene);
void lyrics_scene_layout(Scene *scene, const View *view);
void lyrics_scene_draw(Scene *scene, const View *view, Canvas *canvas);
Rect lyrics_scene_scroll_track(const Scene *scene);

#endif
