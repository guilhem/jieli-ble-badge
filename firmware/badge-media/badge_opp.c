#include "badge_opp.h"
#include "badge_media_store.h"
#include "system/includes.h"
#include <string.h>

/* GOEP 1.1 over the SDK's Object Push SDP record and RFCOMM channel. */
enum {
    OPP_CHANNEL = 5, OBEX_MAX = 1024,
    CONNECT = 0x80, DISCONNECT = 0x81, PUT = 2, PUT_FINAL = 0x82, ABORT_PUT = 0xff,
    CONTINUE = 0x90, SUCCESS = 0xa0, BAD_REQUEST = 0xc0, UNAUTHORIZED = 0xc1,
    FORBIDDEN = 0xc3, TOO_LARGE = 0xcd, UNSUPPORTED = 0xcf, IO_ERROR = 0xd0, FULL = 0xe0,
    NAME = 1, BODY = 0x48, END_BODY = 0x49, LENGTH = 0xc3,
};
static struct badge_opp_status status = {.slot = -1};
static struct badge_opp_diagnostic last = {.stage = "none"};
static int connected, importing, have_length;
static uint32_t expected;
static char filename[BADGE_MEDIA_NAME_SIZE];

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void discard_object(void)
{
    if (importing) badge_media_import_abort();
    importing = have_length = 0;
    expected = 0;
    filename[0] = 0;
}

/* The GATT reader runs with the SDK BLE mutex held. Only copy this small
 * snapshot under the critical section, independently of the OPP mutex. */
static void record_result(int result, uint8_t response, const char *stage)
{
    OS_ENTER_CRITICAL();
    last = (struct badge_opp_diagnostic){result, response, status.received, stage};
    OS_EXIT_CRITICAL();
}

struct badge_opp_diagnostic badge_opp_diagnostic(void)
{
    struct badge_opp_diagnostic snapshot;
    OS_ENTER_CRITICAL();
    snapshot = last;
    OS_EXIT_CRITICAL();
    return snapshot;
}

static int fail(int error, int response, const char *stage)
{
    discard_object();
    if (status.slot < 0) {
        if (status.state != BADGE_OPP_FAILED) record_result(error, response, stage);
        status.error = error;
        status.state = BADGE_OPP_FAILED;
    }
    return response;
}

static int import_error(int error, const char *stage)
{
    int response = IO_ERROR;
    if (error == BADGE_MEDIA_ERR_SIZE) response = TOO_LARGE;
    if (error == BADGE_MEDIA_ERR_FULL || error == BADGE_MEDIA_ERR_SETUP) response = FULL;
    if (error == BADGE_MEDIA_ERR_FORMAT) response = UNSUPPORTED;
    return fail(error, response, stage);
}

/* The archive stores a display basename. Keep the tail (including extension)
 * of a long Unicode name, dropping complete UTF-8 characters from the front. */
static int read_name(const uint8_t *p, size_t len)
{
    size_t used = 0;
    if (len < 2 || (len & 1) || be16(p + len - 2)) return -1;
    for (size_t i = 0; i < len - 2; i += 2) {
        uint32_t c = be16(p + i);
        if (c < 32 || c == '/' || c == '\\' || c == 127) return -1;
        if (c >= 0xd800 && c < 0xe000) {
            if (c >= 0xdc00 || i + 4 > len - 2) return -1;
            uint16_t low = be16(p + i + 2);
            if (low < 0xdc00 || low > 0xdfff) return -1;
            c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
            i += 2;
        }
        unsigned n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (used + n >= sizeof(filename)) {
            size_t drop = used + n - sizeof(filename) + 1;
            while (((uint8_t)filename[drop] & 0xc0) == 0x80) ++drop;
            memmove(filename, filename + drop, used - drop);
            used -= drop;
        }
        static const uint8_t prefix[] = {0, 0, 0xc0, 0xe0, 0xf0};
        filename[used] = prefix[n] | (c >> (6 * (n - 1)));
        for (unsigned k = 1; k < n; ++k)
            filename[used + k] = 0x80 | ((c >> (6 * (n - 1 - k))) & 63);
        used += n;
    }
    filename[used] = 0;
    return used ? 0 : -1;
}

