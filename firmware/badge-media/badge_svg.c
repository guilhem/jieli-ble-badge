#include "badge_svg.h"
#include "yxml.h"

#include <string.h>

#define COORD_MAX 2048
#define COORD_SUBPIXELS 8
#define DURATION_MAX (24u * 60u * 60u * 1000u)

enum attribute {
    A_XMLNS, A_VIEWBOX, A_WIDTH, A_HEIGHT,
    A_X, A_Y, A_FILL, A_OPACITY, A_ANCHOR, A_FAMILY, A_SIZE,
    A_TARGET, A_FROM, A_TO, A_VALUES, A_DUR, A_REPEAT, A_ANIMATE_FILL, A_CALC,
    A_INVALID,
};

#define ATTR_BIT(a) (1u << (a))
#define ATTR_RANGE(first, last) (ATTR_BIT((last) + 1) - ATTR_BIT(first))

struct pending_animation {
    int32_t values[BADGE_SVG_VALUES];
    uint32_t duration;
    uint8_t count, target, loop, freeze;
};

static int space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_space(const char *str)
{
    while (space(*str)) ++str;
    return str;
}

static int digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Parse a signed decimal without bringing floating point into firmware. */
static const char *decimal_parts(const char *str, int32_t *whole, int32_t *fraction,
                                 int *negative)
{
    str = skip_space(str);
    *negative = *str == '-';
    if (*negative || *str == '+') ++str;
    if (!digit(*str) && (*str != '.' || !digit(str[1]))) return NULL;
    int32_t value = 0;
    while (digit(*str)) {
        if (value > (INT32_MAX - 9) / 10) return NULL;
        value = 10 * value + (*str++ - '0');
    }

    *whole = value;
    *fraction = 0;
    if (*str == '.') {
        ++str;
        for (int place = 100; digit(*str); place /= 10) {
            int d = *str++ - '0';
            *fraction += place * d;
            if (!place && d && !*fraction) *fraction = 1;
        }
    }
    return str;
}

static const char *decimal(const char *str, int32_t *thousandths)
{
    int32_t whole, fraction;
    int negative;
    str = decimal_parts(str, &whole, &fraction, &negative);
    if (!str || whole > (INT32_MAX - fraction) / 1000) return NULL;
    int32_t value = whole * 1000 + fraction;
    *thousandths = negative ? -value : value;
    return str;
}

static int number(const char *str, const char *unit, int32_t *value)
{
    const char *end = decimal(str, value);
    return end && (!*skip_space(end) || (unit && !strcmp(end, unit)));
}

static int coordinate(int32_t value)
{
    return value >= -COORD_MAX * 1000 && value <= COORD_MAX * 1000;
}

static int16_t subpixels(int32_t value)
{
    return (int16_t)((value * COORD_SUBPIXELS + (value < 0 ? -500 : 500)) / 1000);
}

static int opacity(int32_t value)
{
    return value >= 0 && value <= 1000;
}

static uint8_t opacity_byte(int32_t value)
{
    return (uint8_t)((value * 255 + 500) / 1000);
}

static int viewbox(const char *value)
{
    static const int32_t expected[] = {0, 0, 360000, 360000};
    for (unsigned i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        if (i) {
            const char *next = skip_space(value);
            if (*next == ',') value = next + 1;
            else if (next == value) return 0;
        }
        int32_t component;
        value = decimal(value, &component);
        if (!value || component != expected[i]) return 0;
    }
    return !*skip_space(value);
}

static int color(const char *value, uint32_t *rgb)
{
    if (value[0] != '#' || strlen(value) != 7) return 0;
    uint32_t result = 0;
    for (unsigned i = 1; i < 7; ++i) {
        unsigned byte = (unsigned char)value[i];
        if (byte >= '0' && byte <= '9') byte -= '0';
        else if ((byte | 32u) >= 'a' && (byte | 32u) <= 'f') byte = (byte | 32u) - 'a' + 10u;
        else return 0;
        result = (result << 4) | byte;
    }
    *rgb = result;
    return 1;
}

