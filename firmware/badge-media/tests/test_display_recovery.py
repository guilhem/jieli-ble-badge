"""Exercise the E87 display deadline with an independent fake scheduler."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SDK = Path(os.environ.get("BADGE_SDK", ROOT / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"))


class DisplayRecovery(unittest.TestCase):
    def test_display_deadline(self):
        source = (SDK / "SDK/apps/common/ui/lvgl_v8/lvgl_main.c").read_text()
        start = source.index("#define BADGE_DISPLAY_TIMEOUT_MS")
        stop = source.index("/* The BLE stack", start)
        code = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OS_ENTER_CRITICAL()
#define OS_EXIT_CRITICAL()
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u8 g_on_update_scr, g_badge_update_active, ota;
static u32 now, delay;
static void (*deadline)(void *);
static int resets, fail_schedule, ota_during_log;

static u32 timer_get_ms(void) { return now; }
static u8 get_ota_status(void) { return ota; }
static void cpu_reset(void) { ++resets; }
static u16 sys_timeout_add_2_task(void *priv, void (*func)(void *), u32 msec,
                                 const char *task_name) {
    assert(!priv && !deadline && msec && !strcmp(task_name, "app_core"));
    if (fail_schedule) return 0;
    deadline = func;
    delay = msec;
    return 1;
}

static int report(const char *format, ...) {
    (void)format;
    if (ota_during_log) ota = 1;
    return 0;
}
#define printf report

""" + source[start:stop] + r"""
static void fire(void) {
    void (*callback)(void *) = deadline;
    assert(callback);
    deadline = NULL;
    callback(NULL);
}

int main(void) {
    assert(!deadline);
    badge_display_watch_begin("refresh");
    assert(delay == 5000);
    now = 1000;
    badge_display_watch_end();
    now = 4000;
    badge_display_watch_begin("wake");
    badge_display_watch_begin("refresh");
    badge_display_watch_end();
    now = 5000;
    fire();
    assert(!resets && delay == 4000);
    now = 9000;

    g_on_update_scr = 1;
    fire();
    assert(!resets);
    g_on_update_scr = 0;
    g_badge_update_active = 1;
    now += delay;
    fire();
    assert(!resets);
    g_badge_update_active = 0;
    ota = 1;
    now += delay;
    fire();
    assert(!resets);
    ota = 0;
    ota_during_log = 1;
    now += delay;
    fire();
    assert(!resets && ota);
    ota_during_log = ota = 0;
    now += delay;
    fire();
    assert(resets == 1);

    badge_display_watch_end();
    now += delay;
    fire();
    assert(!deadline);

    fail_schedule = 1;
    badge_display_watch_begin("refresh");
    assert(!deadline);
    badge_display_watch_end();
    fail_schedule = 0;
    badge_display_watch_begin("refresh");
    assert(deadline);
    badge_display_watch_end();
    now += delay;
    fire();
    assert(!deadline && resets == 1);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", binary],
                input=code,
                text=True,
                check=True,
            )
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