/* Parse all metadata before passing body bytes to the import transaction. */
static int put_object(const uint8_t *pkt, size_t len, int final)
{
    int end_seen = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t pos = 3; pos < len;) {
            uint8_t id = pkt[pos];
            size_t remaining = len - pos, hdr = id < 0x80 ? 3 : 1;
            if (remaining < hdr) return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "header");
            size_t size = id < 0x80 ? be16(pkt + pos + 1) : id < 0xc0 ? 2 : 5;
            if (size < hdr || size > remaining) return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "header");
            const uint8_t *p = pkt + pos + hdr;
            size_t n = size - hdr;
            if (!pass && id == NAME) {
                if (importing || filename[0])
                    return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "name_duplicate");
                if (read_name(p, n))
                    return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "name_invalid");
            } else if (!pass && id == LENGTH) {
                uint32_t length = ((uint32_t)be16(p) << 16) | be16(p + 2);
                if ((have_length && length != expected) || length < status.received)
                    return fail(BADGE_MEDIA_ERR_SIZE, BAD_REQUEST, "length");
                have_length = 1;
                expected = length;
            } else if (pass && (id == BODY || id == END_BODY)) {
                if (end_seen || (id == END_BODY && !final))
                    return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "body_order");
                if (!filename[0]) return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "name_missing");
                end_seen = id == END_BODY;
                if (n > UINT32_MAX - status.received ||
                    (have_length && n > expected - status.received))
                    return fail(BADGE_MEDIA_ERR_SIZE, BAD_REQUEST, "body_length");
                if (!importing) {
                    int err = badge_media_import_begin(filename, expected);
                    if (err < 0) return import_error(err, "import_begin");
                    importing = 1;
                }
                if (n) {
                    int err = badge_media_import_write(p, n);
                    if (err < 0) return import_error(err, "import_write");
                    status.received += n;
                }
            }
            pos += size;
        }
    }
    status.state = BADGE_OPP_RECEIVING;
    if (!final) return CONTINUE;
    if (!importing || !end_seen)
        return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "final_body");
    if (have_length && status.received != expected)
        return fail(BADGE_MEDIA_ERR_FORMAT, BAD_REQUEST, "final_length");
    int slot = badge_media_import_finish();
    if (slot < 0) return import_error(slot, "import_finish");
    importing = 0;
    status.slot = slot;
    record_result(1, SUCCESS, "stored");
    return SUCCESS;
}

static int request(const uint8_t *pkt, size_t len, int secure)
{
    if (len < 3 || len > OBEX_MAX || be16(pkt + 1) != len) return BAD_REQUEST;
    if (pkt[0] == DISCONNECT || pkt[0] == ABORT_PUT) {
        if (!connected) return BAD_REQUEST;
        if (status.state == BADGE_OPP_RECEIVING && status.slot < 0)
            fail(BADGE_OPP_ERR_DISCONNECTED, SUCCESS, pkt[0] == ABORT_PUT ? "peer_abort" : "peer_close");
        if (pkt[0] == DISCONNECT) connected = 0;
        return SUCCESS;
    }
    if ((status.state != BADGE_OPP_WAITING && status.state != BADGE_OPP_RECEIVING) || status.slot >= 0)
        return FORBIDDEN;
    if (!secure) return fail(BADGE_OPP_ERR_PAIRING, UNAUTHORIZED, "pairing");
    if (pkt[0] == CONNECT) {
        if (connected || len < 7 || (pkt[3] >> 4) != 1 || be16(pkt + 5) < 255) return BAD_REQUEST;
        connected = 1;
        return SUCCESS;
    }
    if (!connected || (pkt[0] != PUT && pkt[0] != PUT_FINAL)) return BAD_REQUEST;
    return put_object(pkt, len, pkt[0] == PUT_FINAL);
}

