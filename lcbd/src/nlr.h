// CNC link protocol (NLR slave): byte parser, angle replies, parameter and
// DNC blocks, angle scaling and the line supervision timing
#ifndef LCBD_NLR_H
#define LCBD_NLR_H

#include <stdbool.h>
#include <stdint.h>

#define NLR_DNC_MAX 4096 // DNC message buffer. longer messages are acknowledged but truncated

typedef enum { NLR_CMD, NLR_PARAM, NLR_DNC_HEAD, NLR_DNC_DATA } NlrParserState;

typedef struct {
    uint8_t tx[16];
    int ntx;
    bool ack; // toggle DTR once
} NlrReply;

typedef struct {
    // inputs: station result and controller flags
    float angle;
    bool both, valid_rear, valid_front;
    bool referenced, encoder_used, leg_check;

    // link state
    uint8_t status[6];
    int phase;
    uint16_t p20, p28, p30, set_angle10, raw;

    // parser
    NlrParserState state;
    int block, pos; // parameter block / byte position being received
    uint8_t buf[8];
    uint16_t dnc_words;
    uint32_t dnc_len; // data bytes received of the current DNC block

    // last complete DNC message
    uint8_t dnc[NLR_DNC_MAX];
    int dnc_size;
    bool dnc_ready;
    unsigned dnc_messages;

    // display counters
    uint8_t last_rx, last_tx, last_cmd;
    int last_group;
    unsigned long rx_count;
    unsigned errors, springback_queries;
} NlrLink;

void nlr_init(NlrLink *l);
void nlr_receive(NlrLink *l, uint8_t byte, NlrReply *rep);
int nlr_take_dnc(NlrLink *l, uint8_t *buf, int cap);
uint16_t nlr_scale(float angle, uint16_t p20, uint16_t p28);

typedef enum { LS_OK, LS_DOWN_OFF, LS_DOWN_ON } LineSupState;
typedef struct { // zero-initialised = LS_OK
    LineSupState state;
    uint32_t since_ms;
} LineSup;

bool linesup_step(LineSup *s, bool cts, bool dsr, uint32_t now_ms, bool *changed);

#endif