static size_t utf8(const char *str, size_t available, uint32_t *codepoint)
{
    unsigned char first = (unsigned char)str[0];
    if (first < 0x80) {
        *codepoint = first;
        return 1;
    }
    if (first < 0xc2 || first > 0xf4) return 0;
    size_t length = first < 0xe0 ? 2u : first < 0xf0 ? 3u : 4u;
    if (length > available) return 0;

    uint32_t value = first & (0x7fu >> length);
    for (size_t i = 1; i < length; ++i) {
        unsigned char byte = (unsigned char)str[i];
        if ((byte & 0xc0) != 0x80) return 0;
        value = (value << 6) | (byte & 0x3f);
    }
    if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) return 0;
    *codepoint = value;
    return length;
}

static int valid_document(const char *data, size_t len)
{
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t width = utf8(data + i, len - i, &cp);
        if (!width || (cp < 32u && !space(data[i])) || cp == 0xfffe || cp == 0xffff) return 0;
        i += width;
    }
    /* The XML declaration is handled internally by yxml. Its input must
     * still declare UTF-8 if an encoding is provided. */
    size_t pos = len >= 3 && !memcmp(data, "\xef\xbb\xbf", 3) ? 3u : 0u;
    if (len - pos > 5 && !memcmp(data + pos, "<?xml", 5) && space(data[pos + 5])) {
        for (pos += 6; len - pos >= 2 && memcmp(data + pos, "?>", 2); ++pos) {
            if (len - pos < 8 || memcmp(data + pos, "encoding", 8)) continue;
            pos += 8;
            while (pos < len && space(data[pos])) ++pos;
            if (pos == len || data[pos++] != '=') return 0;
            while (pos < len && space(data[pos])) ++pos;
            if (len - pos < 7) return 0;
            char quote = data[pos];
            if ((quote != '\'' && quote != '"') || data[pos + 6] != quote ||
                (data[pos + 1] | 32) != 'u' || (data[pos + 2] | 32) != 't' ||
                (data[pos + 3] | 32) != 'f' || data[pos + 4] != '-' || data[pos + 5] != '8') return 0;
            pos += 6;
        }
        if (len - pos < 2) return 0;
        pos += 2;
    }

    /* yxml does not emit a DTD token. Find its opener after any prolog comments. */
    for (;;) {
        while (pos < len && space(data[pos])) ++pos;
        if (len - pos < 4 || memcmp(data + pos, "<!--", 4)) break;
        for (pos += 4; len - pos >= 3 && memcmp(data + pos, "-->", 3); ++pos) {}
        if (len - pos < 3) return 0;
        pos += 3;
    }
    return len - pos < 9 || memcmp(data + pos, "<!DOCTYPE", 9);
}

static int glyph(uint32_t cp)
{
    if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff)) return 1;
    switch (cp) {
    case 0x152: case 0x153: case 0x178:
    case 0x2013: case 0x2014: case 0x2018: case 0x2019:
    case 0x201c: case 0x201d: case 0x2026: case 0x202f: case 0x20ac:
        return 1;
    default:
        return 0;
    }
}

static int valid_text(const char *text, size_t len)
{
    unsigned count = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t width = utf8(text + i, len - i, &cp);
        if (!width || !glyph(cp) || ++count > 128) return 0;
        i += width;
    }
    return count != 0;
}

static enum attribute get_attribute(const char *name, unsigned depth)
{
    static const char *const names[] = {
        "xmlns", "viewBox", "width", "height",
        "x", "y", "fill", "fill-opacity", "text-anchor", "font-family", "font-size",
        "attributeName", "from", "to", "values", "dur", "repeatCount", "fill", "calcMode",
    };
    static const uint8_t first[] = {A_XMLNS, A_X, A_TARGET};
    static const uint8_t last[] = {A_HEIGHT, A_SIZE, A_CALC};
    for (unsigned i = first[depth - 1]; i <= last[depth - 1]; ++i)
        if (!strcmp(name, names[i])) return i;
    return A_INVALID;
}

static int set_text(struct badge_svg_text *text, enum attribute attr, const char *value)
{
    int32_t number_value;
    switch (attr) {
    case A_X:
    case A_Y:
        if (!number(value, "px", &number_value) || !coordinate(number_value)) return 0;
        if (attr == A_X) text->x = subpixels(number_value);
        else text->y = subpixels(number_value);
        return 1;
    case A_FILL:
        return color(value, &text->rgb);
    case A_OPACITY:
        if (!number(value, NULL, &number_value) || !opacity(number_value)) return 0;
        text->opacity = opacity_byte(number_value);
        return 1;
    case A_ANCHOR:
        if (!strcmp(value, "start")) text->anchor = 0;
        else if (!strcmp(value, "middle")) text->anchor = 1;
        else if (!strcmp(value, "end")) text->anchor = 2;
        else return 0;
        return 1;
    case A_FAMILY:
        if (!strcmp(value, "DejaVu Sans")) text->serif = 0;
        else if (!strcmp(value, "DejaVu Serif")) text->serif = 1;
        else return 0;
        return 1;
    case A_SIZE:
        if (!number(value, "px", &number_value) ||
            (number_value != 16000 && number_value != 20000 && number_value != 32000)) return 0;
        text->size = (uint8_t)(number_value / 1000);
        return 1;
    default:
        return 0;
    }
}

