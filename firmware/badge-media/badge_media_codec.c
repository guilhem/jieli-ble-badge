#include "badge_media_codec.h"
#include "src/extra/libs/sjpg/tjpgd.h"

#include <stdlib.h>
#include <string.h>

#if JD_FASTDECODE != 0 || JD_FORMAT != 1 || !JD_USE_SCALE
#error "Badge JPEG requires basic entropy decoding, native RGB565 and scaling"
#endif

/* Use the pinned SDK's sequential ChaN R0.03 decoder. The JieLi hardware demo
 * seeks in its source file, but an original phone photo can exceed the flash volume. */
struct photo {
    badge_media_read read;
    void *ctx;
    uint16_t *pixels;
    unsigned orientation, side, left, top;
};

static size_t read_all(badge_media_read read, void *ctx, void *dst, size_t len)
{
    uint8_t *buf = dst;
    size_t used = 0;
    while (used < len) {
        size_t n = read(ctx, buf + used, len - used);
        if (!n || n > len - used) break;
        used += n;
    }
    return used;
}

static int skip_bytes(struct photo *p, size_t len)
{
    uint8_t scratch[96];
    while (len) {
        size_t n = len < sizeof(scratch) ? len : sizeof(scratch);
        if (read_all(p->read, p->ctx, scratch, n) != n) return 0;
        len -= n;
    }
    return 1;
}

static uint16_t u16(const uint8_t *b, int little)
{
    return little ? (uint16_t)(b[0] | b[1] << 8) : (uint16_t)(b[0] << 8 | b[1]);
}

static uint32_t u32(const uint8_t *b, int little)
{
    if (little) return (uint32_t)u16(b + 2, 1) << 16 | u16(b, 1);
    return (uint32_t)u16(b, 0) << 16 | u16(b + 2, 0);
}

/* Read IFD0 from the bounded APP1 block. Embedded EXIF thumbnails are skipped. */
static int read_exif(struct photo *p, size_t len)
{
    uint8_t b[14];
    if (len < sizeof(b)) return skip_bytes(p, len);
    if (read_all(p->read, p->ctx, b, sizeof(b)) != sizeof(b)) return 0;
    len -= sizeof(b);
    int little = !memcmp(b + 6, "II", 2);
    if (memcmp(b, "Exif\0\0", 6) || (!little && memcmp(b + 6, "MM", 2)) ||
        u16(b + 8, little) != 42) return skip_bytes(p, len);

    uint32_t offset = u32(b + 10, little);
    if (offset < 8 || offset - 8 > len || len - (offset - 8) < 2)
        return skip_bytes(p, len);
    if (!skip_bytes(p, offset - 8) || read_all(p->read, p->ctx, b, 2) != 2) return 0;
    len -= offset - 8 + 2;
    unsigned entries = u16(b, little);
    for (unsigned i = 0; i < entries && len >= 12; ++i) {
        if (read_all(p->read, p->ctx, b, 12) != 12) return 0;
        len -= 12;
        if (u16(b, little) == 0x112 && u16(b + 2, little) == 3 && u32(b + 4, little) == 1) {
            unsigned value = u16(b + 8, little);
            if (value >= 1 && value <= 8) p->orientation = value;
            break;
        }
    }
    return skip_bytes(p, len);
}

static size_t jpeg_input(JDEC *jd, uint8_t *dst, size_t n)
{
    struct photo *p = jd->device;
    if (dst) return read_all(p->read, p->ctx, dst, n);
    int ok = jd->input_marker == 0xffe1 ? read_exif(p, n) : skip_bytes(p, n);
    return ok ? n : 0;
}

static unsigned first_pixel(unsigned src, unsigned origin, unsigned side)
{
    if (src <= origin) return 0;
    if (src >= origin + side) return BADGE_MEDIA_SIDE;
    return ((src - origin) * BADGE_MEDIA_SIDE + side - 1) / side;
}

static int jpeg_output(JDEC *jd, void *data, JRECT *r)
{
    struct photo *p = jd->device;
    const uint16_t *block = data;
    enum { n = BADGE_MEDIA_SIDE };
    /* Offset, column step, row step for each TIFF orientation. */
    static const int16_t transform[8][3] = {
        {0, 1, n}, {n-1, -1, n}, {n*n-1, -1, -n}, {n*(n-1), 1, -n},
        {0, n, 1}, {n-1, n, -1}, {n*n-1, -n, -1}, {n*(n-1), -n, 1}
    };
    const int16_t *t = transform[p->orientation - 1];
    unsigned x0 = first_pixel(r->left, p->left, p->side);
    unsigned x1 = first_pixel(r->right + 1u, p->left, p->side);
    unsigned y0 = first_pixel(r->top, p->top, p->side);
    unsigned y1 = first_pixel(r->bottom + 1u, p->top, p->side);
    if (x0 == x1 || y0 == y1) return 1;
    for (unsigned y = y0; y < y1; ++y) {
        unsigned sy = y * p->side / n + p->top - r->top;
        const uint16_t *row = block + sy * (r->right - r->left + 1);
        int out = t[0] + (int)y * t[2] + (int)x0 * t[1];
        for (unsigned x = x0; x < x1; ++x) {
            p->pixels[out] = row[x * p->side / n + p->left - r->left];
            out += t[1];
        }
    }
    return 1;
}

