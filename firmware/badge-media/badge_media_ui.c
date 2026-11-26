#if defined(CONFIG_BADGE_BOARD_E87) && defined(CONFIG_BADGE_MENU)
#include "badge_media_ui.h"
#include "badge_media_codec.h"
#include "badge_media_store.h"
#include "badge_media_svg_font.h"
#include "badge_opp.h"
#include "badge_svg.h"
#include "system/includes.h"

#define SVG_FRAME_MS 84u
#define SVG_GLYPH_MARGIN 4

static lv_obj_t *home, *panel, *image, *text_box, *text_label, *message;
static lv_obj_t *hint;
static uint32_t hint_started_ms;
static lv_group_t *keys;
static lv_event_cb_t settings_cb, tools_cb;
static unsigned char (*receive_blocked)(void);
static int selected, viewing;
static unsigned int brightness;
static bool receiving, playing;
static int receive_state;
static uint32_t shown_kb, pairing_number;
static uint32_t next_frame_ms, frame_fraction_us;
static struct badge_avi clip;
static char body[BADGE_TEXT_MAX + 1];
static struct {
    struct badge_svg_scene scene;
    lv_obj_t *layer;
    struct {
        lv_obj_t *label;
        int width, baseline, x;
        uint8_t opacity;
    } texts[BADGE_SVG_TEXTS];
    uint32_t started_ms, frame_ms;
    bool animating;
} svg;
static lv_img_dsc_t picture = {
    .header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = BADGE_MEDIA_SIDE, .h = BADGE_MEDIA_SIDE},
    .data_size = BADGE_MEDIA_PIXELS * sizeof(uint16_t),
    .data = (const uint8_t *)badge_media_pixels,
};

static void show_text(const char *text)
{
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_static(text_label, text);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(text_box);
    lv_obj_align(text_label, lv_obj_get_height(text_label) > lv_obj_get_height(text_box) ?
                 LV_ALIGN_TOP_MID : LV_ALIGN_CENTER, 0, 0);
    lv_obj_scroll_to_y(text_box, 0, LV_ANIM_OFF);
}

