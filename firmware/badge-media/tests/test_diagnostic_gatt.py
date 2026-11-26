"""Exercise E87's compiled GATT table and ATT read handler on the host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SDK = Path(os.environ.get('BADGE_SDK', ROOT / 'tools/community-re/jieli-sdks/e_badge_707_sdk_200'))
APPS = SDK / 'SDK/apps/common'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


class DiagnosticGatt(unittest.TestCase):
    def test_profile_and_long_reads(self):
        server = (APPS / 'third_party_profile/jieli/rcsp/ble_rcsp_server.c').read_text()
        profile = (APPS / 'third_party_profile/multi_protocol_main.c').read_text()
        ui = (APPS / 'ui/lvgl_v8/lvgl_main.c').read_text()
        uuid_macro = next(line for line in profile.splitlines() if line.startswith('#define QIX_UUID128_LE'))
        table = function(profile, 'uint8_t rcsp_profile_data[] =') + ';'
        handler = function(server, 'uint16_t bt_rcsp_custom_att_read_callback(')
        diagnostic = server[server.index('static char diagnostic_value['):
                            server.index('\n#endif', server.index('static u16 badge_diagnostic_att_read('))]
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_BADGE_MENU
#define printf(...) ((void)0)
typedef uint8_t u8;
typedef uint16_t u16, hci_con_handle_t;
static u8 g_on_update_scr, g_badge_update_active, ota;
u8 get_ota_status(void) { return ota; }
static unsigned int probes;
unsigned int badge_media_diagnostic_read(char *buffer, unsigned int capacity) {
    ++probes;
    assert(capacity >= 7);
    memcpy(buffer, "abcdef", 7);
    return 6;
}
''' + uuid_macro + '\n' + table.replace('rcsp_profile_data', 'existing') + '''
#define CONFIG_BADGE_BOARD_E87
''' + table + '\n' + function(ui, 'u8 badge_update_diagnostics_enabled(') + '\n' + diagnostic + '\n' + handler + r'''
static unsigned int read_value(u16 offset, u8 *p, u16 n) {
    return bt_rcsp_custom_att_read_callback(0, 7, 0x3c, offset, p, n);
}
int main(void) {
    size_t old = sizeof(existing) - 2;
    assert(!memcmp(existing, rcsp_profile_data, old));
    const u8 expected[] = {
        24,0, 2,0, 0x3a,0, 0,0x28, QIX_UUID128_LE(0x10),
        27,0, 2,0, 0x3b,0, 3,0x28, 2,0x3c,0, QIX_UUID128_LE(0x11),
        22,0, 2,3, 0x3c,0, QIX_UUID128_LE(0x11), 0,0
    };
    assert(sizeof(rcsp_profile_data) == old + sizeof(expected));
    assert(!memcmp(rcsp_profile_data + old, expected, sizeof(expected)));

    u8 buf[8] = {0};
    assert(read_value(0, 0, 0) == 0 && probes == 0);
    g_on_update_scr = 1;
    assert(read_value(0, 0, 0) == 6 && probes == 1);
    assert(read_value(0, buf, 2) == 2 && !memcmp(buf, "ab", 2));
    /* BR35's ATT layer repeats its offset-zero length probe for every Read Blob. */
    assert(read_value(0, 0, 0) == 6);
    assert(read_value(9, buf, 1) == 0);
    assert(read_value(2, buf + 2, 8 - 2) == 4 && !memcmp(buf, "abcdef", 6));
    assert(probes == 1);
    assert(read_value(0, 0, 0) == 6 && probes == 2);
    g_badge_update_active = 1;
    assert(read_value(2, buf, 2) == 0 && probes == 2);
    g_badge_update_active = 0;
    ota = 1;
    assert(read_value(0, 0, 0) == 0 && probes == 2);
    ota = 0;
    assert(read_value(0, 0, 0) == 6 && probes == 3);
    g_on_update_scr = 0;
    assert(read_value(0, buf, 1) == 0 && probes == 3);
}
'''
        with tempfile.TemporaryDirectory() as work:
            out = Path(work) / 'diagnostic-gatt'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Werror',
                            '-x', 'c', '-', '-o', str(out)], input=source, text=True, check=True)
            subprocess.run([str(out)], check=True)
