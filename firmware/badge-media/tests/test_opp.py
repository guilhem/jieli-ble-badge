"""Standard OBEX requests against the badge's receive and commit boundary."""
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

MEDIA = Path(__file__).resolve().parents[1]
WRAPPER = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
typedef unsigned OS_MUTEX;
typedef unsigned OS_SEM;
#define OS_ENTER_CRITICAL() ((void)0)
#define OS_EXIT_CRITICAL() ((void)0)
static int os_mutex_create(OS_MUTEX *m) { *m = 0; return 0; }
static int os_mutex_pend(OS_MUTEX *m, int t) {
    if (*m && t) return 1;
    assert(!*m); *m = 1; return 0;
}
static void os_mutex_post(OS_MUTEX *m) { assert(*m); *m = 0; }
static void os_mutex_del(OS_MUTEX *m, int t) { (void)m; (void)t; assert(0); }
static int os_sem_create(OS_SEM *s, int n) { *s = n; return 0; }
static void os_sem_del(OS_SEM *s, int t) { (void)s; (void)t; assert(0); }
static void os_sem_post(OS_SEM *s) { ++*s; }
static void os_sem_pend(OS_SEM *s, int t) { (void)s; (void)t; assert(0); }
static int task_create(void (*fn)(void *), void *arg, const char *name) {
    assert(fn && !arg && !strcmp(name, "badge_opp"));
    return 0;
}
static uint32_t tick;
static unsigned credit, response, bonded, encrypted, accepts, link_up;
unsigned auth_requests, negative_replies, other_rejections, classic, stack_wakes;
int send_error;
static uint8_t remote[] = {1,2,3,4,5,6}, other[] = {1,2,3,4,5,7};
static uint32_t sys_timer_get_ms(void) { return tick; }
static uint8_t *lmp_private_remote_addr_for_handler(int h) { return h == 42 ? remote : 0; }
static uint8_t get_remote_dev_info_index(void) { return 0; }
static const char *bt_get_local_name(void) { return "EC-BADGE"; }
static void lmp_hci_user_confirmation_request_reply(uint8_t *a) { assert(!memcmp(a, remote, 6)); ++accepts; }
static void lmp_hci_user_confirmation_request_negative_reply(uint8_t *a) {
    if (!memcmp(a, other, 6)) ++other_rejections;
    else { assert(!memcmp(a, remote, 6)); ++negative_replies; }
}
static void lmp_hci_authentication_requested(uint8_t *a);
#define badge_opp_start queue_start
#define badge_opp_stop queue_stop
#define badge_opp_confirm queue_confirm
#include "badge_opp.c"
#undef badge_opp_start
#undef badge_opp_stop
#undef badge_opp_confirm
void badge_opp_start(void) { queue_start(); control_step(); }
void badge_opp_stop(void) { queue_stop(); control_step(); }
void badge_opp_confirm(uint32_t number) { control_step(); queue_confirm(number); control_step(); }
unsigned bytes_written, begins, commits, cancels, advertised, opens, closes;
int space_error, close_error, cancel_on_start;
int badge_media_store_check_space(void) { return space_error; }
static rfcomm_handler emit;
void rfcomm_register_service_internal(void *unused, rfcomm_handler cb, uint8_t ch, uint16_t sz) {
    (void)unused; assert(ch == 5 && sz >= 255); emit = cb;
}
void rfcomm_accept_connection_internal(uint16_t channel) { assert(channel == 1); }
void rfcomm_disconnect_internal(uint16_t channel) { (void)channel; }
int rfcomm_send_internal(uint16_t channel, const uint8_t *p, uint16_t n) {
    assert(channel == 1 && n == p[2]);
    if (send_error) return send_error;
    if (!credit) return 114;
    response = p[0]; return 0;
}
void *get_edr_link_for_addr(const uint8_t *a) { return link_up && !memcmp(a, remote, 6) ? remote : 0; }
uint8_t link_conn_get_encrypt(void *link) { assert(link == remote); return encrypted ? 0x60 : 0; }
uint8_t get_link_key_stast(const uint8_t *a, uint8_t i) { (void)i; return bonded && !memcmp(a, remote, 6); }
static struct run_loop *registered_loop;
void stack_run_loop_register(struct run_loop *loop, void (*run)(struct run_loop *), void *var) {
    assert(!registered_loop); registered_loop = loop; loop->run = run; loop->var = var;
}
void stack_run_loop_resume(void) { assert(!mutex); ++stack_wakes; }
unsigned run_stack(void) { registered_loop->run(registered_loop); return response; }
int badge_opp_set_bredr(int on) {
    assert(!mutex && badge_opp_classic_enabled() == on);
    if (on) {
        ++opens;
        if (cancel_on_start) queue_stop();
    } else {
        ++closes;
        if (close_error) return close_error;
    }
    classic = on;
    return 0;
}
static void lmp_hci_authentication_requested(uint8_t *a) {
    assert(!mutex && !memcmp(a, remote, 6));
    ++auth_requests;
    if (!bonded) badge_opp_pair_request(a, 12345);
}
void security(unsigned has_bond, unsigned has_encryption) { bonded = has_bond; encrypted = has_encryption; }
unsigned phase(void) { control_step(); return badge_opp_poll().state; }
unsigned view_phase(void) { return badge_opp_poll().state; }
int view_stalled(void) { return badge_opp_poll().stalled; }
void hold_import_mutex(void) { os_mutex_pend(&mutex, 0); }
void release_import_mutex(void) { os_mutex_post(&mutex); }
void elapse(unsigned ms) { tick += ms; }
void lose_acl(void) { link_up = 0; }
void pair_directly(void) { badge_opp_pair_request(remote, 12345); }
void unrelated_prompt(void) { badge_opp_pair_request(other, 23456); }