static void show_image(const lv_img_dsc_t *source)
{
    lv_obj_add_flag(text_box, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(image, source);
    lv_img_set_zoom(image, 256u * 360u / source->header.w);
    lv_obj_center(image);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(image);
}

static void close_svg(void)
{
    if (svg.layer) lv_obj_del(svg.layer);
    svg.layer = NULL;
    svg.animating = false;
}

static void unload_home(lv_event_t *event)
{
    (void)event;
    close_svg();
}

static bool show_svg(void *slot, uint32_t size)
{
    char *data = (char *)badge_media_pixels;
    if (size > BADGE_SVG_MAX || badge_media_read_slot(slot, 0, data, size) != size ||
        badge_svg_parse(data, size, &svg.scene)) return false;

    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(text_box, LV_OBJ_FLAG_HIDDEN);
    svg.layer = lv_obj_create(home);
    if (!svg.layer) return false;
    lv_obj_remove_style_all(svg.layer);
    lv_obj_clear_flag(svg.layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(svg.layer, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(svg.layer, 360, 360);
    if (hint) lv_obj_move_foreground(hint);

    for (unsigned int i = 0; i < svg.scene.count; ++i) {
        const struct badge_svg_text *text = &svg.scene.texts[i];
        const lv_font_t *font = badge_svg_font(text->serif, text->size);
        lv_obj_t *label = lv_label_create(svg.layer);
        if (!label) {
            close_svg();
            return false;
        }
        svg.texts[i].label = label;
        svg.texts[i].width = lv_txt_get_width(text->text, strlen(text->text), font, 0, LV_TEXT_FLAG_NONE);
        svg.texts[i].baseline = font->line_height - font->base_line;
        lv_obj_remove_style_all(label);
        lv_obj_set_size(label, svg.texts[i].width + 2 * SVG_GLYPH_MARGIN, font->line_height);
        lv_obj_set_style_pad_left(label, SVG_GLYPH_MARGIN, 0);
        lv_obj_set_style_pad_right(label, SVG_GLYPH_MARGIN, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(text->rgb), 0);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_letter_space(label, 0, 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text_static(label, text->text);

        int x, y;
        svg.animating |= badge_svg_sample(text, 0, &x, &y, &svg.texts[i].opacity);
        badge_svg_place(text, svg.texts[i].width, svg.texts[i].baseline, &x, &y);
        svg.texts[i].x = x;
        lv_obj_set_pos(label, x - SVG_GLYPH_MARGIN, y);
        lv_obj_set_style_text_opa(label, svg.texts[i].opacity, 0);
    }
    svg.frame_ms = svg.started_ms = timer_get_ms();
    return true;
}

static void update_svg(void)
{
    uint32_t now = timer_get_ms();
    if (now - svg.frame_ms < SVG_FRAME_MS) return;
    svg.frame_ms = now;
    svg.animating = false;
    for (unsigned int i = 0; i < svg.scene.count; ++i) {
        const struct badge_svg_text *text = &svg.scene.texts[i];
        int x, y;
        uint8_t opacity;
        svg.animating |= badge_svg_sample(text, now - svg.started_ms, &x, &y, &opacity);
        badge_svg_place(text, svg.texts[i].width, svg.texts[i].baseline, &x, &y);
        if (svg.texts[i].x != x) {
            svg.texts[i].x = x;
            lv_obj_set_x(svg.texts[i].label, x - SVG_GLYPH_MARGIN);
        }
        if (svg.texts[i].opacity != opacity) {
            svg.texts[i].opacity = opacity;
            lv_obj_set_style_text_opa(svg.texts[i].label, opacity, 0);
        }
    }
}

static void advance_frame_clock(void)
{
    frame_fraction_us += clip.frame_us % 1000u;
    next_frame_ms += clip.frame_us / 1000u + frame_fraction_us / 1000u;
    frame_fraction_us %= 1000u;
}

static int next_slot(int from, int direction)
{
    for (int i = 0; i < BADGE_MEDIA_SLOTS; ++i) {
        from = (from + direction + BADGE_MEDIA_SLOTS) % BADGE_MEDIA_SLOTS;
        if (badge_media_get(from)) return from;
    }
    return -1;
}

void badge_media_ui_home(void)
{
    close_svg();
    lv_group_remove_all_objs(keys);
    lv_scr_load(home);
    const struct badge_media_item *item = selected >= 0 ? badge_media_get(selected) : NULL;
    playing = false;
    if (!item) {
        show_text("Press Button 1\nthen choose Receive");
        return;
    }
    void *ctx = (void *)(uintptr_t)selected;
    switch (item->kind) {
    case BADGE_MEDIA_TEXT:
        if (item->size <= BADGE_TEXT_MAX &&
            badge_media_read_slot(ctx, 0, body, item->size) == item->size) {
            body[item->size] = 0;
            show_text(body);
            return;
        }
        break;
    case BADGE_MEDIA_SVG:
        if (show_svg(ctx, item->size)) return;
        break;
    case BADGE_MEDIA_PHOTO:
        if (item->size == picture.data_size &&
            badge_media_read_slot(ctx, 0, badge_media_pixels, item->size) == item->size) {
            show_image(&picture);
            return;
        }
        break;
    case BADGE_MEDIA_CLIP:
        if (badge_avi_open(&clip, badge_media_read_slot, ctx, item->size) == 0 &&
            badge_avi_next(&clip, badge_media_pixels) == 0) {
            playing = true;
            next_frame_ms = timer_get_ms();
            frame_fraction_us = 0;
            advance_frame_clock();
            show_image(&picture);
            return;
        }
        break;
    default:
        break;
    }
    show_text("Cannot read this item");
}

static void select_slot(int slot)
{
    if (slot != selected) {
        selected = slot;
        badge_media_preferences_write(selected, brightness);
    }
    badge_media_ui_home();
}

static void gesture(lv_event_t *event)
{
    (void)event;
    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
        int slot = next_slot(selected, direction == LV_DIR_LEFT ? 1 : -1);
        if (slot >= 0 && slot != selected) select_slot(slot);
    }
}

static void dismiss_hint(void)
{
    if (hint) {
        lv_obj_del(hint);
        hint = NULL;
    }
}

static void show_hint(const char *text)
{
    dismiss_hint();
    hint = lv_label_create(home);
    lv_label_set_text_static(hint, text);
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_set_style_bg_color(hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(hint, 6, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -54);
    hint_started_ms = timer_get_ms();
}

static void page(const char *title)
{
    dismiss_hint();
    playing = false;
    lv_group_remove_all_objs(keys);
    lv_obj_clean(panel);
    lv_obj_t *heading = lv_label_create(panel);
    lv_label_set_text_static(heading, title);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 25);
    lv_scr_load(panel);
}

static lv_obj_t *button(const char *caption, int y, lv_event_cb_t action, intptr_t value)
{
    lv_obj_t *b = lv_btn_create(panel);
    lv_obj_set_size(b, 202, 43);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *label = lv_label_create(b);
    lv_obj_set_width(label, 180);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &badge_text_font, 0);
    lv_label_set_text(label, caption);
    lv_obj_center(label);
    lv_obj_add_event_cb(b, action, LV_EVENT_SHORT_CLICKED, (void *)value);
    lv_group_add_obj(keys, b);
    return b;
}

static lv_obj_t *note(const char *content)
{
    lv_obj_t *label = lv_label_create(panel);
    lv_obj_set_width(label, 250);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &badge_text_font, 0);
    lv_label_set_text(label, content);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);
    return label;
}

