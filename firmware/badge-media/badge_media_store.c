#include "badge_media_store.h"
#include "badge_svg.h"

#include <stddef.h>
#include <string.h>

#ifdef CONFIG_CPU_BR35
#include "app_config.h"
#include "asm/wdt.h"
#include "device/ioctl_cmds.h"
#include "device/tzflash_api.h"
#include "syscfg_id.h"
#include "user_cfg_id.h"

#if CONFIG_FLASH_SIZE != 0x400000 || TCFG_VIRFAT_FLASH_ENABLE || \
    TCFG_DATA_FLASH_DEV_PATY_BASE + TCFG_DATA_FLASH_DEV_PATY_SIZE > BADGE_MEDIA_FLASH_START
#error "The E87 media region requires the 4 MiB badge layout without VIRFAT"
#endif
#else
/* Native flash and preferences doubles. */
#define IOCTL_ERASE_SECTOR 200
#define CFG_BADGE_MEDIA_PREFS 48
int tzflash_read(void *dst, uint32_t size, uint32_t addr);
int tzflash_write(const void *src, uint32_t size, uint32_t addr);
int tzflash_ioctl(uint32_t cmd, uint32_t addr);
void wdt_clear(void);
int syscfg_read(uint16_t id, void *dst, uint16_t size);
int syscfg_write(uint16_t id, const void *src, uint16_t size);
#endif

#define SECTOR_SIZE  4096u
#define SMALL_SLOT   (11 * SECTOR_SIZE)
#define LARGE_SLOT   (22 * SECTOR_SIZE)
#define SLOT_OWNER   UINT32_C(0x31444d42) /* BMD1 */
#define SLOT_READY   UINT32_C(0x54494d43) /* CMIT */
#define PREFS_MAGIC  UINT32_C(0x31504642) /* BFP1 */

struct slot_header {
    uint32_t owner;
    uint32_t ready;
    uint32_t size;
    uint32_t crc;
    uint32_t kind;
    char name[BADGE_MEDIA_NAME_SIZE];
};

struct preferences {
    uint32_t magic;
    int8_t selected;
    uint8_t brightness;
    uint8_t reserved[2];
};

_Static_assert(BADGE_MEDIA_FLASH_START + 3 * SMALL_SLOT + LARGE_SLOT == BADGE_MEDIA_FLASH_END,
               "E87 media geometry");
_Static_assert(BADGE_MEDIA_MAX_CLIP <= LARGE_SLOT - sizeof(struct slot_header), "clip size");
_Static_assert(BADGE_MEDIA_PIXELS * sizeof(uint16_t) <= SMALL_SLOT - sizeof(struct slot_header),
               "photo size");

static struct badge_media_item items[BADGE_MEDIA_SLOTS];
static int pending_slot = -1;
static struct slot_header pending;

static uint32_t slot_addr(unsigned int slot)
{
    return BADGE_MEDIA_FLASH_START + slot * SMALL_SLOT;
}

static uint32_t slot_size(unsigned int slot)
{
    return slot == BADGE_MEDIA_SLOTS - 1 ? LARGE_SLOT : SMALL_SLOT;
}

static uint32_t slot_capacity(unsigned int slot)
{
    return slot_size(slot) - sizeof(struct slot_header);
}

static int flash_read(void *data, uint32_t size, uint32_t addr)
{
    return tzflash_read(data, size, addr) == (int)size;
}

static int flash_write(const void *data, uint32_t size, uint32_t addr)
{
    return tzflash_write(data, size, addr) == (int)size;
}

static int program_word(uint32_t value, uint32_t addr)
{
    uint32_t actual = ~value;
    return flash_write(&value, sizeof(value), addr) &&
           flash_read(&actual, sizeof(actual), addr) && actual == value;
}

static uint32_t update_crc(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return crc;
}

