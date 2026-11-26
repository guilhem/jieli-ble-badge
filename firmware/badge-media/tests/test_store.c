#include "badge_media_store.h"
#include "badge_svg.h"
#include "system/includes.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[BADGE_MEDIA_FLASH_END - BADGE_MEDIA_FLASH_START];
static uint8_t prefs[16];
static uint16_t prefs_size;
static unsigned int pref_writes;
static int fail_after = -1;
static uint32_t unreadable_addr = UINT32_MAX;
static uint32_t erase_noop_addr = UINT32_MAX;
static unsigned int bytes_read;
static unsigned int erases;
static unsigned int watchdog_kicks;
pthread_mutex_t badge_test_critical = PTHREAD_MUTEX_INITIALIZER;
static int fail_decode_pool;

void *badge_test_decode_malloc(size_t size)
{
    return fail_decode_pool ? NULL : malloc(size);
}

static uint8_t *flash_at(uint32_t addr, uint32_t size)
{
    assert(addr >= BADGE_MEDIA_FLASH_START && addr < BADGE_MEDIA_FLASH_END);
    assert(size <= BADGE_MEDIA_FLASH_END - addr);
    return flash + addr - BADGE_MEDIA_FLASH_START;
}

int tzflash_read(void *dst, uint32_t size, uint32_t addr)
{
    bytes_read += size;
    if (addr == unreadable_addr) return -1;
    memcpy(dst, flash_at(addr, size), size);
    return size;
}

int tzflash_write(const void *data, uint32_t size, uint32_t addr)
{
    uint8_t *dst = flash_at(addr, size);
    const uint8_t *src = data;
    if (fail_after == 0) {
        /* The SDK reports the requested size even when programming failed. */
        fail_after = -1;
        return size;
    }
    if (fail_after > 0) --fail_after;
    for (uint32_t i = 0; i < size; ++i) {
        assert((dst[i] & src[i]) == src[i]);
        dst[i] &= src[i];
    }
    return size;
}

int tzflash_ioctl(uint32_t cmd, uint32_t addr)
{
    assert(cmd == 200 && !(addr & 4095));
    ++erases;
    assert(watchdog_kicks == erases);
    if (addr == erase_noop_addr) return 0;
    memset(flash_at(addr, 4096), 255, 4096);
    return 0;
}

void wdt_clear(void)
{
    ++watchdog_kicks;
}

int syscfg_read(uint16_t id, void *dst, uint16_t size)
{
    assert(id == 48);
    if (!prefs_size || size < prefs_size) return -1;
    memcpy(dst, prefs, prefs_size);
    return prefs_size;
}

int syscfg_write(uint16_t id, const void *src, uint16_t size)
{
    assert(id == 48 && size <= sizeof(prefs));
    memcpy(prefs, src, size);
    prefs_size = size;
    ++pref_writes;
    return size;
}

static void (*decoder_task)(void *);

static void *run_task(void *arg)
{
    decoder_task(arg);
    return NULL;
}

int task_create(void (*task)(void *), void *arg, const char *name)
{
    pthread_t thread;
    assert(strcmp(name, "badge_decode") == 0 && !decoder_task);
    decoder_task = task;
    assert(!pthread_create(&thread, NULL, run_task, arg));
    return pthread_detach(thread);
}

static void send_text(void)
{
    const char message[] = "Caf\xc3\xa9";
    assert(badge_media_import_begin("welcome.txt", sizeof(message) - 1) == 0);
    assert(!badge_media_import_write(message, 4));
    assert(!badge_media_import_write(message + 4, sizeof(message) - 5));
}

static size_t stream_file(const char *path, const char *name)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!badge_media_import_begin(name, 0));
    uint8_t packet[937];
    size_t total = 0;
    size_t n;
    while ((n = fread(packet, 1, sizeof(packet), file)) != 0) {
        assert(!badge_media_import_write(packet, n));
        total += n;
    }
    assert(!ferror(file));
    fclose(file);
    return total;
}

