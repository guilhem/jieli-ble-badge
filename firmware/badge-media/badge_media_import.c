#include "badge_media_store.h"

#include <string.h>
#include <strings.h>
#include "system/includes.h"

/* A phone's original JPEG can be much larger than the gallery. Decode its
 * stream in a separate task. Hold one receiver buffer until its bytes have
 * been consumed, and keep only the display-sized result. */
#define MAX_JPEG_INPUT (16u * 1024u * 1024u)

uint16_t badge_media_pixels[BADGE_MEDIA_PIXELS] __attribute__((aligned(4)));

static struct {
    enum badge_media_kind kind;
    uint32_t expected;
    uint32_t received;
} transfer;

static OS_SEM decode_start;
static OS_SEM jpeg_input;
static OS_SEM decode_done;
static int decoder_started;
static const uint8_t *jpeg_chunk;
static size_t jpeg_chunk_size;
static int decode_reply;
static uint32_t clip_to_validate;
static struct badge_media_jpeg_diagnostics jpeg_diag = {"idle", 0, -1};

static int validate_clip(uint32_t size);

struct badge_media_jpeg_diagnostics badge_media_import_jpeg_diagnostics(void)
{
    OS_ENTER_CRITICAL();
    struct badge_media_jpeg_diagnostics snapshot = jpeg_diag;
    OS_EXIT_CRITICAL();
    return snapshot;
}

static void record_jpeg_result(const char *stage, int detail)
{
    OS_ENTER_CRITICAL();
    jpeg_diag.stage = stage;
    jpeg_diag.detail = detail;
    OS_EXIT_CRITICAL();
}

struct jpeg_reader {
    const uint8_t *bytes;
    size_t remaining;
    int closed;
};

static void finish_decode_step(int reply)
{
    decode_reply = reply;
    os_sem_post(&decode_done);
}

static size_t read_jpeg(void *ctx, void *dst, size_t len)
{
    struct jpeg_reader *reader = ctx;
    size_t filled = 0;

    while (filled < len && !reader->closed) {
        if (!reader->remaining) {
            os_sem_pend(&jpeg_input, 0);
            reader->bytes = jpeg_chunk;
            reader->remaining = jpeg_chunk_size;
            if (!reader->remaining) {
                reader->closed = 1;
                break;
            }
        }
        size_t count = len - filled;
        if (count > reader->remaining) {
            count = reader->remaining;
        }
        memcpy((uint8_t *)dst + filled, reader->bytes, count);
        filled += count;
        reader->bytes += count;
        reader->remaining -= count;
        if (!reader->remaining) {
            finish_decode_step(0);
        }
    }
    return filled;
}

static void decode_task(void *unused)
{
    (void)unused;
    for (;;) {
        os_sem_pend(&decode_start, 0);
        if (clip_to_validate) {
            /* Keep AVI parsing and JPEG decode off the Bluetooth stack's task. */
            finish_decode_step(validate_clip(clip_to_validate));
            continue;
        }
        struct jpeg_reader reader = { 0 };
        int result = badge_jpeg_decode_report(read_jpeg, &reader, badge_media_pixels, record_jpeg_result);
        int reply = result ? BADGE_MEDIA_ERR_FORMAT : 0;
        /* A decoder error can leave part of a sender buffer unconsumed. Release
         * it and await EOF so that abort joins the decoder before the UI redraws. */
        if (reader.remaining) {
            finish_decode_step(reply);
        }
        while (!reader.closed) {
            os_sem_pend(&jpeg_input, 0);
            if (!jpeg_chunk_size) {
                break;
            }
            finish_decode_step(reply);
        }
        finish_decode_step(reply);
    }
}

static int start_decoder(uint32_t clip_size)
{
    if (!decoder_started) {
        if (os_sem_create(&decode_start, 0)) {
            return BADGE_MEDIA_ERR_IO;
        }
        if (os_sem_create(&jpeg_input, 0)) {
            os_sem_del(&decode_start, 0);
            return BADGE_MEDIA_ERR_IO;
        }
        if (os_sem_create(&decode_done, 0)) {
            os_sem_del(&jpeg_input, 0);
            os_sem_del(&decode_start, 0);
            return BADGE_MEDIA_ERR_IO;
        }
        if (task_create(decode_task, NULL, "badge_decode")) {
            os_sem_del(&decode_done, 0);
            os_sem_del(&jpeg_input, 0);
            os_sem_del(&decode_start, 0);
            return BADGE_MEDIA_ERR_IO;
        }
        decoder_started = 1;
    }
    clip_to_validate = clip_size;
    os_sem_post(&decode_start);
    return 0;
}

static int feed_jpeg(const void *data, size_t len)
{
    jpeg_chunk = data;
    jpeg_chunk_size = len;
    os_sem_post(&jpeg_input);
    os_sem_pend(&decode_done, 0);
    return decode_reply;
}

static uint32_t transfer_limit(enum badge_media_kind kind)
{
    switch (kind) {
    case BADGE_MEDIA_PHOTO:
        return MAX_JPEG_INPUT;
    case BADGE_MEDIA_CLIP:
        return BADGE_MEDIA_MAX_CLIP;
    default:
        return BADGE_TEXT_MAX;
    }
}

