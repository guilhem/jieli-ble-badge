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
    def test_press_and_release_before_ui_reads(self):
        apps = SDK / 'SDK/apps'
        ui = (apps / 'common/ui/lvgl_v8/lvgl_main.c').read_text()
        touch = (apps / 'watch/board/br35/board_ac707n_demo/board_ac707n_demo.c').read_text()
        functions = [function(ui, name) for name in (
            'void badge_media_key_pressed(', 'void badge_media_touch_pressed(', 'static u8 badge_media_take_key(')]
        functions.extend(function(touch, name) for name in (
            'static void axs_poll_task(', 'void get_touch_x_y_status('))
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
static jmp_buf sampled;
static void os_sem_post(int *sem) { ++*sem; }
static void os_sem_pend(int *sem, int ticks) { (void)sem; (void)ticks; assert(0); }
static int badge_tp_init(void) { return 0; }
static int badge_tp_read(u16 *x, u16 *y) { *x = 59; *y = 221; return down; }
static void os_time_dly(int ticks) { last_delay = ticks; longjmp(sampled, 1); }
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
static bool hint, receiving, playing, other_screen, running_animation;
static int refreshes, svg_frames, video_frames;
static uint32_t now, hint_started_ms, next_frame_ms = 1;
static int home, clip, image, badge_media_pixels;
static struct { bool animating; } svg;
static unsigned int timer_get_ms(void) { return now; }
static void dismiss_hint(void) { hint = false; }
static void refresh_receive(void) { ++refreshes; }
static int lv_scr_act(void) { return other_screen ? 1 : home; }
static void update_svg(void) { ++svg_frames; }
static unsigned int lv_disp_get_inactive_time(void *disp) { (void)disp; return now; }
static bool lv_anim_count_running(void) { return running_animation; }
static int badge_avi_next(int *avi, int pixels) { (void)avi; (void)pixels; ++video_frames; return 0; }
static void badge_avi_rewind(int *avi) { (void)avi; }
static void show_text(const char *s) { (void)s; }
static void lv_obj_invalidate(int obj) { (void)obj; }
static void advance_frame_clock(void) {}
''' + function(media, 'unsigned int badge_media_ui_tick(') + r'''
int main(void) {
    now = 1999;
    assert(badge_media_ui_tick() == 1);
    now = 2000;
    assert(badge_media_ui_tick() == 20);
    running_animation = true;
    assert(badge_media_ui_tick() == 1);
    running_animation = false;
    other_screen = receiving = true;
    assert(badge_media_ui_tick() == 1 && refreshes == 1);
    other_screen = receiving = false;
    svg.animating = true;
    assert(badge_media_ui_tick() == 1 && svg_frames == 1);
    svg.animating = false;
    playing = true;
    assert(badge_media_ui_tick() == 1 && video_frames == 1);
}
'''
        compile_and_run(source)