static void go_home(lv_event_t *e) { (void)e; badge_media_ui_home(); }
static void go_menu(lv_event_t *e) { (void)e; badge_media_ui_menu(); }
static void gallery(lv_event_t *e);
static void details(lv_event_t *e);
static void receive(lv_event_t *e);
static void offer_gallery_setup(lv_event_t *e);

static void display(lv_event_t *e)
{
    (void)e;
    select_slot(viewing);
}

static void delete_item(lv_event_t *e)
{
    (void)e;
    if (badge_media_delete(viewing) < 0) {
        page("Gallery");
        note("Could not delete item");
        button("Back", 238, gallery, 0);
        return;
    }
    if (viewing == selected) {
        selected = next_slot(viewing, 1);
        badge_media_preferences_write(selected, brightness);
    }
    gallery(NULL);
}

static void confirm_delete(lv_event_t *e)
{
    (void)e;
    const struct badge_media_item *item = badge_media_get(viewing);
    if (!item) { gallery(NULL); return; }
    page("Delete this item?");
    note(item->name);
    button("Keep", 205, details, viewing);
    button("Delete", 260, delete_item, 0);
}

static void details(lv_event_t *e)
{
    viewing = (int)(intptr_t)lv_event_get_user_data(e);
    const struct badge_media_item *item = badge_media_get(viewing);
    if (!item) { gallery(NULL); return; }
    page("Gallery");
    lv_obj_t *name = lv_label_create(panel);
    lv_obj_set_width(name, 246);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &badge_text_font, 0);
    lv_label_set_text(name, item->name);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 74);
    button("Display", 127, display, 0);
    button("Delete...", 181, confirm_delete, 0);
    button("Back", 235, gallery, 0);
}

static void gallery(lv_event_t *e)
{
    (void)e;
    page("Gallery");
    int rows = 0;
    for (int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        const struct badge_media_item *item = badge_media_get(slot);
        if (item) button(item->name, 57 + (rows++) * 49, details, slot);
    }
    if (!rows) note("Empty gallery");
    if (badge_media_store_needs_preparation())
        button("Prepare storage", 206, offer_gallery_setup, 0);
    button("Back", 260, go_menu, 0);
}

static void leave_receive(lv_event_t *e)
{
    (void)e;
    badge_opp_stop();
    receiving = false;
    badge_media_ui_home();
}

static void prepare_gallery(lv_event_t *e)
{
    (void)e;
    if (receive_blocked()) return;
    page("Preparing gallery");
    note("Preparing the reserved storage");
    lv_refr_now(NULL);
    int result = badge_media_store_prepare();
    printf("badge gallery: prepare result=%d\n", result);
    if (!result) {
        receive(NULL);
    } else {
        page("Storage error");
        note("Could not prepare the gallery");
        button("Retry", 206, prepare_gallery, 0);
        button("Back", 260, go_menu, 0);
    }
}

