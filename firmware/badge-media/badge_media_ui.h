#ifndef BADGE_MEDIA_UI_H
#define BADGE_MEDIA_UI_H

#include "lvgl.h"

unsigned int badge_media_ui_init(lv_obj_t *home, lv_group_t *keys,
                                lv_event_cb_t settings, lv_event_cb_t tools,
                                unsigned char (*receive_blocked)(void));
void badge_media_ui_home(void);
void badge_media_ui_menu(void);
void badge_media_ui_save_brightness(unsigned int percent);
unsigned int badge_media_ui_tick(void);
unsigned int badge_media_diagnostic_read(char *buffer, unsigned int size);

#endif
