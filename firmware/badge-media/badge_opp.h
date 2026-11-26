#ifndef BADGE_OPP_H
#define BADGE_OPP_H

#include <stdint.h>

enum badge_opp_state {
    BADGE_OPP_OFF, BADGE_OPP_WAITING, BADGE_OPP_PAIRING,
    BADGE_OPP_RECEIVING, BADGE_OPP_DONE, BADGE_OPP_FAILED,
    BADGE_OPP_SECURING
};

enum badge_opp_error {
    BADGE_OPP_ERR_TIMEOUT = -10, BADGE_OPP_ERR_DISCONNECTED = -11,
    BADGE_OPP_ERR_RADIO = -12, BADGE_OPP_ERR_PAIRING = -13
};

struct badge_opp_status {
    enum badge_opp_state state;
    uint32_t received;
    uint32_t number;
    int slot;
    int error;
};

const char *badge_opp_name(void);
struct badge_opp_diagnostic {
    int result;         /* 0 before a result, 1 for a stored item, otherwise a negative error. */
    uint8_t response;   /* OBEX response selected for the request, or 0 for a local event. */
    uint32_t bytes;     /* Body bytes accepted by the importer. */
    const char *stage;  /* Static ASCII label. */
};
struct badge_opp_diagnostic badge_opp_diagnostic(void);
void badge_opp_start(void);
void badge_opp_stop(void);
void badge_opp_confirm(uint32_t shown_number);
struct badge_opp_status badge_opp_poll(void);

/* Called by the Classic stack's application event handler. */
int badge_opp_classic_enabled(void);
void badge_opp_bt_ready(void);
void badge_opp_pair_request(const uint8_t address[6], uint32_t number);

#endif