static int valid_size(enum badge_media_kind kind, uint32_t size)
{
    switch (kind) {
    case BADGE_MEDIA_PHOTO:
        return size == BADGE_MEDIA_PIXELS * sizeof(uint16_t);
    case BADGE_MEDIA_TEXT:
        return size && size <= BADGE_TEXT_MAX;
    case BADGE_MEDIA_SVG:
        return size && size <= BADGE_SVG_MAX;
    case BADGE_MEDIA_CLIP:
        return size && size <= BADGE_MEDIA_MAX_CLIP;
    default:
        return 0;
    }
}

static int payload_intact(unsigned int slot, const struct slot_header *header)
{
    uint8_t data[128];
    uint32_t crc = UINT32_MAX;
    uint32_t start = slot_addr(slot) + sizeof(*header);

    for (uint32_t offset = 0; offset < header->size;) {
        uint32_t count = header->size - offset;
        if (count > sizeof(data)) {
            count = sizeof(data);
        }
        if (!flash_read(data, count, start + offset)) {
            return 0;
        }
        crc = update_crc(crc, data, count);
        offset += count;
    }
    return crc == header->crc;
}

static void publish_item(unsigned int slot, const struct slot_header *header)
{
    items[slot].kind = header->kind;
    items[slot].size = header->size;
    memcpy(items[slot].name, header->name, sizeof(items[slot].name));
}

void badge_media_store_init(void)
{
    pending_slot = -1;
    memset(items, 0, sizeof(items));
    for (unsigned int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        struct slot_header header;
        if (flash_read(&header, sizeof(header), slot_addr(slot)) &&
            header.owner == SLOT_OWNER && header.ready == SLOT_READY &&
            header.size <= slot_capacity(slot) &&
            valid_size(header.kind, header.size) &&
            memchr(header.name, '\0', sizeof(header.name)) &&
            payload_intact(slot, &header)) {
            publish_item(slot, &header);
        }
    }
}

const struct badge_media_item *badge_media_get(unsigned int slot)
{
    return slot < BADGE_MEDIA_SLOTS && items[slot].kind != BADGE_MEDIA_EMPTY ? &items[slot] : NULL;
}

static size_t read_payload(unsigned int slot, uint32_t size, uint32_t offset, void *dst, size_t len)
{
    if (offset >= size || !len) {
        return 0;
    }
    if (len > size - offset) {
        len = size - offset;
    }
    return flash_read(dst, len, slot_addr(slot) + sizeof(struct slot_header) + offset) ? len : 0;
}

size_t badge_media_read_slot(void *slot_context, uint32_t offset, void *dst, size_t len)
{
    unsigned int slot = (uintptr_t)slot_context;
    const struct badge_media_item *item = badge_media_get(slot);
    return item ? read_payload(slot, item->size, offset, dst, len) : 0;
}

int badge_media_delete(unsigned int slot)
{
    if (pending_slot >= 0 || !badge_media_get(slot)) {
        return BADGE_MEDIA_ERR_IO;
    }
    if (!program_word(0, slot_addr(slot) + offsetof(struct slot_header, ready))) {
        return BADGE_MEDIA_ERR_IO;
    }
    items[slot].kind = BADGE_MEDIA_EMPTY;
    return 0;
}

static int check_erased(uint32_t start, uint32_t size)
{
    uint8_t data[128];
    for (uint32_t addr = start; addr < start + size; addr += sizeof(data)) {
        if (!flash_read(data, sizeof(data), addr)) {
            return BADGE_MEDIA_ERR_IO;
        }
        for (size_t i = 0; i < sizeof(data); ++i) {
            if (data[i] != UINT8_MAX) {
                return BADGE_MEDIA_ERR_SETUP;
            }
        }
    }
    return 0;
}