int badge_media_import_begin(const char *name, uint32_t expected)
{
    if (transfer.kind != BADGE_MEDIA_EMPTY) {
        return BADGE_MEDIA_ERR_IO;
    }
    if (!name || strlen(name) >= BADGE_MEDIA_NAME_SIZE || strpbrk(name, "/\\\r\n")) {
        return BADGE_MEDIA_ERR_FORMAT;
    }
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) {
        return BADGE_MEDIA_ERR_FORMAT;
    }
    enum badge_media_kind kind;
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) {
        kind = BADGE_MEDIA_PHOTO;
    } else if (!strcasecmp(ext, ".txt")) {
        kind = BADGE_MEDIA_TEXT;
    } else if (!strcasecmp(ext, ".avi")) {
        kind = BADGE_MEDIA_CLIP;
    } else {
        return BADGE_MEDIA_ERR_FORMAT;
    }
    if (expected > transfer_limit(kind)) {
        return BADGE_MEDIA_ERR_SIZE;
    }

    uint32_t stored_size = kind == BADGE_MEDIA_PHOTO ? sizeof(badge_media_pixels) : expected;
    int ret = badge_media_store_begin(kind, name, stored_size);
    if (ret) {
        return ret;
    }
    if (kind == BADGE_MEDIA_PHOTO) {
        OS_ENTER_CRITICAL();
        jpeg_diag = (struct badge_media_jpeg_diagnostics){"idle", 0, -1};
        OS_EXIT_CRITICAL();
        if (start_decoder(0)) {
            badge_media_store_abort();
            return BADGE_MEDIA_ERR_IO;
        }
    }
    transfer.kind = kind;
    transfer.expected = expected;
    transfer.received = 0;
    return 0;
}

int badge_media_import_write(const void *data, size_t len)
{
    if (transfer.kind == BADGE_MEDIA_EMPTY) {
        return BADGE_MEDIA_ERR_IO;
    }
    uint32_t limit = transfer.expected ? transfer.expected : transfer_limit(transfer.kind);
    if (len > limit - transfer.received) {
        return BADGE_MEDIA_ERR_SIZE;
    }
    if (!len) {
        return 0;
    }

    /* Check the receiver's stream even if the decoder could not allocate its pool. */
    if (transfer.kind == BADGE_MEDIA_PHOTO && transfer.received < 2) {
        const uint8_t *bytes = data;
        OS_ENTER_CRITICAL();
        if (!transfer.received) {
            if (bytes[0] != 0xff) jpeg_diag.soi = 0;
            else if (len >= 2) jpeg_diag.soi = bytes[1] == 0xd8;
        } else if (jpeg_diag.soi == -1) {
            jpeg_diag.soi = bytes[0] == 0xd8;
        }
        OS_EXIT_CRITICAL();
    }
    int ret = transfer.kind == BADGE_MEDIA_PHOTO ? feed_jpeg(data, len) : badge_media_store_append(data, len);
    if (!ret) {
        transfer.received += len;
    }
    return ret;
}

static int validate_clip(uint32_t size)
{
    struct badge_avi avi;
    if (badge_avi_open(&avi, badge_media_store_read_pending, NULL, size)) {
        return BADGE_MEDIA_ERR_FORMAT;
    }
    int ret;
    do {
        ret = badge_avi_next(&avi, badge_media_pixels);
    } while (!ret);
    return ret < 0 ? BADGE_MEDIA_ERR_FORMAT : 0;
}

int badge_media_import_finish(void)
{
    if (transfer.kind == BADGE_MEDIA_EMPTY) {
        return BADGE_MEDIA_ERR_IO;
    }
    uint32_t received = transfer.received;
    if (!received || (transfer.expected && transfer.expected != received)) {
        badge_media_import_abort();
        return BADGE_MEDIA_ERR_FORMAT;
    }

    int ret = 0;
    switch (transfer.kind) {
    case BADGE_MEDIA_PHOTO:
        ret = feed_jpeg(NULL, 0);
        if (!ret) {
            ret = badge_media_store_append(badge_media_pixels, sizeof(badge_media_pixels));
        }
        break;
    case BADGE_MEDIA_TEXT:
        if (badge_media_store_read_pending(NULL, 0, badge_media_pixels, received) != received) {
            ret = BADGE_MEDIA_ERR_IO;
        } else if (!badge_text_valid(badge_media_pixels, received)) {
            ret = BADGE_MEDIA_ERR_FORMAT;
        }
        break;
    case BADGE_MEDIA_CLIP:
        ret = start_decoder(received);
        if (!ret) {
            os_sem_pend(&decode_done, 0);
            ret = decode_reply;
        }
        break;
    default:
        break;
    }
    transfer.kind = BADGE_MEDIA_EMPTY;
    if (ret) {
        badge_media_store_abort();
        return ret;
    }
    return badge_media_store_commit();
}

void badge_media_import_abort(void)
{
    if (transfer.kind == BADGE_MEDIA_PHOTO) {
        feed_jpeg(NULL, 0);
    }
    transfer.kind = BADGE_MEDIA_EMPTY;
    badge_media_store_abort();
}