#include "btstack/avctp_user.h"
#include "btctrler/classic/hci_lmp.h"
#include "bt_event_func.h"
#include "user_cfg.h"

/* BR35 SDK exports used by its own OPP transport, missing from public headers. */
typedef void (*rfcomm_handler)(uint8_t, uint16_t, uint8_t *, uint16_t);
extern void rfcomm_register_service_internal(void *, rfcomm_handler, uint8_t, uint16_t);
extern void rfcomm_accept_connection_internal(uint16_t);
extern void rfcomm_disconnect_internal(uint16_t);
extern int rfcomm_send_internal(uint16_t, const uint8_t *, uint16_t);
extern void *get_edr_link_for_addr(const uint8_t *);
extern uint8_t link_conn_get_encrypt(void *);
extern uint8_t get_link_key_stast(const uint8_t *, uint8_t);
/* Executes the controller transition in app_core and waits for completion. */
extern int badge_opp_set_bredr(int on);
struct run_loop {
    struct run_loop *next;
    void (*run)(struct run_loop *);
    void *var;
};
extern void stack_run_loop_register(struct run_loop *, void (*)(struct run_loop *), void *);
extern void stack_run_loop_resume(void);

static OS_MUTEX mutex;
static uint8_t ready, security_peer[6], acl_open;
static volatile uint8_t radio_on;
static uint16_t rfcomm_cid, l2cap_cid, acl_handle, used;
/* A complete OBEX CONNECT is retained until this peer is authorized. */
static uint8_t connect_pending;
static struct run_loop security_loop;
static uint8_t packet[OBEX_MAX];
static uint8_t pending_reply[7], sending, commit_acknowledged;
static uint32_t last_activity;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int associating(void)
{
    return status.state == BADGE_OPP_PAIRING || status.state == BADGE_OPP_SECURING;
}

/* With an RFCOMM channel, resolve its actual ACL peer. Standalone system
 * pairing already supplies the peer address in the numeric comparison event. */
static int peer_security(int from_channel, int require_bond, uint8_t peer[6])
{
    if (from_channel && !acl_open) return -1;
    int encrypted = -1;
    /* The controller owns these two pointers. Sample them before it can retire the ACL. */
    OS_ENTER_CRITICAL();
    const uint8_t *address = from_channel ? lmp_private_remote_addr_for_handler(acl_handle) : peer;
    if (address) {
        if (from_channel) memcpy(peer, address, 6);
        void *link = get_edr_link_for_addr(peer);
        if (link) encrypted = (link_conn_get_encrypt(link) & 0x60) == 0x60;
    }
    OS_EXIT_CRITICAL();
    if (encrypted <= 0) return encrypted;
    return !require_bond || get_link_key_stast(peer, get_remote_dev_info_index());
}

static void forget_channel(void)
{
    connect_pending = 0;
    rfcomm_cid = l2cap_cid = acl_handle = used = 0;
    acl_open = pending_reply[0] = commit_acknowledged = 0;
    connected = 0;
    discard_object();
}

static void wait_for_file(void)
{
    status.state = BADGE_OPP_WAITING;
    status.number = 0;
    last_activity = sys_timer_get_ms();
}

/* RFCOMM grants outgoing credits asynchronously. Only the stack task sends. */
static void flush_reply(uint16_t channel)
{
    uint8_t reply[sizeof(pending_reply)];
    os_mutex_pend(&mutex, 0);
    if (rfcomm_cid != channel || !pending_reply[0] || sending) {
        os_mutex_post(&mutex);
        return;
    }
    sending = 1;
    memcpy(reply, pending_reply, sizeof(reply));
    os_mutex_post(&mutex);
    int result = rfcomm_send_internal(channel, reply, reply[2]);
    os_mutex_pend(&mutex, 0);
    sending = 0;
    if (rfcomm_cid == channel && !result) {
        pending_reply[0] = 0;
        last_activity = sys_timer_get_ms();
        if (status.slot >= 0) commit_acknowledged = 1;
    } else if (rfcomm_cid == channel && result != 114) {
        fail(BADGE_OPP_ERR_RADIO, 0, "radio_send");
    }
    os_mutex_post(&mutex);
    if (result && result != 114) rfcomm_disconnect_internal(channel);
}

