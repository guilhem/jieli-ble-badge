#ifndef BADGE_MEDIA_SVG_FONT_H
#define BADGE_MEDIA_SVG_FONT_H

#include "lvgl.h"

extern const lv_font_t badge_text_font;
const lv_font_t *badge_svg_font(unsigned int serif, unsigned int size);

#endif
