#include "nlr.h"

#include <string.h>

#include "bytes.h"
#include "x87.h"

static void put(NlrReply *reply, uint8_t byte)
{
    if (reply->tx_len < (int)sizeof(reply->tx)) {
        reply->tx[reply->tx_len++] = byte;
    }
}

#define ANSWER_ANGLE 0x10 // | bits 8 to 11 of the raw value, then its low byte
#define ANSWER_LEG_INVALID 0x81
#define ANSWER_NOT_REFERENCED 0x85

static void angle_reply(NlrLink *link, NlrReply *reply)
{
    uint16_t scaled = nlr_scale(link->angle, link->param20, link->param28);
    int phase = link->phase;
    if (phase == NLR_PHASE_IDLE) {
        link->raw_angle = link->set_angle_x10 == NLR_CONTINUOUS_SCAN_ANGLE_X10 ? scaled : link->param20;
    } else if (phase == NLR_PHASE_MEASURE_FIRST) {
        link->raw_angle = link->both ? scaled : link->param20;
    } else if (phase <= NLR_PHASE_MEASURE_LAST) {
        link->raw_angle = scaled;
    } else if (phase == NLR_PHASE_END) {
        link->raw_angle = link->param20;
    }

    if (phase >= NLR_PHASE_CHECKED_FIRST && phase <= NLR_PHASE_CHECKED_LAST && link->encoder_used) {
        if (!link->referenced) {
            put(reply, ANSWER_NOT_REFERENCED);
            link->error_count++;
            return;
        }

        if (link->leg_check && (!link->valid_rear || !link->valid_front)) {
            put(reply, ANSWER_LEG_INVALID);
            link->error_count++;
            return;
        }
    }

    put(reply, (uint8_t)(ANSWER_ANGLE | (link->raw_angle >> 8)));
    put(reply, (uint8_t)(link->raw_angle & 0xFF));
}

// A command byte has bit 7 clear, the group in bits 4 to 6 and the operand in bits 0 to 3
typedef enum {
    COMMAND_NONE = -1, // bit 7 set
    COMMAND_STATUS,    // answered with the status bytes
    COMMAND_ANGLE,     // angle request, the operand is the phase
    COMMAND_PARAMETER, // the parameter block with the operand as number follows
    COMMAND_DNC,       // a DNC block follows
    COMMAND_SPRINGBACK,
} CommandGroup;

#define CNC_ERROR_REPORT 0xFF // the only byte with bit 7 set that gets ANSWER_ERROR_REPORT
#define ANSWER_SPRINGBACK 0x40
#define ANSWER_ERROR_REPORT 0x80
#define ANSWER_UNKNOWN 0xFF

// The answer byte, six status bytes (only the first is set) and the end mark
static const uint8_t STATUS_REPLY[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00};

static void handle_command(NlrLink *link, uint8_t cmd, NlrReply *reply)
{
    link->last_cmd = cmd;
    if (cmd & 0x80) {
        link->last_group = COMMAND_NONE;
        put(reply, cmd == CNC_ERROR_REPORT ? ANSWER_ERROR_REPORT : ANSWER_UNKNOWN);
        return;
    }

    int group = (cmd >> 4) & 7, operand = cmd & 0x0F;
    link->last_group = group;
    switch (group) {
    case COMMAND_STATUS:
        for (size_t i = 0; i < sizeof(STATUS_REPLY); i++) {
            put(reply, STATUS_REPLY[i]);
        }
        break;
    case COMMAND_ANGLE:
        reply->ack = false; // the answer itself is the acknowledge
        link->phase = operand;
        angle_reply(link, reply);
        break;
    case COMMAND_PARAMETER:
        link->state = NLR_PARAMETER;
        link->block = operand;
        link->pos = 0;
        break;
    case COMMAND_DNC:
        link->state = NLR_DNC_HEADER;
        link->pos = 0;
        break;
    case COMMAND_SPRINGBACK:
        put(reply, ANSWER_SPRINGBACK);
        link->springback_query_count++;
        break;
    default:
        put(reply, ANSWER_UNKNOWN);
        break;
    }
}

typedef enum { BLOCK_IGNORED = 0, BLOCK_SCALING = 5, BLOCK_SET_ANGLE = 6 } ParameterBlock;

// Data bytes of the parameter blocks by number (0 to 15), 0 for an unknown block
static const int BLOCK_LEN[16] = {[BLOCK_IGNORED] = 8, [BLOCK_SCALING] = 6, [BLOCK_SET_ANGLE] = 4};

#define ANSWER_BLOCK 0x20 // | number of the parameter block received

static void parameter_byte(NlrLink *link, uint8_t byte, NlrReply *reply)
{
    int len = BLOCK_LEN[link->block];
    if (len == 0) {
        put(reply, ANSWER_UNKNOWN);
        link->state = NLR_COMMAND;
        return;
    }

    link->buf[link->pos++] = byte;

    if (link->pos < len) {
        return;
    }

    const uint8_t *data = link->buf;
    if (link->block == BLOCK_SCALING) {
        link->param20 = le16(data);
        link->param28 = le16(data + 2);
        link->param30 = le16(data + 4);
    } else if (link->block == BLOCK_SET_ANGLE) {
        link->set_angle_x10 = le16(data);
    }

    put(reply, (uint8_t)(ANSWER_BLOCK | link->block));
    link->state = NLR_COMMAND;
}