static void rfcomm_event(uint16_t mux, uint8_t *p, uint16_t n)
{
    if (n < 2) return;
    uint16_t accept = 0, drop = 0, credit = 0;
    int reject = 0;
    uint8_t peer[6];
    os_mutex_pend(&mutex, 0);
    switch (p[0]) {
    case 0x82: /* RFCOMM_EVENT_INCOMING_CONNECTION */
        if (n >= 11 && p[8] == OPP_CHANNEL) {
            uint16_t incoming = le16(p + 9);
            if (!rfcomm_cid && radio_on &&
                (status.state == BADGE_OPP_WAITING || associating())) {
                rfcomm_cid = accept = incoming;
                l2cap_cid = mux;
            } else drop = incoming;
        }
        break;
    case 0x80: /* RFCOMM_EVENT_CHANNEL_OPENED */
        if (n >= 16 && mux == l2cap_cid && rfcomm_cid && le16(p + 12) == rfcomm_cid) {
            if (p[2]) {
                record_result(BADGE_OPP_ERR_RADIO, 0, "radio_open");
                forget_channel();
            } else {
                acl_handle = le16(p + 9);
                acl_open = 1;
                if (associating() && (peer_security(1, 0, peer) < 0 || memcmp(peer, security_peer, 6))) {
                    drop = rfcomm_cid;
                    forget_channel();
                }
            }
        }
        break;
    case 0x84: /* RFCOMM_EVENT_CREDITS */
        if (n >= 5 && mux == l2cap_cid) credit = le16(p + 2);
        break;
    case 0x81: /* RFCOMM_EVENT_CHANNEL_CLOSED */
        if (n >= 4 && mux == l2cap_cid && rfcomm_cid && le16(p + 2) == rfcomm_cid) {
            if (status.slot >= 0) status.state = BADGE_OPP_DONE;
            else if (status.state == BADGE_OPP_RECEIVING) fail(BADGE_OPP_ERR_DISCONNECTED, 0, "link_lost");
            else if (acl_open && (connect_pending || associating())) {
                reject = status.state == BADGE_OPP_PAIRING;
                if (reject) memcpy(peer, security_peer, 6);
                record_result(BADGE_OPP_ERR_PAIRING, 0, "pair_link");
                wait_for_file();
            }
            forget_channel();
        }
        break;
    }
    os_mutex_post(&mutex);
    if (reject) lmp_hci_user_confirmation_request_negative_reply(peer);
    if (accept) rfcomm_accept_connection_internal(accept);
    if (drop) rfcomm_disconnect_internal(drop);
    if (credit) flush_reply(credit);
}

static void queue_response(int secure)
{
    uint8_t reply[] = {0, 0, 3, 0x10, 0, OBEX_MAX >> 8, OBEX_MAX & 255};
    reply[0] = request(packet, used, secure);
    if (packet[0] == CONNECT && reply[0] == SUCCESS) reply[2] = 7;
    memcpy(pending_reply, reply, sizeof(reply));
    used = 0;
}

/* BR35 enables encryption on successful Classic Authentication Complete.
 * The profile loop runs after stack messages. The Receive screen also wakes
 * it while a CONNECT waits for the controller and the saved link key. */
