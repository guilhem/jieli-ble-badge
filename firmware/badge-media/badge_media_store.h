#ifndef BADGE_MEDIA_STORE_H
#define BADGE_MEDIA_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "badge_media_codec.h"

/* E87 only. Keep the end of firmware staging and raw writes below this area. */
#define BADGE_MEDIA_FLASH_START 0x387000u
#define BADGE_MEDIA_FLASH_END   0x3be000u
#define BADGE_MEDIA_SLOTS       4
#define BADGE_MEDIA_NAME_SIZE   48
#define BADGE_MEDIA_MAX_CLIP    90000u

enum badge_media_kind {
    BADGE_MEDIA_EMPTY,
    BADGE_MEDIA_PHOTO,
    BADGE_MEDIA_TEXT,
    BADGE_MEDIA_CLIP,
};

enum badge_media_error {
    BADGE_MEDIA_ERR_FORMAT = -1,
    BADGE_MEDIA_ERR_FULL = -2,
    BADGE_MEDIA_ERR_SIZE = -3,
    BADGE_MEDIA_ERR_IO = -4,
    BADGE_MEDIA_ERR_SETUP = -5,
};

struct badge_media_item {
    enum badge_media_kind kind;
    uint32_t size;
    char name[BADGE_MEDIA_NAME_SIZE];
};

extern uint16_t badge_media_pixels[BADGE_MEDIA_PIXELS];

void badge_media_store_init(void);
int badge_media_store_check_space(void);
int badge_media_store_needs_preparation(void);
/* Counts blank sectors per slot and sectors whose blank check failed to read. */
void badge_media_store_stats(unsigned int blank[BADGE_MEDIA_SLOTS], unsigned int *read_errors);
/* Call only after on-device confirmation and with the receiver stopped. */
int badge_media_store_prepare(void);
const struct badge_media_item *badge_media_get(unsigned int slot);
size_t badge_media_read_slot(void *slot_context, uint32_t offset, void *dst, size_t len);
int badge_media_delete(unsigned int slot);

void badge_media_preferences_read(int *selected, unsigned int *brightness_pct);
void badge_media_preferences_write(int selected, unsigned int brightness_pct);

/* One transfer at a time. Call these in order from the receiver task. A zero
 * expected length means unknown. Finish returns the committed slot or an error. */
int badge_media_import_begin(const char *name, uint32_t expected);
int badge_media_import_write(const void *data, size_t len);
int badge_media_import_finish(void);
void badge_media_import_abort(void);

/* Internal storage transaction used by the importer. */
int badge_media_store_begin(enum badge_media_kind kind, const char *name, uint32_t size_hint);
int badge_media_store_append(const void *data, size_t len);
size_t badge_media_store_read_pending(void *ctx, uint32_t offset, void *dst, size_t len);
int badge_media_store_commit(void);
void badge_media_store_abort(void);

#endif
