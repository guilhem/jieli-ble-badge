#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "badge_media_svg_font.h"
#include "src/misc/lv_mem.h"

/* Back the SDK font decoder's shared bitmap and scratch rows with the host heap. */
uint8_t *_lv_font_decompr_buf;
void *lv_mem_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void lv_mem_free(void *ptr) { free(ptr); }
void *lv_mem_buf_get(uint32_t size) { return malloc(size); }
void lv_mem_buf_release(void *ptr) { free(ptr); }
uint32_t lv_tick_get(void) { return 0; }

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    int menu = !strcmp(argv[1], "Montserrat");
    const lv_font_t *font = menu ? &lv_font_montserrat_14
                                 : badge_svg_font(!strcmp(argv[1], "Serif"), atoi(argv[2]));
    if (menu) {
        const lv_font_fmt_txt_dsc_t *descriptor = font->dsc;
        puts(descriptor->bitmap_format == LV_FONT_FMT_TXT_COMPRESSED ? "compressed" : "plain");
    }
    printf("%d %d\n", font->line_height, font->base_line);
    unsigned int code, next;
    while (scanf("%u%u", &code, &next) == 2) {
        lv_font_glyph_dsc_t glyph;
        if (!font->get_glyph_dsc(font, &glyph, code, next) || glyph.bpp != (menu ? 4 : 2))
            return 1;
        if (next) {
            printf("%u\n", glyph.adv_w);
            continue;
        }
        printf("%u %u %u %u %d %d ", code, glyph.adv_w, glyph.box_w, glyph.box_h,
               glyph.ofs_x, glyph.ofs_y);
        unsigned int count = glyph.box_w * glyph.box_h;
        if (count) {
            const uint8_t *bitmap = font->get_glyph_bitmap(font, code);
            if (!bitmap) return 1;
            for (unsigned int i = 0; i < count; ++i) {
                unsigned int bit = i * glyph.bpp;
                unsigned int shift = 8 - glyph.bpp - (bit % 8);
                putchar("0123456789abcdef"[(bitmap[bit / 8] >> shift) & ((1u << glyph.bpp) - 1)]);
            }
        }
        putchar('\n');
    }
    lv_mem_free(_lv_font_decompr_buf);
    return 0;
}