static void offer_gallery_setup(lv_event_t *e)
{
    (void)e;
    page("Prepare gallery?");
    note("Erase old data in the\nreserved gallery area?");
    button("Erase and prepare", 206, prepare_gallery, 0);
    button("Back", 260, go_menu, 0);
}

static void receive(lv_event_t *e)
{
    (void)e;
    if (receive_blocked()) return;
    int space = badge_media_store_check_space();
    if (space == BADGE_MEDIA_ERR_SETUP) {
        offer_gallery_setup(NULL);
        return;
    }
    page("Receive");
    if (space) {
        if (space == BADGE_MEDIA_ERR_FULL) {
            note("Gallery full\nDelete an item to receive another.");
            button("Gallery", 206, gallery, 0);
        } else {
            note("Storage error");
        }
        button("Back", 260, go_menu, 0);
        return;
    }
    receive_state = BADGE_OPP_OFF;
    shown_kb = UINT32_MAX;
    receiving = true;
    badge_opp_start();
}

static void confirm_pair(lv_event_t *e)
{
    lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
    badge_opp_confirm(pairing_number);
}

static const char *receive_error(int error)
{
    switch (error) {
    case BADGE_MEDIA_ERR_FORMAT: return "Unsupported or damaged file";
    case BADGE_MEDIA_ERR_FULL: return "Gallery full";
    case BADGE_MEDIA_ERR_SIZE: return "File too large";
    case BADGE_MEDIA_ERR_IO: return "Storage error";
    case BADGE_OPP_ERR_TIMEOUT: return "Transfer timed out";
    case BADGE_OPP_ERR_DISCONNECTED: return "Sender disconnected";
    case BADGE_OPP_ERR_RADIO: return "Bluetooth unavailable";
    case BADGE_OPP_ERR_PAIRING: return "Pairing failed";
    default: return "Transfer failed";
    }
}

static void refresh_receive(void)
{
    struct badge_opp_status status = badge_opp_poll();
    bool changed = receive_state != (int)status.state;
    receive_state = status.state;
    switch (status.state) {
    case BADGE_OPP_WAITING:
        if (changed) {
            page("Receive");
            message = note("");
            lv_label_set_text_fmt(message, "Send a file by Bluetooth to\n%s", badge_opp_name());
            button("Cancel", 249, leave_receive, 0);
        }
        break;
    case BADGE_OPP_PAIRING:
        if (changed || status.number != pairing_number) {
            pairing_number = status.number;
            page("Pair with sender");
            message = note("");
            lv_label_set_text_fmt(message, "Compare both codes\n%06u", (unsigned int)pairing_number);
            button("Codes match", 215, confirm_pair, 0);
            button("Cancel", 264, leave_receive, 0);
        }
        break;
    case BADGE_OPP_SECURING:
        if (changed) {
            page("Pair with sender");
            note("Waiting for the other device");
            button("Cancel", 249, leave_receive, 0);
        }
        break;
    case BADGE_OPP_RECEIVING: {
        uint32_t kb = (status.received + 1023u) / 1024u;
        if (changed) {
            page("Receive");
            message = note("");
            button("Cancel", 249, leave_receive, 0);
        }
        if (changed || kb != shown_kb)
            lv_label_set_text_fmt(message, "Receiving\n%u KiB", (unsigned int)kb);
        shown_kb = kb;
        break;
    }
    case BADGE_OPP_DONE:
    case BADGE_OPP_FAILED:
        badge_opp_stop();
        receiving = false;
        if (status.state == BADGE_OPP_DONE) {
            printf("badge receive: stored %u bytes\n", (unsigned int)status.received);
            select_slot(status.slot);
        } else {
            struct badge_opp_diagnostic diagnostic = badge_opp_diagnostic();
            printf("badge receive: error=%d obex=%02x bytes=%u stage=%s\n", diagnostic.result,
                   (unsigned int)diagnostic.response, (unsigned int)diagnostic.bytes, diagnostic.stage);
            if (status.error == BADGE_MEDIA_ERR_SETUP) {
                offer_gallery_setup(NULL);
            } else {
                badge_media_ui_home();
                show_hint(receive_error(status.error));
            }
        }
        break;
    default:
        break;
    }
}