static void test_gallery_hint(void)
{
    memset(flash, 255, sizeof(flash));
    badge_media_store_init();
    unreadable_addr = BADGE_MEDIA_FLASH_START + 128;
    assert(!badge_media_store_needs_preparation());
    unreadable_addr = UINT32_MAX;
    bytes_read = 0;
    assert(!badge_media_store_needs_preparation());
    assert(bytes_read >= sizeof(flash));
    bytes_read = 0;
    assert(!badge_media_store_needs_preparation() && bytes_read == 0);

    /* A cached blank hint must never authorize a write over foreign data. */
    flash[sizeof(flash) - 1] = 0;
    assert(badge_media_store_begin(BADGE_MEDIA_CLIP, "clip", 1) == BADGE_MEDIA_ERR_SETUP);
    assert(!erases && flash[sizeof(flash) - 1] == 0);
    unreadable_addr = BADGE_MEDIA_FLASH_START;
    assert(badge_media_store_check_space() == BADGE_MEDIA_ERR_IO);
    unreadable_addr = UINT32_MAX;
    assert(badge_media_store_needs_preparation());
    bytes_read = 0;
    assert(badge_media_store_needs_preparation() && bytes_read == 0);

    assert(!badge_media_store_prepare());
    assert(!badge_media_store_needs_preparation());
    assert(!badge_media_store_begin(BADGE_MEDIA_TEXT, "note", 1));
    badge_media_store_abort();
    bytes_read = 0;
    assert(!badge_media_store_needs_preparation() && bytes_read > 0);
    assert(erases == 1);
    erases = watchdog_kicks = 0;
}

static void test_prepare(const char *photo)
{
    /* The upgraded E87 had foreign bytes in every one of its 55 sectors. */
    memset(flash, 0x5a, sizeof(flash));
    badge_media_store_init();
    for (unsigned i = 0; i < BADGE_MEDIA_SLOTS; ++i) assert(!badge_media_get(i));
    assert(badge_media_store_check_space() == BADGE_MEDIA_ERR_SETUP);
    assert(badge_media_store_needs_preparation());
    assert(badge_media_import_begin("Android.jpg", 0) == BADGE_MEDIA_ERR_SETUP);
    assert(erases == 0);
    unsigned int blanks[BADGE_MEDIA_SLOTS], errors;
    badge_media_store_stats(blanks, &errors);
    assert(!errors);
    for (unsigned i = 0; i < BADGE_MEDIA_SLOTS; ++i) assert(!blanks[i]);

    unreadable_addr = BADGE_MEDIA_FLASH_START;
    badge_media_store_stats(blanks, &errors);
    assert(errors == 1 && !blanks[0]);
    assert(badge_media_store_check_space() == BADGE_MEDIA_ERR_IO);
    assert(badge_media_store_prepare() == BADGE_MEDIA_ERR_IO);
    assert(erases == 0);
    unreadable_addr = UINT32_MAX;

    /* A controller may report success without erasing the sector. */
    erase_noop_addr = BADGE_MEDIA_FLASH_START + 11 * 4096;
    assert(badge_media_store_prepare() == BADGE_MEDIA_ERR_IO);
    assert(erases == 12);
    badge_media_store_init();
    assert(!badge_media_store_check_space() && badge_media_store_needs_preparation());
    erase_noop_addr = UINT32_MAX;
    assert(!badge_media_store_prepare() && erases == 56);
    badge_media_store_stats(blanks, &errors);
    assert(!errors && blanks[0] == 11 && blanks[1] == 11 && blanks[2] == 11 && blanks[3] == 22);
    assert(!badge_media_store_prepare() && erases == 56);
    assert(!badge_media_store_check_space());
    assert(!badge_media_store_needs_preparation());
    assert(stream_file(photo, "Android.jpg") > sizeof(flash));
    assert(badge_media_store_prepare() == BADGE_MEDIA_ERR_IO);
    assert(badge_media_import_finish() == 0);

    /* Only the first and last sectors of the remaining slots are dirty. */
    flash[11 * 4096] = 0;
    flash[sizeof(flash) - 1] = 0;
    assert(!badge_media_store_prepare() && erases == 58);
    badge_media_store_init();
    assert(badge_media_get(0) && badge_media_get(0)->kind == BADGE_MEDIA_PHOTO);
    assert(!strcmp(badge_media_get(0)->name, "Android.jpg"));
    assert(flash[11 * 4096] == 255 && flash[sizeof(flash) - 1] == 255);

    /* A boot read failure can hide a valid item from the in-memory gallery. */
    unreadable_addr = BADGE_MEDIA_FLASH_START;
    badge_media_store_init();
    assert(!badge_media_get(0));
    unreadable_addr = UINT32_MAX;
    assert(!badge_media_store_prepare() && erases == 58);
    send_text();
    assert(badge_media_import_finish() == 1);
    badge_media_store_init();
    assert(badge_media_get(0) && badge_media_get(0)->kind == BADGE_MEDIA_PHOTO);
}