char imported_name[BADGE_MEDIA_NAME_SIZE];
int begin_error, write_error, finish_error;
int badge_media_import_begin(const char *name, uint32_t len) {
    strcpy(imported_name, name); advertised = len; ++begins; return begin_error;
}
int badge_media_import_write(const void *p, size_t n) {
    if (write_error) return write_error;
    (void)p; bytes_written += n; return 0;
}
int badge_media_import_finish(void) {
    if (finish_error) return finish_error;
    ++commits; return 2;
}
void badge_media_import_abort(void) { ++cancels; }
void open_channel(void) {
    link_up = 1;
    uint8_t incoming[] = {0x82,9,6,5,4,3,2,1,5,1,0};
    uint8_t opened[] = {0x80,14,0,6,5,4,3,2,1,42,0,5,1,0,0,2};
    emit(4,0x43,incoming,sizeof(incoming)); emit(4,0x43,opened,sizeof(opened));
}
void open_window(void) {
    space_error = close_error = cancel_on_start = 0;
    badge_opp_bt_ready(); badge_opp_stop(); badge_opp_start(); open_channel();
    opens = closes = 0;
    bonded = encrypted = credit = 1; accepts = tick = 0;
    auth_requests = response = negative_replies = other_rejections = stack_wakes = 0;
    bytes_written = begins = commits = cancels = 0;
    begin_error = write_error = finish_error = send_error = 0;
}
int exchange(uint8_t *p, size_t n, int secure) {
    if (secure >= 0) bonded = encrypted = secure;
    response = 0;
    /* Fragment the OBEX size field as well as regular UTF-16 and body headers. */
    for (size_t pos=0; pos<n;) {
        size_t step = pos ? 47 : 2;
        if (step > n-pos) step = n-pos;
        emit(7, 0x43, p+pos, step); pos+=step;
    }
    return response;
}
void hold_credit(void) { credit = 0; }
unsigned grant_credit(void) {
    uint8_t event[] = {0x84,3,1,0,1}; credit = 1;
    emit(4,0x43,event,sizeof(event)); return response;
}
int stored_slot(void) { tick += 1001; control_step(); return badge_opp_poll().slot; }
void close_channel(void) { uint8_t e[] = {0x81,8,1,0,6,5,4,3,2,1}; emit(4,0x43,e,sizeof(e)); }
int pair_check(void) {
    badge_opp_pair_request(remote,12345);
    assert(phase() == BADGE_OPP_PAIRING);
    badge_opp_confirm(12346); assert(!accepts);
    badge_opp_confirm(12345); return accepts;
}
'''


def sdk_disconnect_fixture():
    """Exercise the pinned SDK's rescan functions if its patched checkout is available."""
    root = os.environ.get('BADGE_OPP_SDK')
    if not root:
        return ''
    source = (Path(root) / 'SDK/apps/watch/mode/bt/dual_conn.c').read_text()

    def function(signature):
        start = source.index(signature)
        return source[start:source.index('\n}', start) + 2]

    return r'''