unsigned int badge_media_diagnostic_read(char *buffer, unsigned int size)
{
    if (!size) return 0;
    unsigned int blank[BADGE_MEDIA_SLOTS], errors, items = 0;
    badge_media_store_stats(blank, &errors);
    for (unsigned int slot = 0; slot < BADGE_MEDIA_SLOTS; ++slot) {
        if (badge_media_get(slot)) ++items;
    }
    struct badge_opp_diagnostic diagnostic = badge_opp_diagnostic();
    struct badge_media_jpeg_diagnostics jpeg = badge_media_import_jpeg_diagnostics();
    int length = snprintf(buffer, size,
                          "v=1\nitems=%u\nblank=%u,%u,%u,%u\nread_errors=%u\nlast_receive=%d\n"
                          "obex=%02x\nbytes=%u\nstage=%s\njpeg_stage=%s\njpeg_detail=%d\njpeg_soi=%d\n",
                          items, blank[0], blank[1], blank[2], blank[3], errors, diagnostic.result,
                          (unsigned int)diagnostic.response, (unsigned int)diagnostic.bytes, diagnostic.stage,
                          jpeg.stage, jpeg.detail, jpeg.soi);
    if (length <= 0) return 0;
    return (unsigned int)length < size ? (unsigned int)length : size - 1;
}

void badge_media_ui_menu(void)
{
    page("Badge");
    button("Receive", 57, receive, 0);
    button("Gallery", 106, gallery, 0);
    button("Settings", 155, settings_cb, 0);
    button("Tools", 204, tools_cb, 0);
    button("Back", 253, go_home, 0);
}

void badge_media_ui_save_brightness(unsigned int percent)
{
    brightness = percent;
    badge_media_preferences_write(selected, brightness);
}

unsigned int badge_media_ui_init(lv_obj_t *screen, lv_group_t *keypad,
                                lv_event_cb_t settings, lv_event_cb_t tools,
                                unsigned char (*blocked)(void))
{
    home = screen;
    keys = keypad;
    settings_cb = settings;
    tools_cb = tools;
    receive_blocked = blocked;
    badge_media_store_init();
    badge_media_preferences_read(&selected, &brightness);
    if (selected < 0 || selected >= BADGE_MEDIA_SLOTS || !badge_media_get(selected))
        selected = next_slot(-1, 1);
    panel = lv_obj_create(NULL);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_text_color(panel, lv_color_white(), 0);
    lv_obj_add_event_cb(home, gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(home, unload_home, LV_EVENT_SCREEN_UNLOADED, NULL);
    image = lv_img_create(home);
    lv_obj_add_flag(image, LV_OBJ_FLAG_GESTURE_BUBBLE);
    text_box = lv_obj_create(home);
    lv_obj_remove_style_all(text_box);
    lv_obj_set_size(text_box, 248, 236);
    lv_obj_center(text_box);
    lv_obj_set_scroll_dir(text_box, LV_DIR_VER);
    lv_obj_add_flag(text_box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    text_label = lv_label_create(text_box);
    lv_obj_set_width(text_label, 244);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(text_label, &badge_text_font, 0);
    lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
    return brightness;
}

unsigned int badge_media_ui_tick(void)
{
    if (hint && timer_get_ms() - hint_started_ms >= 5000) dismiss_hint();
    if (receiving) refresh_receive();
    if (lv_scr_act() != home) return 1;
    if (svg.animating) {
        update_svg();
        if (svg.animating) return 1;
    }
    if (!playing) {
        /* Give input and scrolling their normal cadence before parking on a still image. */
        if (lv_disp_get_inactive_time(NULL) < 2000 || lv_anim_count_running()) return 1;
        return 20; /* 200 ms maximum. The touch and key drivers wake the UI on a press. */
    }
    uint32_t now = timer_get_ms();
    if ((int32_t)(now - next_frame_ms) >= 0) {
        int result = badge_avi_next(&clip, badge_media_pixels);
        if (result == 1) {
            badge_avi_rewind(&clip);
            result = badge_avi_next(&clip, badge_media_pixels);
        }
        if (result != 0) {
            playing = false;
            show_text("Cannot play this clip");
        } else {
            lv_obj_invalidate(image);
            advance_frame_clock();
        }
    }
    return 1;
}
#endif
