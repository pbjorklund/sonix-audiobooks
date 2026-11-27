#ifndef AUDIOBOOKSHELFPAGE_H
#define AUDIOBOOKSHELFPAGE_H

#include "src/gui/shell/gui.h"
#include "src/system/streaming/abs_episode_card.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *audiobookshelf_screen;

void audiobookshelfpage_init(gui_config_t *cfg);
// Apply a persisted completion to the retained podcast detail and list.
void audiobookshelfpage_episode_finished(const abs_episode_manifest_t *id);

#endif /* AUDIOBOOKSHELFPAGE_H */