/* In the SDK's basic TJpgDec configuration dptr points at the last consumed
 * entropy byte and dctr counts bytes buffered after it. Validate the actual EOI. */
static int jpeg_eoi(JDEC *jd)
{
    struct photo *p = jd->device;
    uint8_t b;
    int prefix = 0;
    do {
        if (jd->dctr) {
            b = *++jd->dptr;
            --jd->dctr;
        } else if (read_all(p->read, p->ctx, &b, 1) != 1) {
            return 0;
        }
        if (b != 0xff) return prefix && b == 0xd9;
        prefix = 1;
    } while (1);
}

static int decode(badge_media_read read, void *ctx, uint16_t *pixels, int avi, badge_jpeg_report report)
{
    if (!read || !pixels) return -1;
    struct photo p = {read, ctx, pixels, 1, 0, 0, 0};
    JDEC jd;
    /* Keep this off the small firmware task stacks. */
    const size_t pool_size = 4096;
    void *pool = malloc(pool_size);
    const char *stage = "alloc";
    int detail = -1;
    if (!pool) goto done;
    stage = "prepare";
    detail = jd_prepare(&jd, jpeg_input, pool, pool_size, &p);
    if (detail != JDR_OK) goto done;
    if ((avi && (jd.width != 360 || jd.height != 360)) ||
        (uint32_t)jd.width * jd.height > 64u * 1024u * 1024u) {
        stage = "dimensions";
        detail = -1;
        goto done;
    }
    unsigned side = jd.width < jd.height ? jd.width : jd.height;
    unsigned shift = 0;
    while (shift < 3 && (side >> (shift + 1)) >= BADGE_MEDIA_SIDE) ++shift;
    p.side = side >> shift;
    p.left = ((jd.width >> shift) - p.side) / 2;
    p.top = ((jd.height >> shift) - p.side) / 2;
    stage = "decompress";
    detail = jd_decomp(&jd, jpeg_output, shift);
    if (detail != JDR_OK) goto done;
    if (!jpeg_eoi(&jd)) {
        stage = "eoi";
        detail = -1;
        goto done;
    }
    stage = "done";
done:
    if (pool) free(pool);
    if (report) report(stage, detail);
    return detail ? -1 : 0;
}

int badge_jpeg_decode(badge_media_read read, void *ctx, uint16_t *pixels)
{
    return decode(read, ctx, pixels, 0, NULL);
}

int badge_jpeg_decode_report(badge_media_read read, void *ctx, uint16_t *pixels, badge_jpeg_report report)
{
    return decode(read, ctx, pixels, 0, report);
}

/* RFC 3629. Labels may span lines, but control strings and embedded NULs
 * cannot be displayed as their file bytes. */
int badge_text_valid(const void *bytes, size_t len)
{
    if (!bytes || !len || len > BADGE_TEXT_MAX) return 0;
    const uint8_t *p = bytes;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i++];
        if (c < 0x80) {
            if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return 0;
            continue;
        }
        if (c < 0xc2 || c > 0xf4) return 0;
        unsigned n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        if (len - i < n) return 0;
        unsigned first = p[i];
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) return 0;
        while (n--) if ((p[i++] & 0xc0) != 0x80) return 0;
    }
    return 1;
}

struct chunk {
    uint8_t id[4];
    uint32_t data, size, next;
};

static int avi_read(const struct badge_avi *a, uint32_t pos, void *dst, size_t len)
{
    uint8_t *b = dst;
    while (len) {
        size_t n = a->read(a->ctx, pos, b, len);
        if (!n || n > len) return 0;
        pos += (uint32_t)n;
        b += n;
        len -= n;
    }
    return 1;
}

static int read_chunk(const struct badge_avi *a, uint32_t pos, uint32_t end, struct chunk *c)
{
    uint8_t b[8];
    if (pos > end || end - pos < sizeof(b) || !avi_read(a, pos, b, sizeof(b))) return 0;
    memcpy(c->id, b, 4);
    c->size = u32(b + 4, 1);
    c->data = pos + 8;
    if (c->size > end - c->data) return 0;
    c->next = c->data + c->size;
    if (c->size & 1) {
        if (c->next == end) return 0;
        ++c->next;
    }
    return 1;
}

static int list_type(const struct badge_avi *a, const struct chunk *c, uint8_t *type)
{
    return c->size >= 4 && avi_read(a, c->data, type, 4);
}

