#ifndef NATIVE_PANEL_SCENE_H
#define NATIVE_PANEL_SCENE_H

#include "scene.h"

void panel_scene_layout(Scene *scene, const View *view);
void panel_scene_draw(Scene *scene, const View *view, Canvas *canvas);
void panel_scene_toggle(Scene *scene, SidePanel panel);

#endif
