#ifndef LCBD_NLR_H
#define LCBD_NLR_H

#include <stdbool.h>
#include <stdint.h>

#define NLR_DNC_MAX 4096                   // DNC message buffer, longer messages are acknowledged but truncated
#define NLR_CONTINUOUS_SCAN_ANGLE_X10 1500 // set angle 150.0 degrees: the CNC asks for a continuous scan
#define NLR_TX_MAX 16
#define NLR_BLOCK_MAX 8

// Bend phase, the operand of an angle request, phases 8 to 15 keep the raw value
#define NLR_PHASE_IDLE 0          // the angle only during a continuous scan, the controller executes commands
#define NLR_PHASE_MEASURE_FIRST 1 // the angle once both legs were valid
#define NLR_PHASE_CHECKED_FIRST 2 // phases 2 to 5 answer a missing reference or an invalid leg instead of the angle
#define NLR_PHASE_CHECKED_LAST 5
#define NLR_PHASE_MEASURE_LAST 6
#define NLR_PHASE_END 7 // end of the measurement, stops a scan

typedef enum { NLR_COMMAND, NLR_PARAMETER, NLR_DNC_HEADER, NLR_DNC_DATA } NlrParserState;

typedef struct {
    uint8_t tx[NLR_TX_MAX];
    int tx_len;
    bool ack; // toggle DTR once
} NlrReply;

typedef struct {
    float angle;
    bool both, valid_rear, valid_front;
    bool referenced, encoder_used, leg_check;

    int phase; // of the last angle request, 0 to 15
    // param20, param28: raw values of 180 and 90 degrees, raw_angle: the 12-bit value of the last angle reply
    uint16_t param20, param28, param30, set_angle_x10, raw_angle;

    NlrParserState state;
    int block, pos;             // parameter block / byte position being received
    uint8_t buf[NLR_BLOCK_MAX]; // the parameter block or DNC header received so far
    uint16_t dnc_words;
    uint32_t dnc_received; // data bytes received of the current DNC block

    uint8_t dnc[NLR_DNC_MAX];
    int dnc_len;
    bool dnc_ready;
    unsigned dnc_count;

    uint8_t last_rx, last_tx, last_cmd;
    int last_group;
    unsigned long rx_count;
    unsigned error_count, springback_query_count;
} NlrLink;

void nlr_init(NlrLink *link);
void nlr_receive(NlrLink *link, uint8_t byte, NlrReply *reply);
int nlr_take_dnc(NlrLink *link, uint8_t buf[NLR_DNC_MAX]);
// 12-bit raw value of an angle in degrees: param20 is the raw value of 180 degrees, param28 the one of 90 degrees
uint16_t nlr_scale(float angle, uint16_t param20, uint16_t param28);
const char *nlr_parser_state_name(NlrParserState state);

typedef enum { LINE_OK, LINE_DOWN_OFF, LINE_DOWN_ON } LineState;
typedef struct { // zero-initialised = LINE_OK
    LineState state;
    uint32_t since_ms;
} LineSupervision;

// Called about every millisecond
bool line_supervision_step(LineSupervision *sup, bool cts, bool dsr, uint32_t now_ms, bool *changed);

#endif