void badge_media_store_stats(unsigned int blank[BADGE_MEDIA_SLOTS], unsigned int *read_errors)
{
    *read_errors = 0;
    for (unsigned int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        blank[slot] = 0;
        for (uint32_t offset = 0; offset < slot_size(slot); offset += SECTOR_SIZE) {
            int ret = check_erased(slot_addr(slot) + offset, SECTOR_SIZE);
            if (!ret) ++blank[slot];
            if (ret == BADGE_MEDIA_ERR_IO) ++*read_errors;
        }
    }
}

static int erase_sector(uint32_t addr)
{
    wdt_clear();
    return tzflash_ioctl(IOCTL_ERASE_SECTOR, addr);
}

/* Return an eligible slot, or the reason no slot can be used. */
static int find_slot(enum badge_media_kind kind, uint32_t size_hint, uint32_t *owner)
{
    if (pending_slot >= 0) {
        return BADGE_MEDIA_ERR_IO;
    }
    if (size_hint > slot_capacity(BADGE_MEDIA_SLOTS - 1)) {
        return BADGE_MEDIA_ERR_SIZE;
    }

    int unavailable = BADGE_MEDIA_ERR_FULL;
    for (unsigned int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        /* Keep the large slot available for a clip when one of the small slots fits. */
        if (badge_media_get(slot) || size_hint > slot_capacity(slot) ||
            (kind == BADGE_MEDIA_CLIP && slot != BADGE_MEDIA_SLOTS - 1)) {
            continue;
        }
        uint32_t start = slot_addr(slot);
        struct slot_header header;
        if (!flash_read(&header, offsetof(struct slot_header, size), start)) {
            return BADGE_MEDIA_ERR_IO;
        }
        *owner = header.owner;
        if (*owner == SLOT_OWNER) {
            /* A transient boot-time read error can hide a committed item from the UI. */
            if (header.ready == SLOT_READY) continue;
            return slot;
        }
        int ret = check_erased(start, slot_size(slot));
        if (ret == BADGE_MEDIA_ERR_SETUP) {
            unavailable = ret;
            continue;
        }
        if (ret) return ret;
        return slot;
    }
    return unavailable;
}

int badge_media_store_check_space(void)
{
    uint32_t owner;
    int slot = find_slot(BADGE_MEDIA_EMPTY, 0, &owner);
    return slot < 0 ? slot : 0;
}

int badge_media_store_needs_preparation(void)
{
    for (unsigned int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        if (badge_media_get(slot)) continue;
        uint32_t start = slot_addr(slot), owner;
        if (flash_read(&owner, sizeof(owner), start) && owner != SLOT_OWNER &&
            check_erased(start, slot_size(slot)) == BADGE_MEDIA_ERR_SETUP) return 1;
    }
    return 0;
}

int badge_media_store_prepare(void)
{
    if (pending_slot >= 0) return BADGE_MEDIA_ERR_IO;
    for (unsigned int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        if (badge_media_get(slot)) continue;
        uint32_t start = slot_addr(slot), owner;
        if (!flash_read(&owner, sizeof(owner), start)) return BADGE_MEDIA_ERR_IO;
        if (owner == SLOT_OWNER) continue;
        for (uint32_t offset = 0; offset < slot_size(slot); offset += SECTOR_SIZE) {
            uint32_t addr = start + offset;
            int ret = check_erased(addr, SECTOR_SIZE);
            if (!ret) continue;
            if (ret != BADGE_MEDIA_ERR_SETUP ||
                erase_sector(addr) || check_erased(addr, SECTOR_SIZE)) {
                return BADGE_MEDIA_ERR_IO;
            }
        }
    }
    return 0;
}