static int stream_header(const struct badge_avi *a, const struct chunk *list,
                         uint32_t *count, uint32_t *frame_us)
{
    uint8_t b[56];
    uint32_t end = list->data + list->size;
    int header = 0, format = 0;
    for (uint32_t pos = list->data + 4; pos < end;) {
        struct chunk c;
        if (!read_chunk(a, pos, end, &c)) return 0;
        if (!memcmp(c.id, "strh", 4)) {
            if (header || c.size < 48 || !avi_read(a, c.data, b, 48) ||
                memcmp(b, "vidsMJPG", 8)) return 0;
            uint32_t scale = u32(b + 20, 1), rate = u32(b + 24, 1);
            if (!scale || !rate || rate < scale || (uint64_t)rate > (uint64_t)scale * 12) return 0;
            *frame_us = (uint32_t)(((uint64_t)scale * 1000000 + rate / 2) / rate);
            *count = u32(b + 32, 1);
            header = 1;
        } else if (!memcmp(c.id, "strf", 4)) {
            if (format || c.size < 40 || !avi_read(a, c.data, b, 40) ||
                u32(b, 1) < 40 || u32(b, 1) > c.size ||
                u32(b + 4, 1) != 360 || u32(b + 8, 1) != 360 ||
                u16(b + 12, 1) != 1 || u16(b + 14, 1) != 24 ||
                memcmp(b + 16, "MJPG", 4)) return 0;
            format = 1;
        }
        pos = c.next;
    }
    return header && format;
}

static int avi_header(struct badge_avi *a, const struct chunk *list)
{
    uint8_t b[40];
    uint32_t end = list->data + list->size, count = 0, frame_us = 0;
    int header = 0, stream = 0;
    for (uint32_t pos = list->data + 4; pos < end;) {
        struct chunk c;
        if (!read_chunk(a, pos, end, &c)) return 0;
        if (!memcmp(c.id, "avih", 4)) {
            /* The existing encoder emits a 52-byte avih, the spec uses 56.
             * Both contain all fields through width and height (40 bytes). */
            if (header || c.size < sizeof(b) || !avi_read(a, c.data, b, sizeof(b)) ||
                u32(b + 24, 1) != 1 || u32(b + 32, 1) != 360 || u32(b + 36, 1) != 360)
                return 0;
            a->frame_us = u32(b, 1);
            a->frame_count = u32(b + 16, 1);
            header = 1;
        } else if (!memcmp(c.id, "LIST", 4)) {
            if (!list_type(a, &c, b)) return 0;
            if (!memcmp(b, "strl", 4)) {
                if (stream || !stream_header(a, &c, &count, &frame_us)) return 0;
                stream = 1;
            }
        }
        pos = c.next;
    }
    return header && stream && count && count == a->frame_count &&
           frame_us == a->frame_us;
}

int badge_avi_open(struct badge_avi *a, badge_media_read_at read, void *ctx, uint32_t size)
{
    if (!a || !read || size < 12) return -1;
    *a = (struct badge_avi){ .read = read, .ctx = ctx };
    uint8_t b[12];
    if (!avi_read(a, 0, b, sizeof(b)) || memcmp(b, "RIFF", 4) ||
        u32(b + 4, 1) != size - 8 || memcmp(b + 8, "AVI ", 4)) return -1;
    int header = 0;
    for (uint32_t pos = 12; pos < size;) {
        struct chunk c;
        if (!read_chunk(a, pos, size, &c)) return -1;
        if (!memcmp(c.id, "LIST", 4)) {
            if (!list_type(a, &c, b)) return -1;
            if (!memcmp(b, "hdrl", 4)) {
                if (header || !avi_header(a, &c)) return -1;
                header = 1;
            } else if (!memcmp(b, "movi", 4)) {
                if (a->movie_start) return -1;
                a->movie_start = c.data + 4;
                a->movie_end = c.data + c.size;
            }
        }
        pos = c.next;
    }
    if (!header || !a->movie_start) return -1;
    uint32_t frames = 0;
    for (uint32_t pos = a->movie_start; pos < a->movie_end;) {
        struct chunk c;
        if (!read_chunk(a, pos, a->movie_end, &c) || memcmp(c.id, "00dc", 4) ||
            c.size < 4 || !avi_read(a, c.data, b, 2) || b[0] != 0xff || b[1] != 0xd8 ||
            !avi_read(a, c.data + c.size - 2, b, 2) || b[0] != 0xff || b[1] != 0xd9)
            return -1;
        ++frames;
        pos = c.next;
    }
    a->cursor = a->movie_start;
    return frames == a->frame_count ? 0 : -1;
}

struct frame_source {
    const struct badge_avi *avi;
    uint32_t pos, end;
};

static size_t frame_read(void *ctx, void *dst, size_t len)
{
    struct frame_source *f = ctx;
    uint32_t left = f->end - f->pos;
    if (len > left) len = left;
    if (!len) return 0;
    size_t n = f->avi->read(f->avi->ctx, f->pos, dst, len);
    if (n > len) return 0;
    f->pos += n;
    return n;
}

int badge_avi_next(struct badge_avi *a, uint16_t *pixels)
{
    if (!a || !a->read || !pixels) return -1;
    if (a->cursor == a->movie_end) return 1;
    struct chunk c;
    if (!read_chunk(a, a->cursor, a->movie_end, &c) || memcmp(c.id, "00dc", 4)) return -1;
    struct frame_source f = { a, c.data, c.data + c.size };
    if (decode(frame_read, &f, pixels, 1, NULL)) return -1;
    a->cursor = c.next;
    return 0;
}

void badge_avi_rewind(struct badge_avi *a)
{
    a->cursor = a->movie_start;
}
