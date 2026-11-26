#include "badge_media_svg_font.h"
#include "badge_media_font.h"
#include "badge_media_serif.h"

struct scaled_font {
    const lv_font_t *base;
    int size;
};

/* Both source fonts have boxes up to 21 × 21. LVGL draws each glyph
 * synchronously on the UI task, so the scaled 2-bpp bitmaps share one buffer. */
static uint8_t bitmap[(34 * 34 + 3) / 4] __attribute__((aligned(4)));

static int scale(int value, int size)
{
    return (value * size + (value < 0 ? -10 : 10)) / 20;
}

static void scale_box(lv_font_glyph_dsc_t *glyph, int size)
{
    int left = scale(glyph->ofs_x, size);
    int bottom = scale(glyph->ofs_y, size);
    int width = scale(glyph->ofs_x + glyph->box_w, size) - left;
    int height = scale(glyph->ofs_y + glyph->box_h, size) - bottom;
    glyph->ofs_x = left;
    glyph->ofs_y = bottom;
    glyph->box_w = !width && glyph->box_w ? 1 : width;
    glyph->box_h = !height && glyph->box_h ? 1 : height;
    glyph->adv_w = scale(glyph->adv_w, size);
}

static bool get_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                      uint32_t letter, uint32_t next)
{
    const struct scaled_font *scaled = font->dsc;
    if (!scaled->base->get_glyph_dsc(scaled->base, glyph, letter, next)) return false;
    scale_box(glyph, scaled->size);
    return true;
}

static const uint8_t *get_bitmap(const lv_font_t *font, uint32_t letter)
{
    const struct scaled_font *scaled = font->dsc;
    lv_font_glyph_dsc_t glyph;
    if (!scaled->base->get_glyph_dsc(scaled->base, &glyph, letter, 0)) return NULL;
    unsigned int source_w = glyph.box_w, source_h = glyph.box_h;
    if (!source_w || !source_h) return NULL;
    const uint8_t *source = scaled->base->get_glyph_bitmap(scaled->base, letter);
    if (!source) return NULL;
    scale_box(&glyph, scaled->size);
    unsigned int pixels = glyph.box_w * glyph.box_h;
    if (pixels > sizeof(bitmap) * 4) return NULL;

    unsigned int step_x = (source_w << 8) / glyph.box_w;
    unsigned int step_y = (source_h << 8) / glyph.box_h;
    unsigned int dst = 0;
    for (unsigned int y = 0, sy = step_y / 2; y < glyph.box_h; ++y, sy += step_y) {
        unsigned int row = (sy >> 8) * source_w;
        for (unsigned int x = 0, sx = step_x / 2; x < glyph.box_w; ++x, sx += step_x) {
            unsigned int src = row + (sx >> 8);
            unsigned int alpha = (source[src / 4] >> (6 - 2 * (src % 4))) & 3;
            if (!(dst % 4)) bitmap[dst / 4] = 0;
            bitmap[dst / 4] |= alpha << (6 - 2 * (dst % 4));
            ++dst;
        }
    }
    return bitmap;
}

#define SCALED_FONT(base, size, height, baseline) { \
    .get_glyph_dsc=get_glyph, .get_glyph_bitmap=get_bitmap, \
    .line_height=((height) * (size) + 10) / 20, \
    .base_line=((baseline) * (size) + 10) / 20, \
    .dsc=&(const struct scaled_font){base, size}, \
}

static const lv_font_t fonts[2][2] = {
    {SCALED_FONT(&badge_text_font, 16, 24, 5), SCALED_FONT(&badge_text_font, 32, 24, 5)},
    {SCALED_FONT(&serif_font, 16, 26, 6), SCALED_FONT(&serif_font, 32, 26, 6)},
};

const lv_font_t *badge_svg_font(unsigned int serif, unsigned int size)
{
    if (size == 20) return serif ? &serif_font : &badge_text_font;
    return &fonts[!!serif][size == 32];
}