#include <stdbool.h>
#define CONFIG_BADGE_MENU
#define CONFIG_BADGE_BOARD_E87
#define APP_MSG_BT_IN_PAIRING_MODE 1
static struct { unsigned page_scan_auto_disable, timer; } g_dual_conn;
unsigned scan_bits;
static void lmp_hci_write_scan_enable(unsigned bits) { scan_bits = bits; }
static bool page_list_empty(void) { return true; }
static int bt_get_total_connect_dev(void) { return 0; }
static void app_send_message(int type, int count) { (void)type; (void)count; }
static void sys_timeout_del(unsigned timer) { (void)timer; }
#define printf(...) ((void)0)
#pragma GCC diagnostic ignored "-Wunused-variable"
''' + function('static void write_scan_conn_enable(') + '\n' + function('static void dual_conn_state_handler()') + r'''
unsigned simulate_acl_disconnect(void) {
    scan_bits = 0xff;
    dual_conn_state_handler();
    return scan_bits;
}
'''

def header(kind, payload):
    return bytes([kind]) + struct.pack('>H', len(payload) + 3) + payload


def name(value):
    return header(1, (value + '\0').encode('utf-16-be'))


def length(value):
    return b'\xc3' + struct.pack('>I', value)


class Diagnostic(ctypes.Structure):
    _fields_ = [('result', ctypes.c_int), ('response', ctypes.c_uint8),
                ('bytes', ctypes.c_uint32), ('stage', ctypes.c_char_p)]


class ObjectPush(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name) / 'opp.so'
        for header_path in ['system/includes.h', 'btstack/avctp_user.h',
                            'btctrler/classic/hci_lmp.h', 'bt_event_func.h', 'user_cfg.h']:
            path = Path(cls.temp.name) / header_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared',
                        '-fPIC', '-I', cls.temp.name, '-I', str(MEDIA), '-x', 'c', '-', '-o', str(out)],
                       input=WRAPPER + sdk_disconnect_fixture(), text=True, check=True)
        cls.lib = ctypes.CDLL(str(out))
        cls.lib.exchange.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int)
        cls.lib.badge_opp_diagnostic.restype = Diagnostic

    def setUp(self):
        self.lib.open_window()

    def val(self, key):
        return ctypes.c_uint.in_dll(self.lib, key).value

    def diagnostic(self):
        sample = self.lib.badge_opp_diagnostic()
        return sample.stage.decode('ascii'), sample.result, sample.response, sample.bytes

    def exchange(self, op, data=b'', secure=True):
        frame = bytes([op]) + struct.pack('>H', len(data) + 3) + data
        return self.lib.exchange(frame, len(frame), secure)

    def connect(self, secure=True):
        return self.exchange(0x80, b'\x10\0\x04\0', secure)

    def test_cancel_leaves_the_ui_responsive_during_an_import(self):
        self.assertEqual(self.connect(), 0xa0)
        self.assertEqual(self.exchange(2, name('a.txt') + header(0x48, b'ab')), 0x90)
        self.assertEqual(self.lib.phase(), 3)  # Receiving
        self.lib.hold_import_mutex()
        try:
            self.lib.queue_stop()
            self.lib.queue_stop()
            self.assertEqual(self.lib.view_phase(), 8)  # Stopping
            self.lib.elapse(5000)
            self.assertEqual(self.lib.view_stalled(), 1)
            self.assertEqual((self.val('classic'), self.val('cancels')), (1, 0))
        finally:
            self.lib.release_import_mutex()
        self.assertEqual(self.lib.phase(), 0)
        self.assertEqual((self.val('classic'), self.val('cancels'), self.val('closes')), (0, 1, 1))

    def test_early_cancel_is_serialized_with_classic_startup(self):
        self.lib.badge_opp_stop()
        self.lib.queue_start()
        self.assertEqual(self.lib.view_phase(), 7)  # Starting
        self.lib.queue_stop()
        self.assertEqual(self.lib.phase(), 0)
        self.assertEqual((self.val('opens'), self.val('closes')), (0, 1))
        ctypes.c_int.in_dll(self.lib, 'cancel_on_start').value = 1
        self.lib.queue_start()
        self.assertEqual(self.lib.phase(), 8)  # Start completed, Cancel is still pending
        self.assertEqual(self.val('classic'), 1)
        self.assertEqual(self.lib.phase(), 0)
        self.assertEqual((self.val('opens'), self.val('closes'), self.val('classic')), (1, 2, 0))

    def test_saved_item_waits_for_controller_shutdown(self):
        self.assertEqual(self.connect(), 0xa0)
        self.assertEqual(self.exchange(0x82, name('a.txt') + header(0x49, b'hello')), 0xa0)
        self.assertEqual(self.lib.phase(), 3)
        ctypes.c_int.in_dll(self.lib, 'close_error').value = 1
        self.lib.elapse(1001)
        self.assertEqual(self.lib.phase(), 3)  # Do not navigate away from the receive screen
        self.lib.queue_start()  # A late request must not start a new session
        self.assertEqual((self.val('opens'), self.val('commits')), (0, 1))
        ctypes.c_int.in_dll(self.lib, 'close_error').value = 0
        self.assertEqual(self.lib.phase(), 4)
        self.assertEqual(self.lib.stored_slot(), 2)
        self.assertEqual(self.val('classic'), 0)

    def test_storage_preflight_does_not_start_bluetooth(self):
        self.lib.badge_opp_stop()
        ctypes.c_int.in_dll(self.lib, 'space_error').value = -2
        self.lib.queue_start()
        self.assertEqual(self.lib.view_phase(), 7)
        self.assertEqual(self.lib.phase(), 5)
        self.assertEqual(self.val('opens'), 0)
        self.assertEqual(self.diagnostic(), ('storage_preflight', -2, 0, 0))

    def test_single_final_put(self):
        self.assertEqual(self.connect(), 0xa0)
        self.assertEqual(self.exchange(0x82, name('Été.txt') + length(5) + header(0x49, b'hello')), 0xa0)
        self.assertEqual((self.val('commits'), self.val('bytes_written'), self.val('advertised')), (1, 5, 5))
        saved = (ctypes.c_char * 48).in_dll(self.lib, 'imported_name')
        self.assertEqual(saved.value.decode(), 'Été.txt')
        self.assertEqual(self.exchange(0x81), 0xa0)

    def test_streamed_photo(self):
        self.assertEqual(self.connect(), 0xa0)
        self.assertEqual(self.exchange(2, name('photo.jpg')), 0x90)
        for _ in range(300):
            self.assertEqual(self.exchange(2, header(0x48, b'x' * 800)), 0x90)
        self.assertEqual(self.val('commits'), 0)
        self.assertEqual(self.exchange(0x82, header(0x49, b'last')), 0xa0)
        self.assertEqual((self.val('begins'), self.val('bytes_written'), self.val('commits')), (1, 240004, 1))
        self.assertEqual(self.diagnostic(), ('stored', 1, 0xa0, 240004))

    def test_terminal_parser_diagnostic_survives_leaving_receive(self):
        samples = [
            ('header', b'\x42\x00', -1, 0),
            ('name_invalid', header(1, b'\0x'), -1, 0),
            ('name_duplicate', name('one.jpg') + name('two.jpg'), -1, 0),
            ('name_missing', header(0x49, b'hi'), -1, 0),
            ('body_length', name('one.jpg') + length(1) + header(0x49, b'hi'), -3, 0),
            ('final_body', name('one.jpg') + header(0x48, b'hi'), -1, 2),
            ('final_length', name('one.jpg') + length(3) + header(0x49, b'hi'), -1, 2),
        ]
        for stage, data, error, accepted in samples:
            with self.subTest(stage=stage):
                self.lib.open_window()
                self.assertEqual(self.connect(), 0xa0)
                self.assertEqual(self.exchange(0x82, data), 0xc0)
                self.assertEqual(self.lib.phase(), 5)
                self.lib.badge_opp_stop()
                self.lib.badge_opp_start()
                self.lib.badge_opp_stop()
                self.assertEqual(self.diagnostic(), (stage, error, 0xc0, accepted))

    def test_terminal_import_diagnostic(self):
        for operation, accepted in [('begin', 0), ('write', 3), ('finish', 5)]:
            with self.subTest(operation=operation):
                self.lib.open_window()
                self.assertEqual(self.connect(), 0xa0)
                self.assertEqual(self.exchange(2, name('photo.jpg') + length(5)), 0x90)
                if operation != 'begin':
                    self.assertEqual(self.exchange(2, header(0x48, b'abc')), 0x90)
                ctypes.c_int.in_dll(self.lib, operation + '_error').value = -1
                self.assertEqual(self.exchange(0x82, header(0x49, b'de')), 0xcf)
                self.assertEqual(self.diagnostic(), ('import_' + operation, -1, 0xcf, accepted))

    def test_radio_send_error_keeps_the_first_terminal_cause(self):
        for opcode, data, expected in [
            (2, name('one.jpg') + header(0x48, b'ab'), ('radio_send', -12, 0, 2)),
            (0x82, header(0x49, b'ab'), ('name_missing', -1, 0xc0, 0)),
        ]:
            with self.subTest(stage=expected[0]):
                self.lib.open_window()
                self.assertEqual(self.connect(), 0xa0)
                ctypes.c_int.in_dll(self.lib, 'send_error').value = -1
                self.assertEqual(self.exchange(opcode, data), 0)
                self.assertEqual(self.diagnostic(), expected)

    def test_abort_or_short_transfer_preserves_slot(self):
        for abort in (True, False):
            with self.subTest(abort=abort):
                self.lib.open_window()
                self.assertEqual(self.connect(), 0xa0)
                self.assertEqual(self.exchange(2, name('a.txt') + length(6) + header(0x48, b'ab')), 0x90)
                if abort:
                    self.assertEqual(self.exchange(0xff), 0xa0)
                    self.assertEqual(self.diagnostic(), ('peer_abort', -11, 0xa0, 2))
                else:
                    self.assertEqual(self.exchange(0x82, header(0x49, b'c')), 0xc0)
                self.assertEqual((self.val('cancels'), self.val('commits')), (1, 0))

    def test_disconnect_and_user_cancel(self):
        for cancel, stage in [(self.lib.close_channel, 'link_lost'), (self.lib.badge_opp_stop, 'cancel')]:
            self.lib.open_window()
            self.assertEqual(self.connect(), 0xa0)
            self.assertEqual(self.exchange(2, name('a.txt') + header(0x48, b'partial')), 0x90)
            cancel()
            self.assertEqual((self.val('cancels'), self.val('commits')), (1, 0))
            self.assertEqual(self.diagnostic(), (stage, -11, 0, 7))

    def test_credit_backpressure_and_numeric_confirmation(self):
        self.assertEqual(self.lib.pair_check(), 1)
        self.assertEqual(self.connect(), 0xa0)
        self.lib.hold_credit()
        self.assertEqual(self.exchange(0x82, name('a.txt') + header(0x49, b'hi')), 0)
        self.assertEqual(self.lib.grant_credit(), 0xa0)
        self.assertEqual(self.lib.stored_slot(), 2)
        self.assertEqual(self.val('commits'), 1)

    def test_pair_on_first_connect(self):
        self.lib.unrelated_prompt()  # An RFCOMM sender already owns the channel.
        self.assertEqual(self.val('other_rejections'), 1)
        self.assertEqual(self.lib.phase(), 1)
        self.assertEqual(self.connect(False), 0)
        self.assertEqual(self.val('auth_requests'), 1)
        self.lib.elapse(3600000)
        self.assertEqual(self.lib.run_stack(), 0)
        self.assertEqual(self.lib.phase(), 2)  # Native numeric comparison
        self.lib.unrelated_prompt()
        self.assertEqual(self.val('other_rejections'), 2)
        self.lib.badge_opp_confirm(23456)
        self.assertEqual(self.lib.phase(), 2)
        self.assertEqual(self.lib.run_stack(), 0)
        self.lib.badge_opp_confirm(12345)
        self.lib.security(1, 0)
        self.lib.elapse(3600000)
        self.assertEqual(self.lib.run_stack(), 0)
        self.assertEqual(self.lib.phase(), 6)  # Waiting for the other device
        self.assertEqual(self.val('negative_replies'), 0)
        self.assertEqual(self.val('begins'), 0)
        self.lib.security(0, 1)
        self.assertEqual(self.lib.run_stack(), 0)
        self.assertEqual(self.lib.phase(), 6)
        self.lib.security(1, 1)
        self.assertEqual(self.lib.run_stack(), 0xa0)
        self.assertEqual(self.exchange(0x82, name('a.txt') + header(0x49, b'hello')), 0xa0)

    def test_prepaired_sender_upgrades_its_low_security_socket(self):
        self.lib.security(1, 0)
        self.assertEqual(self.connect(-1), 0)
        self.assertEqual(self.val('auth_requests'), 1)
        self.assertEqual(self.lib.phase(), 1)  # No new confirmation for the existing key
        self.assertGreater(self.val('stack_wakes'), 0)
        self.assertEqual(self.lib.run_stack(), 0)
        self.lib.security(1, 1)
        self.assertEqual(self.lib.run_stack(), 0xa0)

    def test_connection_without_a_pairing_prompt_still_expires(self):
        self.lib.security(1, 0)
        self.assertEqual(self.connect(-1), 0)
        self.lib.elapse(119999)
        self.assertEqual(self.lib.phase(), 1)
        self.lib.elapse(1)
        self.assertEqual(self.lib.phase(), 5)
        self.assertEqual((self.val('classic'), self.val('begins')), (0, 0))

    def test_standalone_pair_waits_for_both_sides_and_restarts_idle_clock(self):
        self.lib.close_channel()  # Pairing from system settings before opening OPP.
        self.lib.security(0, 0)
        self.lib.pair_directly()
        self.lib.elapse(3600000)
        self.lib.run_stack()
        self.assertEqual(self.lib.phase(), 2)
        self.lib.badge_opp_confirm(12345)
        self.lib.elapse(3600000)
        self.lib.run_stack()
        self.assertEqual(self.lib.phase(), 6)
        self.lib.security(1, 1)
        self.lib.run_stack()
        self.assertEqual(self.lib.phase(), 1)
        self.lib.elapse(119999)
        self.assertEqual(self.lib.phase(), 1)
        self.lib.elapse(1)
        self.assertEqual(self.lib.phase(), 5)

    def test_unsecured_put_never_writes(self):
        self.assertEqual(self.connect(False), 0)
        self.lib.elapse(3600000)
        self.assertEqual(self.lib.run_stack(), 0)
        self.assertEqual(self.exchange(0x82, name('a.txt') + header(0x49, b'hello'), False), 0)
        self.assertEqual((self.val('begins'), self.val('bytes_written')), (0, 0))

    def test_cancel_before_or_after_confirmation(self):
        for confirm in (False, True):
            with self.subTest(confirm=confirm):
                self.lib.open_window()
                self.assertEqual(self.connect(False), 0)
                if confirm:
                    self.lib.badge_opp_confirm(12345)
                self.lib.badge_opp_stop()
                self.lib.security(1, 1)
                self.assertEqual(self.lib.run_stack(), 0)
                self.assertEqual(self.lib.phase(), 0)
                self.assertEqual(self.val('negative_replies'), int(not confirm))
                self.assertEqual((self.val('classic'), self.val('begins'), self.val('bytes_written')), (0, 0, 0))
                self.assertEqual(self.diagnostic(), ('pair_cancel', -13, 0, 0))

    def test_retry_after_loss_before_or_after_confirmation(self):
        for loss in (self.lib.close_channel, self.lib.lose_acl):
            for confirm in (False, True):
                with self.subTest(loss=loss, confirm=confirm):
                    self.lib.open_window()
                    self.assertEqual(self.connect(False), 0)
                    if confirm:
                        self.lib.badge_opp_confirm(12345)
                    self.lib.elapse(3600000)
                    loss()
                    self.assertEqual(self.lib.run_stack(), 0)
                    self.assertEqual(self.lib.phase(), 1)
                    self.assertEqual(self.diagnostic(), ('pair_link', -13, 0, 0))
                    self.assertEqual((self.val('classic'), self.val('begins')), (1, 0))
                    self.lib.open_channel()
                    self.assertEqual(self.connect(False), 0)
                    self.lib.badge_opp_confirm(12345)
                    self.lib.security(1, 1)
                    self.assertEqual(self.lib.run_stack(), 0xa0)
                    self.assertEqual(self.exchange(0x82, name('a.txt') + header(0x49, b'hi')), 0xa0)
                    self.assertEqual(self.diagnostic(), ('stored', 1, 0xa0, 2))

    def test_standalone_pairing_retries_after_acl_loss(self):
        self.lib.close_channel()
        self.lib.security(0, 0)
        self.lib.pair_directly()
        self.lib.elapse(3600000)
        self.lib.lose_acl()
        self.lib.run_stack()
        self.assertEqual(self.lib.phase(), 1)
        self.assertEqual(self.val('classic'), 1)
        self.lib.open_channel()
        self.assertEqual(self.connect(False), 0)
        self.assertEqual(self.lib.phase(), 2)

    def test_real_transfer_still_has_an_inactivity_deadline(self):
        self.assertEqual(self.connect(), 0xa0)
        self.assertEqual(self.exchange(2, name('a.txt') + header(0x48, b'ab')), 0x90)
        self.lib.elapse(30001)
        self.assertEqual(self.lib.phase(), 5)
        self.assertEqual((self.val('classic'), self.val('cancels'), self.val('commits')), (0, 1, 0))
        self.assertEqual(self.diagnostic(), ('timeout', -10, 0, 2))

    @unittest.skipUnless(os.environ.get('BADGE_OPP_SDK'), 'requires patched SDK checkout')
    def test_pair_then_sdp_disconnect_restores_receive_scan(self):
        self.assertEqual(self.val('classic'), 1)
        self.assertEqual(self.lib.pair_check(), 1)
        self.lib.close_channel()
        self.assertEqual(self.lib.phase(), 1)  # Receive keeps waiting for a file.
        self.assertEqual(self.lib.simulate_acl_disconnect(), 3)
        self.lib.badge_opp_stop()
        self.assertEqual(self.val('classic'), 0)
        self.assertEqual(self.lib.simulate_acl_disconnect(), 0)
