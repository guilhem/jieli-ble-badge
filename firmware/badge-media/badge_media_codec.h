#ifndef BADGE_MEDIA_CODEC_H
#define BADGE_MEDIA_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define BADGE_MEDIA_SIDE 144
#define BADGE_MEDIA_PIXELS (144u * 144u)
#define BADGE_TEXT_MAX 512u

/* Reads may be short. Zero means EOF or failure. A read never receives a null destination. */
typedef size_t (*badge_media_read)(void *ctx, void *dst, size_t len);
typedef size_t (*badge_media_read_at)(void *ctx, uint32_t offset, void *dst, size_t len);

/* Baseline JPEG to center-cropped RGB565 (native 16-bit words). A failed decode may
 * have drawn partial pixels. The caller publishes them only after success. */
int badge_jpeg_decode(badge_media_read read, void *ctx, uint16_t *pixels);
/* The optional callback receives the final stage and its result. TJpgDec returns
 * positive JRESULT codes. A failed allocation, size check or EOI check returns -1. */
typedef void (*badge_jpeg_report)(const char *stage, int detail);
int badge_jpeg_decode_report(badge_media_read read, void *ctx, uint16_t *pixels, badge_jpeg_report report);

struct badge_media_jpeg_diagnostics {
    const char *stage;
    int detail;
    int soi; /* Input begins with JPEG SOI: 1 yes, 0 no, -1 unknown. */
};
struct badge_media_jpeg_diagnostics badge_media_import_jpeg_diagnostics(void);
int badge_text_valid(const void *bytes, size_t len);

struct badge_avi {
    uint32_t frame_count;
    uint32_t frame_us;
    badge_media_read_at read;
    void *ctx;
    uint32_t movie_start, movie_end, cursor;
};

/* One silent, 360x360 MJPEG video stream. Frame n starts at n * frame_us.
 * Open verifies the container and frame boundaries. Next decodes one frame,
 * returning 0 for success, 1 at end, or -1 for an invalid frame. */
int badge_avi_open(struct badge_avi *avi, badge_media_read_at read, void *ctx, uint32_t size);
int badge_avi_next(struct badge_avi *avi, uint16_t *pixels);
void badge_avi_rewind(struct badge_avi *avi);

#endif