static void test_svg_import(void)
{
    static const char svg[] = "<svg viewBox=\"0 0 360 360\">"
                             "<text x=\"180\" y=\"180\" fill=\"#FFFFFF\">Guilhem &amp; Caf\xc3\xa9"
                             "<animate attributeName=\"x\" from=\"180\" to=\"200\" dur=\"1s\"/>"
                             "</text></svg>";
    static const char unsupported[] = "<svg viewBox=\"0 0 360 360\"><image/></svg>";
    const size_t size = sizeof(svg) - 1;
    uint8_t previous[11 * 4096];

    memset(flash, 255, sizeof(flash));
    badge_media_store_init();
    send_text();
    assert(badge_media_import_finish() == 0);
    memcpy(previous, flash, sizeof(previous));

    assert(badge_media_import_begin("oversize.svg", BADGE_SVG_MAX + 1) == BADGE_MEDIA_ERR_SIZE);
    assert(!badge_media_import_begin("short.svg", size));
    assert(!badge_media_import_write(svg, size - 1));
    assert(badge_media_import_finish() == BADGE_MEDIA_ERR_FORMAT);
    assert(!badge_media_import_begin("cancelled.svg", 0));
    assert(!badge_media_import_write(svg, size));
    badge_media_import_abort();
    assert(!badge_media_import_begin("image.svg", 0));
    assert(!badge_media_import_write(unsupported, sizeof(unsupported) - 1));
    assert(badge_media_import_finish() == BADGE_MEDIA_ERR_FORMAT);
    badge_media_store_init();
    assert(badge_media_get(0)->kind == BADGE_MEDIA_TEXT && !badge_media_get(1));
    assert(!memcmp(previous, flash, sizeof(previous)));

    assert(!badge_media_import_begin("badge.SVG", 0));
    assert(!badge_media_import_write(svg, 7));
    assert(!badge_media_import_write(svg + 7, size - 7));
    assert(badge_media_import_finish() == 1);
    badge_media_store_init();
    assert(badge_media_get(1)->kind == BADGE_MEDIA_SVG && badge_media_get(1)->size == size);
    char stored[sizeof(svg)];
    assert(badge_media_read_slot((void *)(uintptr_t)1, 0, stored, size) == size);
    assert(!memcmp(stored, svg, size));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    test_gallery_hint();
    test_prepare(argv[1]);
    memset(flash, 255, sizeof(flash));
    badge_media_store_init();
    int selected;
    unsigned int brightness;
    badge_media_preferences_read(&selected, &brightness);
    assert(selected == -1 && brightness == 50);

    send_text();
    assert(badge_media_import_finish() == 0);
    badge_media_preferences_write(0, 30);
    badge_media_preferences_write(0, 30);
    assert(pref_writes == 1);

    uint8_t *factory = flash_at(BADGE_MEDIA_FLASH_START + 11 * 4096 + 9000, 1);
    *factory = 0;
    send_text();
    badge_media_import_abort();
    badge_media_store_init();
    assert(badge_media_get(0) && !badge_media_get(1) && !badge_media_get(2));
    badge_media_preferences_read(&selected, &brightness);
    assert(selected == 0 && brightness == 30 && *factory == 0);

    send_text();
    fail_after = 1;
    assert(badge_media_import_finish() == BADGE_MEDIA_ERR_IO);
    badge_media_store_init();
    assert(badge_media_get(0) && !badge_media_get(2));

    assert(!badge_media_import_begin("broken.jpg", 0));
    assert(badge_media_import_write("bad", 3) == BADGE_MEDIA_ERR_FORMAT);
    badge_media_import_abort();
    assert(!badge_media_get(2));
    struct badge_media_jpeg_diagnostics jpeg = badge_media_import_jpeg_diagnostics();
    assert(!strcmp(jpeg.stage, "prepare") && jpeg.detail == 6 && jpeg.soi == 0);

    assert(!badge_media_import_begin("short.jpg", 0));
    jpeg = badge_media_import_jpeg_diagnostics();
    assert(!strcmp(jpeg.stage, "idle") && jpeg.soi == -1);
    assert(!badge_media_import_write("\xff", 1));
    assert(badge_media_import_jpeg_diagnostics().soi == -1);
    assert(!badge_media_import_write("\xd8", 1));
    assert(badge_media_import_finish() == BADGE_MEDIA_ERR_FORMAT);
    jpeg = badge_media_import_jpeg_diagnostics();
    assert(!strcmp(jpeg.stage, "prepare") && jpeg.detail == 2 && jpeg.soi == 1);

    /* Match the first BODY refusal even when the decoder never asks for a byte. */
    fail_decode_pool = 1;
    assert(!badge_media_import_begin("photo.jpg", 0));
    assert(badge_media_import_write("\xff\xd8", 2) == BADGE_MEDIA_ERR_FORMAT);
    badge_media_import_abort();
    jpeg = badge_media_import_jpeg_diagnostics();
    assert(!strcmp(jpeg.stage, "alloc") && jpeg.detail == -1 && jpeg.soi == 1);
    fail_decode_pool = 0;

    assert(stream_file(argv[1], "portrait.JPG") > sizeof(flash));
    assert(badge_media_import_finish() == 2);
    jpeg = badge_media_import_jpeg_diagnostics();
    assert(!strcmp(jpeg.stage, "done") && jpeg.detail == 0 && jpeg.soi == 1);
    badge_media_store_init();
    assert(badge_media_get(2)->size == sizeof(badge_media_pixels));

    send_text();
    assert(badge_media_import_finish() == 3);
    assert(badge_media_import_begin("full.txt", 0) == BADGE_MEDIA_ERR_SETUP);
    assert(badge_media_store_check_space() == BADGE_MEDIA_ERR_SETUP);
    fail_after = 0;
    assert(badge_media_delete(2) == BADGE_MEDIA_ERR_IO);
    assert(badge_media_get(2));
    assert(!badge_media_delete(2));
    badge_media_store_init();
    assert(!badge_media_get(2) && badge_media_get(0) && badge_media_get(3));
    send_text();
    assert(badge_media_import_finish() == 2);
    assert(*factory == 0);

    assert(badge_media_import_begin("large.avi", BADGE_MEDIA_MAX_CLIP + 1) == BADGE_MEDIA_ERR_SIZE);
    assert(!badge_media_delete(3));
    size_t clip_size = stream_file(argv[2], "loop.avi");
    assert(badge_media_import_finish() == 3);
    badge_media_store_init();
    assert(badge_media_get(3)->kind == BADGE_MEDIA_CLIP && badge_media_get(3)->size == clip_size);
    assert(!badge_media_store_prepare());
    send_text();
    assert(badge_media_import_finish() == 1);
    assert(badge_media_store_check_space() == BADGE_MEDIA_ERR_FULL);
    assert(badge_media_import_begin("full.txt", 0) == BADGE_MEDIA_ERR_FULL);
    test_svg_import();
    return 0;
}