static int set_animation(struct pending_animation *animation, enum attribute attr, const char *value)
{
    switch (attr) {
    case A_TARGET:
        if (!strcmp(value, "x")) animation->target = A_X;
        else if (!strcmp(value, "fill-opacity")) animation->target = A_OPACITY;
        else return 0;
        return 1;
    case A_FROM:
    case A_TO:
        return number(value, NULL, &animation->values[attr - A_FROM]);
    case A_VALUES:
        while (animation->count < BADGE_SVG_VALUES) {
            value = decimal(value, &animation->values[animation->count++]);
            if (!value) return 0;
            value = skip_space(value);
            if (!*value) return animation->count >= 2;
            if (*value++ != ';') return 0;
        }
        return 0;
    case A_DUR: {
        int32_t whole, fraction;
        int negative;
        const char *unit = decimal_parts(value, &whole, &fraction, &negative);
        if (!unit || negative) return 0;
        uint32_t time;
        if (!strcmp(unit, "ms")) {
            time = (uint32_t)whole;
        } else if (!strcmp(unit, "s") && (uint32_t)whole <= DURATION_MAX / 1000u) {
            time = (uint32_t)whole * 1000u + (uint32_t)fraction;
        } else {
            return 0;
        }
        if (!time || time > DURATION_MAX) return 0;
        animation->duration = time;
        return 1;
    }
    case A_REPEAT:
        animation->loop = !strcmp(value, "indefinite");
        return animation->loop || !strcmp(value, "1");
    case A_ANIMATE_FILL:
        animation->freeze = !strcmp(value, "freeze");
        return animation->freeze || !strcmp(value, "remove");
    case A_CALC:
        return !strcmp(value, "linear");
    default:
        return 0;
    }
}

static int finish_animation(struct badge_svg_text *text, const struct pending_animation *animation,
                            unsigned seen)
{
    unsigned endpoints = seen & ATTR_RANGE(A_FROM, A_TO);
    if (!animation->target || !animation->duration ||
        (animation->count ? endpoints != 0 : endpoints != ATTR_RANGE(A_FROM, A_TO))) return 0;
    int is_motion = animation->target == A_X;
    struct badge_svg_animation *dest = is_motion ? &text->motion : &text->fade;
    if (dest->count) return 0;
    dest->count = animation->count ? animation->count : 2u;
    for (unsigned i = 0; i < dest->count; ++i) {
        int32_t value = animation->values[i];
        if (is_motion) {
            if (!coordinate(value)) return 0;
            dest->values[i] = subpixels(value);
        } else {
            if (!opacity(value)) return 0;
            dest->values[i] = opacity_byte(value);
        }
    }
    dest->duration = animation->duration;
    dest->loop = animation->loop;
    dest->freeze = animation->freeze;
    return 1;
}

