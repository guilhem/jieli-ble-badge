#ifndef BADGE_SVG_H
#define BADGE_SVG_H

#include <stddef.h>
#include <stdint.h>

#define BADGE_SVG_MAX 4096u
#define BADGE_SVG_TEXTS 4u
#define BADGE_SVG_VALUES 16u

struct badge_svg_animation {
    uint32_t duration;
    int16_t values[BADGE_SVG_VALUES];
    uint8_t count, loop, freeze;
};

struct badge_svg_text {
    const char *text;
    struct badge_svg_animation motion, fade;
    uint32_t rgb;
    int16_t x, y;
    uint8_t size, serif, anchor, opacity;
};

struct badge_svg_scene {
    struct badge_svg_text texts[BADGE_SVG_TEXTS];
    uint8_t count;
};

/* Compacts decoded text into data. Its contents must outlive the scene. */
int badge_svg_parse(char *data, size_t len, struct badge_svg_scene *scene);

/* Returns nonzero while an animation is active. */
int badge_svg_sample(const struct badge_svg_text *text, uint32_t elapsed_ms,
                     int *x, int *y, uint8_t *opacity);
void badge_svg_place(const struct badge_svg_text *text, int width, int baseline, int *x, int *y);

#endif