static void check_security(struct run_loop *loop)
{
    (void)loop;
    if (!radio_on) return;
    os_mutex_pend(&mutex, 0);
    if (!connect_pending && !associating()) { os_mutex_post(&mutex); return; }
    uint8_t peer[6];
    memcpy(peer, security_peer, 6);
    int secure = peer_security(connect_pending, 1, peer);
    uint16_t reply = 0;
    if (secure < 0 || memcmp(peer, security_peer, 6)) {
        record_result(BADGE_OPP_ERR_PAIRING, 0, "pair_link");
        forget_channel();
        wait_for_file();
    } else if (secure && status.state != BADGE_OPP_PAIRING) {
        wait_for_file();
        if (connect_pending) {
            connect_pending = 0;
            queue_response(1);
            reply = rfcomm_cid;
        }
    }
    os_mutex_post(&mutex);
    if (reply) flush_reply(reply);
}

static void rfcomm_packet(uint8_t type, uint16_t mux, uint8_t *p, uint16_t n)
{
    if (type == 4) { rfcomm_event(mux, p, n); return; }
    if (type != 7) return; /* RFCOMM_DATA_PACKET */
    while (n) {
        uint8_t peer[6];
        int authenticate = 0;
        os_mutex_pend(&mutex, 0);
        uint16_t channel = rfcomm_cid;
        if (!channel || mux != l2cap_cid || !radio_on) { os_mutex_post(&mutex); return; }
        if (pending_reply[0] || connect_pending) {
            os_mutex_post(&mutex);
            rfcomm_disconnect_internal(channel);
            return;
        }
        size_t need = used < 3 ? 3 : be16(packet + 1);
        size_t count = need - used < n ? need - used : n;
        memcpy(packet + used, p, count);
        used += count; p += count; n -= count;
        last_activity = sys_timer_get_ms();
        if (used >= 3 && (be16(packet + 1) < 3 || be16(packet + 1) > sizeof(packet))) {
            used = 0;
            fail(BADGE_MEDIA_ERR_SIZE, 0, "packet_length");
            os_mutex_post(&mutex);
            rfcomm_disconnect_internal(channel);
            return;
        }
        if (used >= 3 && used == be16(packet + 1)) {
            uint8_t op = packet[0];
            int secure = (op == DISCONNECT || op == ABORT_PUT) ? 1 : peer_security(1, op == CONNECT, peer);
            if (op == CONNECT && !connected && secure >= 0 && used >= 7 &&
                (packet[3] >> 4) == 1 && be16(packet + 5) >= 255 &&
                (status.state == BADGE_OPP_WAITING || (associating() && !memcmp(peer, security_peer, 6)))) {
                if (!secure || status.state == BADGE_OPP_PAIRING) {
                    connect_pending = 1;
                    if (status.state == BADGE_OPP_WAITING) {
                        memcpy(security_peer, peer, 6);
                        authenticate = 1;
                    }
                } else if (status.state == BADGE_OPP_SECURING) {
                    wait_for_file();
                }
            }
            if (!connect_pending) queue_response(secure > 0);
        }
        os_mutex_post(&mutex);
        if (authenticate) lmp_hci_authentication_requested(peer);
        /* No Continue or Success is issued until the import has consumed the bytes. */
        flush_reply(channel);
    }
}

void badge_opp_bt_ready(void)
{
    if (ready || os_mutex_create(&mutex)) return;
    stack_run_loop_register(&security_loop, check_security, NULL);
    rfcomm_register_service_internal(NULL, rfcomm_packet, OPP_CHANNEL, 512);
    ready = 1;
}

const char *badge_opp_name(void) { return bt_get_local_name(); }
int badge_opp_classic_enabled(void) { return radio_on; }

void badge_opp_start(void)
{
    if (!ready) {
        status = (struct badge_opp_status){.state=BADGE_OPP_FAILED, .error=BADGE_OPP_ERR_RADIO, .slot=-1};
        record_result(BADGE_OPP_ERR_RADIO, 0, "radio_start");
        return;
    }
    os_mutex_pend(&mutex, 0);
    if (radio_on) { os_mutex_post(&mutex); return; }
    forget_channel();
    status = (struct badge_opp_status){.state = BADGE_OPP_WAITING, .slot = -1};
    last_activity = sys_timer_get_ms();
    radio_on = 1;
    os_mutex_post(&mutex);
    if (badge_opp_set_bredr(1)) {
        os_mutex_pend(&mutex, 0);
        radio_on = 0;
        fail(BADGE_OPP_ERR_RADIO, 0, "radio_start");
        os_mutex_post(&mutex);
    }
}