int badge_svg_parse(char *data, size_t len, struct badge_svg_scene *scene)
{
    if (!scene || !data || !len || len > BADGE_SVG_MAX || !valid_document(data, len)) return -1;

    yxml_t xml;
    char names[40];
    yxml_init(&xml, names, sizeof(names));
    memset(scene, 0, sizeof(*scene));
    struct pending_animation animation;
    struct badge_svg_text *text = NULL;
    enum attribute attr = A_INVALID;
    unsigned depth = 0, seen = 0;
    int has_viewbox = 0, pending_space = 0;
    size_t used = 0, written = 0;
    char *value = NULL;

    for (size_t i = 0; i < len; ++i) {
        switch (yxml_parse(&xml, data[i])) {
        case YXML_OK:
            break;
        case YXML_ELEMSTART:
            seen = 0;
            ++depth;
            if (depth == 1 && !strcmp(xml.elem, "svg")) break;
            if (depth == 2 && has_viewbox && !strcmp(xml.elem, "text") && scene->count < BADGE_SVG_TEXTS) {
                text = &scene->texts[scene->count++];
                text->text = data + written;
                text->size = 16;
                text->opacity = 255;
                pending_space = 0;
                break;
            }
            if (depth == 3 && !strcmp(xml.elem, "animate")) {
                memset(&animation, 0, sizeof(animation));
                break;
            }
            return -1;
        case YXML_ATTRSTART:
            attr = get_attribute(xml.attr, depth);
            if (attr == A_INVALID || (seen & ATTR_BIT(attr))) return -1;
            seen |= ATTR_BIT(attr);
            /* Temporarily reuse bytes after the decoded text prefix. */
            value = data + written;
            used = 0;
            break;
        case YXML_ATTRVAL:
            for (const char *p = xml.data; *p; ++p) {
                char c = space(*p) ? ' ' : *p;
                if (attr != A_XMLNS && c == ' ' && (!used || value[used - 1] == ' ')) continue;
                if (written + used > i) return -1;
                value[used++] = c;
            }
            break;
        case YXML_ATTREND:
            if (attr != A_XMLNS && used && value[used - 1] == ' ') --used;
            if (written + used > i) return -1;
            value[used] = 0;
            if (depth == 1) {
                if (attr == A_XMLNS) {
                    if (strcmp(value, "http://www.w3.org/2000/svg")) return -1;
                } else if (attr == A_VIEWBOX) {
                    has_viewbox = viewbox(value);
                    if (!has_viewbox) return -1;
                } else {
                    int32_t size;
                    if (!number(value, "px", &size) || size != 360000) return -1;
                }
            } else if (depth == 2) {
                if (!set_text(text, attr, value)) return -1;
            } else if (!set_animation(&animation, attr, value)) {
                return -1;
            }
            break;
        case YXML_CONTENT:
            for (const char *p = xml.data; *p; ++p) {
                if (depth != 2) {
                    if (!space(*p)) return -1;
                } else if (space(*p)) {
                    pending_space = data + written != text->text;
                } else {
                    /* Only bytes already consumed by yxml can be reused. */
                    if (written + (size_t)pending_space > i) return -1;
                    if (pending_space) data[written++] = ' ';
                    pending_space = 0;
                    data[written++] = *p;
                }
            }
            break;
        case YXML_ELEMEND:
            if (depth == 3 && !finish_animation(text, &animation, seen)) return -1;
            if (depth == 2) {
                if (written > i || !valid_text(text->text, (size_t)(data + written - text->text))) return -1;
                data[written++] = 0;
            }
            --depth;
            break;
        default:
            return -1;
        }
    }
    return yxml_eof(&xml) == YXML_OK && scene->count ? 0 : -1;
}

static int interpolate(const struct badge_svg_animation *animation, uint32_t elapsed,
                       int16_t base, int *active)
{
    if (!animation->count) return base;
    if (animation->loop) {
        elapsed %= animation->duration;
    } else if (elapsed >= animation->duration) {
        return animation->freeze ? animation->values[animation->count - 1] : base;
    }
    *active = 1;

    uint32_t position = elapsed * (animation->count - 1);
    unsigned left = position / animation->duration;
    uint32_t remainder = position % animation->duration;
    int32_t delta = animation->values[left + 1] - animation->values[left];
    int64_t step = (int64_t)delta * remainder;
    step += (delta < 0 ? -(int32_t)animation->duration : (int32_t)animation->duration) / 2;
    return animation->values[left] + (int)(step / (int32_t)animation->duration);
}

int badge_svg_sample(const struct badge_svg_text *text, uint32_t elapsed_ms,
                     int *x, int *y, uint8_t *opacity_out)
{
    int active = 0;
    int position = interpolate(&text->motion, elapsed_ms, text->x, &active);
    *x = (position + (position < 0 ? -COORD_SUBPIXELS / 2 : COORD_SUBPIXELS / 2)) / COORD_SUBPIXELS;
    *y = (text->y + (text->y < 0 ? -COORD_SUBPIXELS / 2 : COORD_SUBPIXELS / 2)) / COORD_SUBPIXELS;
    *opacity_out = (uint8_t)interpolate(&text->fade, elapsed_ms, text->opacity, &active);
    return active;
}

void badge_svg_place(const struct badge_svg_text *text, int width, int baseline, int *x, int *y)
{
    *x -= text->anchor * width / 2;
    *y -= baseline;
}
