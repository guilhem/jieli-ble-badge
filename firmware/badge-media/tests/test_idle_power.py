"""Exercise E87 key wakeups and the touch cache using the patched SDK on the host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SDK = Path(os.environ.get('BADGE_SDK', ROOT / 'tools/community-re/jieli-sdks/e_badge_707_sdk_200'))


def function(source, name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]


def compile_and_run(source):
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'test'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', exe],
                       input=source, text=True, check=True)
        subprocess.run([exe], check=True)


class IdleInput(unittest.TestCase):
    def test_receive_replaces_the_active_controls_before_start(self):
        media = (ROOT / 'firmware/badge-media/badge_media_ui.c').read_text()
        functions = [function(media, name) for name in (
            'static void leave_receive(', 'static void show_receive_status(', 'static void receive(lv_event_t *e)\n{')]
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int lv_event_t;
enum { BADGE_OPP_STARTING = 1 };
static bool receiving, receive_stalled, blocked, replacement_visible, cancel_visible;
static int receive_state, starts;
static uint32_t shown_kb;
static bool receive_blocked(void) { return blocked; }
static void badge_opp_stop(void) {}
static void page(const char *title) { replacement_visible = !strcmp(title, "Receive"); }
static void note(const char *text) { assert(!strcmp(text, "Starting Bluetooth")); }
static void button(const char *text, int y, void (*action)(lv_event_t *), int arg) {
    (void)y; (void)action; (void)arg;
    cancel_visible = !strcmp(text, "Cancel");
}
static void badge_opp_start(void) {
    assert(receiving && replacement_visible && cancel_visible);
    ++starts;
}
''' + '\n'.join(functions) + r'''
int main(void) {
    blocked = true;
    receive(0);
    assert(!starts && !replacement_visible);
    blocked = false;
    receive(0);
    assert(starts == 1 && receive_state == BADGE_OPP_STARTING && !receive_stalled);
}
'''
        compile_and_run(source)

    def test_e87_keeps_one_key_until_debounced_release(self):
        adkey = (SDK / 'SDK/apps/common/device/key/adkey.c').read_text()
        source = r'''
#include <assert.h>
#include <stdint.h>
#define CONFIG_BADGE_MENU 1
#define CONFIG_BADGE_BOARD_E87 1
#define NO_KEY 255
#define CONFIG_ADKEY_MAX_NUM 3
#define P33_CON_GET(reg) (physical_low ? 0 : (1u << 1))
typedef uint8_t u8;
typedef uint16_t u16;
static const struct {
    u8 enable, ad_channel;
    u16 ad_value[3];
    u8 key_value[3];
} config = {1, 0, {342, 571, 1500}, {2, 0, 1}}, *__this = &config;
static u16 adc;
static u8 physical_low, published = NO_KEY, pressed;
static u8 update_blocked, update_starts_on_irq_mask, irq_masked;
static unsigned int edges, resets;
static u16 adc_get_value(u8 channel) { (void)channel; return adc; }
void badge_adkey_publish(u8 key) { published = key; }
void badge_media_key_pressed(u8 key) { pressed = key; ++edges; }
u8 badge_restart_blocked(void) { return update_blocked; }
void unmask_enter_critical(void) {
    assert(!irq_masked);
    irq_masked = 1;
    if (update_starts_on_irq_mask) update_blocked = 1;
}
void unmask_exit_critical(void) { assert(irq_masked); irq_masked = 0; }
void cpu_reset(void) { assert(irq_masked && !update_blocked); ++resets; }
''' + function(adkey[adkey.index('#else\n\nu8 ad_get_key_value'):], 'u8 ad_get_key_value(') + r'''
static u8 sample(u16 value, u8 low, unsigned int count) {
    adc = value;
    physical_low = low;
    while (count--) assert(ad_get_key_value() == NO_KEY);
    return published;
}
int main(void) {
    assert(sample(737, 1, 4) == 1 && edges == 1 && pressed == 1);
    assert(sample(405, 1, 30) == 1 && edges == 1);
    assert(sample(4090, 0, 4) == 1);
    assert(sample(737, 1, 1) == 1);
    assert(sample(4090, 0, 5) == NO_KEY);

    /* A slow contact can read in the upper window on its way down. */
    assert(sample(737, 1, 2) == NO_KEY);
    assert(sample(405, 1, 2) == 0 && edges == 2 && pressed == 0);
    assert(sample(737, 1, 30) == 0 && edges == 2);
    assert(sample(4090, 0, 5) == NO_KEY);

    /* ADC alone uses the slower noise filter. Invalid levels emit no key. */
    assert(sample(737, 0, 24) == NO_KEY);
    assert(sample(737, 0, 1) == 1 && edges == 3);
    assert(sample(4090, 0, 5) == NO_KEY);
    assert(sample(2000, 1, 8) == NO_KEY && edges == 3);
    assert(sample(279, 1, 1) == 2 && edges == 4);
}
'''
        compile_and_run(source)

        restart = source[:source.index('int main(void)')] + r'''
int main(void) {
    /* Buttons still down from a reboot must not start another one. */
    sample(279, 1, 1200);
    assert(!resets);
    sample(4090, 0, 5);

    sample(405, 1, 600);
    sample(279, 0, 600);
    assert(!resets);
    sample(279, 1, 499);
    assert(!resets);
    sample(737, 1, 1);
    sample(279, 1, 499);
    assert(!resets);
    /* The UI still sees the original Select press. */
    assert(sample(279, 1, 1) == 0 && resets == 1);
    sample(279, 1, 1200);
    sample(4090, 0, 4);
    sample(279, 1, 600);
    assert(resets == 1);

    sample(4090, 0, 5);
    update_starts_on_irq_mask = 1;
    sample(279, 1, 500);
    assert(resets == 1 && update_blocked && !irq_masked);
    update_blocked = update_starts_on_irq_mask = 0;

    sample(4090, 0, 5);
    sample(279, 1, 499);
    update_blocked = 1;
    sample(279, 1, 1);
    update_blocked = 0;
    sample(279, 1, 600);
    assert(resets == 1);

    sample(4090, 0, 5);
    sample(737, 1, 10);
    assert(sample(279, 1, 500) == 1 && resets == 2);
}
'''
        compile_and_run(restart)

    def test_press_and_release_before_ui_reads(self):
        apps = SDK / 'SDK/apps'
        ui = (apps / 'common/ui/lvgl_v8/lvgl_main.c').read_text()
        touch = (apps / 'watch/board/br35/board_ac707n_demo/board_ac707n_demo.c').read_text()
        functions = [function(ui, name) for name in (
            'void badge_media_key_pressed(', 'void badge_media_touch_pressed(', 'static u8 badge_media_take_key(')]
        functions.extend(function(touch, name) for name in (
            'static u8 badge_touch_init(', 'static void axs_poll_task(', 'void get_touch_x_y_status('))
        source = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#define CONFIG_BADGE_BOARD_E87 1
#define NO_KEY 255
#define OS_ENTER_CRITICAL()
#define OS_EXIT_CRITICAL()
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u8 g_badge_locked, media_input_ready, media_idle_wait, media_pending_key = NO_KEY;
static u8 g_axs_ready, g_axs_want = 1, g_tp_reinit_req, g_tp_cdown, media_touch_idle, media_touch_pending;
static u32 g_tp_cxy, media_touch_start;
static int media_input, media_touch_wake, down, last_delay;
static int init_failures, init_attempts, retry_delays;
static jmp_buf sampled;
static void os_sem_post(int *sem) { ++*sem; }
static void os_sem_pend(int *sem, int ticks) { (void)sem; (void)ticks; assert(0); }
static int badge_tp_init(void) {
    ++init_attempts;
    if (!init_failures) return 0;
    --init_failures;
    return -1;
}
static int badge_tp_read(u16 *x, u16 *y) { *x = 59; *y = 221; return down; }
static void os_time_dly(int ticks) {
    if (ticks == 50) { ++retry_delays; return; }
    last_delay = ticks;
    longjmp(sampled, 1);
}
''' + '\n'.join(functions) + r'''
static void sample(int pressed) {
    down = pressed;
    if (setjmp(sampled) == 0) axs_poll_task(NULL);
}
int main(void) {
    badge_media_key_pressed(0);
    assert(!media_input && badge_media_take_key() == NO_KEY);
    media_input_ready = 1;
    badge_media_key_pressed(0);
    assert(!media_input && badge_media_take_key() == NO_KEY);
    g_badge_locked = 1;
    badge_media_touch_pressed();
    assert(!media_input);
    badge_media_key_pressed(1);
    assert(media_input == 1 && badge_media_take_key() == 1 && badge_media_take_key() == NO_KEY);
    g_badge_locked = 0;
    media_idle_wait = media_touch_idle = 1;
    badge_media_key_pressed(0);
    assert(badge_media_take_key() == 0);
    media_input = 0;
    sample(0);
    assert(last_delay == 5 && !media_input);
    sample(1);
    assert(last_delay == 2 && media_input == 1);
    sample(1);
    assert(media_input == 1);
    sample(0);
    int32_t x, y;
    uint8_t state;
    get_touch_x_y_status(&x, &y, &state);
    assert(state && x == 59 && y == 221);
    get_touch_x_y_status(&x, &y, &state);
    assert(!state && x == 59 && y == 221);
    media_idle_wait = media_touch_idle = 0;
    sample(1);
    assert(last_delay == 2 && media_input == 1);

    init_attempts = 0;
    init_failures = 1;
    sample(0);
    assert(init_attempts == 2 && retry_delays == 1 && g_axs_ready);
    init_attempts = retry_delays = 0;
    init_failures = 2;
    sample(0);
    assert(init_attempts == 2 && retry_delays == 1 && !g_axs_ready);
}
'''
        compile_and_run(source)

    def test_held_press_keeps_the_ui_loop_running(self):
        lvgl = SDK / 'SDK/apps/common/ui/lvgl_v8'
        ui = (lvgl / 'lvgl_main.c').read_text()
        keypad = (lvgl / 'porting/lv_port_indev.c').read_text()
        start = ui.index('/* Lock / screensaver control + hardware buttons. */')
        lock_paths = ui[start:ui.index('/* Update screen:', start)]
        functions = [function(keypad, name) for name in (
            'void badge_adkey_publish(', 'u8 badge_adkey_read(', 'void badge_adkey_consume(')]
        start = keypad.index('static void keypad_read(', keypad.index('void badge_adkey_consume('))
        end = keypad.index('    struct key_event', start)
        functions.append(keypad[start:end].replace('\n{', '\n{\n    (void)indev_drv;', 1) + '}')
        source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define CONFIG_BADGE_MENU 1
#define CONFIG_BADGE_MEDIA_UI 1
#define NO_KEY 255
#define KEY_AD_NUM0 0
#define KEY_AD_NUM1 1
#define LV_KEY_ENTER 10
#define LV_KEY_NEXT 11
#define LV_INDEV_STATE_REL 0
#define LV_INDEV_STATE_PR 1
typedef uint8_t u8;
typedef int lv_obj_t;
typedef int lv_indev_drv_t;
typedef struct { unsigned int key; int state; } lv_indev_data_t;
static volatile u8 g_badge_adkey_kv = NO_KEY, badge_adkey_consumed;
static volatile unsigned char g_scanner_detail_active, g_scanner_retry_req;
static u8 g_badge_locked, media_input_ready, media_pending_key = NO_KEY;
static int media_input, refreshes, screen_home = 1, screen_menu;
static lv_indev_data_t keypad_state;
static lv_obj_t *g_lock_scr = &screen_home, *active_screen = &screen_home;
static lv_obj_t *lv_scr_act(void) { return active_screen; }
static void badge_lock_wake(void) { g_badge_locked = 0; }
static void badge_lock_enter(void) { g_badge_locked = 1; }
static void badge_media_ui_menu(void) { active_screen = &screen_menu; }
static u8 badge_media_take_key(void) {
    u8 key = media_pending_key;
    media_pending_key = NO_KEY;
    return key;
}
static void os_sem_pend(int *sem, int ticks) { (void)sem; (void)ticks; assert(0); }
static void os_time_dly(int ticks) { assert(ticks == 3); }
''' + '\n'.join(functions) + r'''
static void ui_tick(void) {
    do {
''' + lock_paths + r'''
        keypad_read(NULL, &keypad_state);
        ++refreshes;
    } while (0);
}
int main(void) {
    badge_adkey_publish(KEY_AD_NUM1);
    ui_tick();
    assert(active_screen == &screen_menu && refreshes == 1);
    assert(keypad_state.state == LV_INDEV_STATE_REL);
    badge_adkey_publish(KEY_AD_NUM0);
    ui_tick();
    assert(refreshes == 2 && keypad_state.state == LV_INDEV_STATE_REL);
    badge_adkey_publish(NO_KEY);
    badge_adkey_publish(KEY_AD_NUM0);
    ui_tick();
    assert(keypad_state.state == LV_INDEV_STATE_PR && keypad_state.key == LV_KEY_ENTER);

    active_screen = g_lock_scr;
    g_badge_locked = media_input_ready = 1;
    media_pending_key = KEY_AD_NUM0;
    ui_tick();
    assert(!g_badge_locked && refreshes == 3);
    ui_tick();
    assert(refreshes == 4 && !g_badge_locked && keypad_state.state == LV_INDEV_STATE_REL);

    badge_adkey_publish(NO_KEY);
    badge_adkey_publish(KEY_AD_NUM1);
    ui_tick();
    assert(active_screen == &screen_menu && refreshes == 5);

    g_scanner_detail_active = 1;
    badge_adkey_publish(NO_KEY);
    ui_tick();
    badge_adkey_publish(KEY_AD_NUM1);
    ui_tick();
    assert(g_scanner_retry_req == 1);
    g_scanner_retry_req = 0;
    ui_tick();
    assert(g_scanner_retry_req == 0);
    badge_adkey_publish(NO_KEY);
    badge_adkey_publish(KEY_AD_NUM1);
    ui_tick();
    assert(g_scanner_retry_req == 1);

    g_scanner_detail_active = 0;
    badge_adkey_publish(NO_KEY);
    ui_tick();
    badge_adkey_publish(KEY_AD_NUM1);
    ui_tick();
    assert(keypad_state.state == LV_INDEV_STATE_PR && keypad_state.key == LV_KEY_NEXT);
    ui_tick();
    assert(keypad_state.state == LV_INDEV_STATE_REL);
}
'''
        compile_and_run(source)

    def test_only_a_quiet_still_view_parks(self):
        media = (ROOT / 'firmware/badge-media/badge_media_ui.c').read_text()
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
static bool hint, receiving, playing, running_animation;
static int refreshes, svg_frames, video_frames;
static int active_screen, panel = 1, heading, *menu_heading;
static int menu_battery_percent, battery_reads, battery_redraws;
static uint32_t menu_battery_checked_ms;
static u8 battery = 74;
static uint32_t now, hint_started_ms, next_frame_ms = 1;
static int home, clip, image, badge_media_pixels;
static struct { bool animating; } svg;
static unsigned int timer_get_ms(void) { return now; }
static void dismiss_hint(void) { hint = false; }
static void refresh_receive(void) { ++refreshes; }
static int lv_scr_act(void) { return active_screen; }
u8 get_vbat_percent(void) { ++battery_reads; return battery; }
static void lv_label_set_text_fmt(int *label, const char *format, unsigned int percent) {
    assert(label == &heading && !strcmp(format, "Badge  |  Battery %u%%") && percent == battery);
    ++battery_redraws;
}
static void update_svg(void) { ++svg_frames; }
static unsigned int lv_disp_get_inactive_time(void *disp) { (void)disp; return now; }
static bool lv_anim_count_running(void) { return running_animation; }
static int badge_avi_next(int *avi, int pixels) { (void)avi; (void)pixels; ++video_frames; return 0; }
static void badge_avi_rewind(int *avi) { (void)avi; }
static void show_text(const char *s) { (void)s; }
static void lv_obj_invalidate(int obj) { (void)obj; }
static void advance_frame_clock(void) {}
''' + '\n'.join(function(media, name) for name in (
            'static void refresh_menu_battery(', 'unsigned int badge_media_ui_tick(')) + r'''
int main(void) {
    now = 1999;
    assert(badge_media_ui_tick() == 1);
    now = 2000;
    assert(badge_media_ui_tick() == 20);
    running_animation = true;
    assert(badge_media_ui_tick() == 1);
    running_animation = false;
    receiving = true;
    active_screen = panel;
    assert(badge_media_ui_tick() == 1 && refreshes == 1);
    receiving = false;
    active_screen = home;
    svg.animating = true;
    assert(badge_media_ui_tick() == 1 && svg_frames == 1);
    svg.animating = false;
    playing = true;
    assert(badge_media_ui_tick() == 1 && video_frames == 1);

    menu_heading = &heading;
    menu_battery_percent = -1;
    active_screen = panel;
    refresh_menu_battery();
    assert(menu_battery_percent == 74 && battery_reads == 1 && battery_redraws == 1);
    now += 999;
    assert(badge_media_ui_tick() == 1 && battery_reads == 1);
    ++now;
    assert(badge_media_ui_tick() == 1 && battery_reads == 2 && battery_redraws == 1);
    battery = 73;
    now += 1000;
    assert(badge_media_ui_tick() == 1 && battery_reads == 3 && battery_redraws == 2);
    active_screen = home;
    now += 1000;
    assert(badge_media_ui_tick() == 1 && battery_reads == 3);
    menu_heading = NULL;
    active_screen = panel;
    assert(badge_media_ui_tick() == 1 && battery_reads == 3);
}
'''
        compile_and_run(source)