void badge_opp_pair_request(const uint8_t address[6], uint32_t number)
{
    uint8_t peer[6];
    memcpy(peer, address, 6);
    if (ready) {
        os_mutex_pend(&mutex, 0);
        int available = radio_on && status.state == BADGE_OPP_WAITING && !connected;
        if (available && acl_open) {
            uint8_t channel_peer[6];
            available = peer_security(1, 0, channel_peer) >= 0 && !memcmp(channel_peer, peer, 6);
        }
        if (number <= 999999 && (available || (associating() && !memcmp(security_peer, peer, 6)))) {
            memcpy(security_peer, peer, sizeof(peer));
            status.number = number;
            status.state = BADGE_OPP_PAIRING;
            last_activity = sys_timer_get_ms();
            os_mutex_post(&mutex);
            return;
        }
        os_mutex_post(&mutex);
    }
    lmp_hci_user_confirmation_request_negative_reply(peer);
}

void badge_opp_confirm(uint32_t shown_number)
{
    uint8_t peer[6];
    if (!ready) return;
    os_mutex_pend(&mutex, 0);
    if (status.state != BADGE_OPP_PAIRING || status.number != shown_number) { os_mutex_post(&mutex); return; }
    memcpy(peer, security_peer, sizeof(peer));
    status.state = BADGE_OPP_SECURING;
    last_activity = sys_timer_get_ms();
    os_mutex_post(&mutex);
    lmp_hci_user_confirmation_request_reply(peer);
}

static void close_state(void)
{
    forget_channel();
    radio_on = 0;
}

void badge_opp_stop(void)
{
    if (!ready) return;
    os_mutex_pend(&mutex, 0);
    int close = radio_on;
    uint8_t reject_peer[6];
    int reject = status.state == BADGE_OPP_PAIRING;
    if (reject) memcpy(reject_peer, security_peer, 6);
    if (associating() || connect_pending) record_result(BADGE_OPP_ERR_PAIRING, 0, "pair_cancel");
    else if (status.state == BADGE_OPP_RECEIVING && status.slot < 0)
        record_result(BADGE_OPP_ERR_DISCONNECTED, 0, "cancel");
    if (close) close_state();
    status.state = BADGE_OPP_OFF;
    os_mutex_post(&mutex);
    if (reject) lmp_hci_user_confirmation_request_negative_reply(reject_peer);
    if (close) badge_opp_set_bredr(0);
}

struct badge_opp_status badge_opp_poll(void)
{
    if (!ready) return status;
    os_mutex_pend(&mutex, 0);
    uint32_t elapsed = sys_timer_get_ms() - last_activity;
    if (radio_on && commit_acknowledged && elapsed >= 1000) status.state = BADGE_OPP_DONE;
    int close = 0;
    if (radio_on && (status.state == BADGE_OPP_DONE || status.state == BADGE_OPP_FAILED) && !sending && !pending_reply[0]) {
        close = 1;
    } else if (radio_on && !associating() && elapsed >= (status.state == BADGE_OPP_WAITING ? 120000u : 30000u)) {
        close = 1;
        if (status.slot >= 0) status.state = BADGE_OPP_DONE;
        else fail(BADGE_OPP_ERR_TIMEOUT, 0, "timeout");
    }
    if (close) close_state();
    struct badge_opp_status snapshot = status;
    int wake_stack = connect_pending || associating();
    os_mutex_post(&mutex);
    if (close) badge_opp_set_bredr(0);
    else if (wake_stack) stack_run_loop_resume();
    return snapshot;
}