int badge_media_store_begin(enum badge_media_kind kind, const char *name, uint32_t size_hint)
{
    uint32_t owner;
    int slot = find_slot(kind, size_hint, &owner);
    if (slot < 0) return slot;
    uint32_t start = slot_addr(slot);
    if (owner == SLOT_OWNER) {
        /* Keep the ownership marker until all payload sectors have been erased. */
        for (uint32_t offset = SECTOR_SIZE; offset < slot_size(slot); offset += SECTOR_SIZE) {
            if (erase_sector(start + offset)) return BADGE_MEDIA_ERR_IO;
        }
        if (erase_sector(start)) return BADGE_MEDIA_ERR_IO;
    }
    if (!program_word(SLOT_OWNER, start)) return BADGE_MEDIA_ERR_IO;
    memset(&pending, 0, sizeof(pending));
    pending.kind = kind;
    pending.crc = UINT32_MAX;
    strncpy(pending.name, name, sizeof(pending.name) - 1);
    pending_slot = slot;
    return 0;
}

int badge_media_store_append(const void *data, size_t len)
{
    if (pending_slot < 0) {
        return BADGE_MEDIA_ERR_IO;
    }
    if (len > slot_capacity(pending_slot) - pending.size) {
        return BADGE_MEDIA_ERR_SIZE;
    }
    uint32_t addr = slot_addr(pending_slot) + sizeof(pending) + pending.size;
    if (len && !flash_write(data, len, addr)) {
        return BADGE_MEDIA_ERR_IO;
    }
    pending.crc = update_crc(pending.crc, data, len);
    pending.size += len;
    return 0;
}

size_t badge_media_store_read_pending(void *ctx, uint32_t offset, void *dst, size_t len)
{
    (void)ctx;
    return pending_slot >= 0 ? read_payload(pending_slot, pending.size, offset, dst, len) : 0;
}

int badge_media_store_commit(void)
{
    int slot = pending_slot;
    if (slot < 0) {
        return BADGE_MEDIA_ERR_IO;
    }
    if (!valid_size(pending.kind, pending.size)) {
        badge_media_store_abort();
        return BADGE_MEDIA_ERR_FORMAT;
    }

    uint32_t addr = slot_addr(slot);
    const size_t offset = offsetof(struct slot_header, size);
    struct slot_header written = { 0 };
    if (!payload_intact(slot, &pending) ||
        !flash_write((const uint8_t *)&pending + offset, sizeof(pending) - offset, addr + offset) ||
        !flash_read(&written, sizeof(written), addr) ||
        memcmp((const uint8_t *)&pending + offset, (const uint8_t *)&written + offset,
               sizeof(pending) - offset) ||
        written.owner != SLOT_OWNER || written.ready != UINT32_MAX ||
        !program_word(SLOT_READY, addr + offsetof(struct slot_header, ready))) {
        badge_media_store_abort();
        return BADGE_MEDIA_ERR_IO;
    }
    publish_item(slot, &pending);
    pending_slot = -1;
    return slot;
}

void badge_media_store_abort(void)
{
    pending_slot = -1;
}

void badge_media_preferences_read(int *selected, unsigned int *brightness_pct)
{
    struct preferences prefs;
    *selected = -1;
    *brightness_pct = 50;
    if (syscfg_read(CFG_BADGE_MEDIA_PREFS, &prefs, sizeof(prefs)) == sizeof(prefs) &&
        prefs.magic == PREFS_MAGIC && prefs.brightness >= 10 && prefs.brightness <= 100) {
        *brightness_pct = prefs.brightness;
        if (prefs.selected >= 0 && badge_media_get(prefs.selected)) {
            *selected = prefs.selected;
        }
    }
}

void badge_media_preferences_write(int selected, unsigned int brightness_pct)
{
    if (selected < 0 || !badge_media_get(selected)) {
        selected = -1;
    }
    int saved_selected;
    unsigned int saved_brightness;
    badge_media_preferences_read(&saved_selected, &saved_brightness);
    if (saved_selected == selected && saved_brightness == brightness_pct) {
        return;
    }
    const struct preferences prefs = { PREFS_MAGIC, selected, brightness_pct, { 0, 0 } };
    syscfg_write(CFG_BADGE_MEDIA_PREFS, &prefs, sizeof(prefs));
}