// The DNC block header: count, type and number of data words, each 16 bit
typedef enum { DNC_COUNT = 0, DNC_TYPE = 2, DNC_WORDS = 4, DNC_HEADER_LEN = 6 } DncHeaderField;

#define DNC_TYPE_LOW '6' // low byte of the only type that is accepted

static void dnc_header_byte(NlrLink *link, uint8_t byte)
{
    link->buf[link->pos++] = byte;
    const uint8_t *header = link->buf;

    if (link->pos == DNC_TYPE) { // count received
        if (le16(header + DNC_COUNT) == 0) {
            link->state = NLR_COMMAND;
        }
    } else if (link->pos == DNC_WORDS) { // type received
        if (header[DNC_TYPE] != DNC_TYPE_LOW) {
            link->state = NLR_COMMAND;
        }
    } else if (link->pos == DNC_HEADER_LEN) {
        link->dnc_words = le16(header + DNC_WORDS);
        link->dnc_received = 0;
        link->state = link->dnc_words == 0 ? NLR_COMMAND : NLR_DNC_DATA;
        link->dnc_len = 0;
        link->dnc_ready = false;
    }
}

// The answer is a DNC block of its own: DNC command, count 5, type '7', 3 words: "OK " and the number of data bytes
// received as three digits
static const uint8_t DNC_ANSWER[] = {0x30, 0x05, 0x00, '7', 0x00, 0x03, 0x00, 'O', 'K', ' '};

static void dnc_data_byte(NlrLink *link, uint8_t byte, NlrReply *reply)
{
    if (link->dnc_len < NLR_DNC_MAX) {
        link->dnc[link->dnc_len++] = byte;
    }

    link->dnc_received++;
    if (link->dnc_received < 2u * link->dnc_words) {
        return;
    }

    for (size_t i = 0; i < sizeof(DNC_ANSWER); i++) {
        put(reply, DNC_ANSWER[i]);
    }

    put(reply, (uint8_t)('0' + link->dnc_received / 100 % 10));
    put(reply, (uint8_t)('0' + link->dnc_received / 10 % 10));
    put(reply, (uint8_t)('0' + link->dnc_received % 10));
    link->dnc_ready = true;
    link->dnc_count++;
    link->state = NLR_COMMAND;
}

#define INITIAL_RAW_ANGLE 0x104

void nlr_init(NlrLink *link)
{
    memset(link, 0, sizeof(*link));
    link->raw_angle = INITIAL_RAW_ANGLE;
    link->both = link->valid_rear = link->valid_front = true;
    link->referenced = link->encoder_used = true;
}

void nlr_receive(NlrLink *link, uint8_t byte, NlrReply *reply)
{
    reply->tx_len = 0;
    reply->ack = true;
    link->last_rx = byte;
    link->rx_count++;

    switch (link->state) {
    case NLR_COMMAND:
        handle_command(link, byte, reply);
        break;
    case NLR_PARAMETER:
        parameter_byte(link, byte, reply);
        break;
    case NLR_DNC_HEADER:
        dnc_header_byte(link, byte);
        break;
    case NLR_DNC_DATA:
        dnc_data_byte(link, byte, reply);
        break;
    }

    if (reply->tx_len) {
        link->last_tx = reply->tx[reply->tx_len - 1];
    }
}

int nlr_take_dnc(NlrLink *link, uint8_t buf[NLR_DNC_MAX])
{
    if (!link->dnc_ready) {
        return 0;
    }

    memcpy(buf, link->dnc, (size_t)link->dnc_len);
    link->dnc_ready = false;

    return link->dnc_len;
}

#define PARAM20_ANGLE 180
#define PARAM28_ANGLE 90
#define RAW_ANGLE_MASK 0xFFF

uint16_t nlr_scale(float angle, uint16_t param20, uint16_t param28)
{
    const ext per_degree = (ext)(1.0 / (PARAM20_ANGLE - PARAM28_ANGLE)); // IEEE double nearest to 1/90
    ext raw = ((ext)PARAM20_ANGLE - angle) * (ext)((int)param28 - (int)param20) * per_degree + param20;
    return (uint16_t)((long long)raw & RAW_ANGLE_MASK);
}

const char *nlr_parser_state_name(NlrParserState state)
{
    static const char *const names[] = {"command", "parameter block", "DNC header", "DNC data"};
    return names[state];
}

#define LINE_CYCLE_HALF_MS 1100 // while CTS and DSR are inactive, DTR and RTS are off and on for this long each

bool line_supervision_step(LineSupervision *sup, bool cts, bool dsr, uint32_t now_ms, bool *changed)
{
    LineState before = sup->state;

    switch (sup->state) {
    case LINE_OK:
        if (!cts && !dsr) {
            sup->state = LINE_DOWN_OFF;
            sup->since_ms = now_ms;
        }
        break;
    case LINE_DOWN_OFF:
        if (now_ms - sup->since_ms >= LINE_CYCLE_HALF_MS) {
            sup->state = LINE_DOWN_ON;
            sup->since_ms = now_ms;
        }
        break;
    case LINE_DOWN_ON:
        if (now_ms - sup->since_ms >= LINE_CYCLE_HALF_MS) {
            if (!cts && !dsr) {
                sup->state = LINE_DOWN_OFF;
                sup->since_ms = now_ms;
            } else {
                sup->state = LINE_OK;
            }
        }
        break;
    }

    *changed = sup->state != before && (before == LINE_OK || sup->state == LINE_OK);

    return sup->state != LINE_DOWN_OFF;
}
