#include "nlr.h"

#include <string.h>

#include "x87.h"

#define LINESUP_HALF_MS 1100

static const uint8_t DNC_ACK[] = {0x30, 0x05, 0x00, 0x37, 0x00, 0x03, 0x00, 0x4F, 0x4B, 0x20};

void nlr_init(NlrLink *l)
{
    memset(l, 0, sizeof(*l));
    l->status[0] = 0x01;
    l->raw = 0x104;
    l->both = l->valid_rear = l->valid_front = true;
    l->referenced = l->encoder_used = true;
}

uint16_t nlr_scale(float angle, uint16_t p20, uint16_t p28)
{
    const ext k = (ext)(1.0 / 90.0); // IEEE double nearest to 1/90
    ext v = ((ext)180 - angle) * (ext)((int)p28 - (int)p20) * k + p20;
    return (uint16_t)((long long)v & 0xFFF);
}

static void put(NlrReply *rep, uint8_t b)
{
    if (rep->ntx < (int)sizeof(rep->tx)) {
        rep->tx[rep->ntx++] = b;
    }
}

static void angle_reply(NlrLink *l, NlrReply *rep)
{
    uint16_t scaled = nlr_scale(l->angle, l->p20, l->p28);
    int p = l->phase;
    if (p == 0) {
        l->raw = l->set_angle10 == 1500 ? scaled : l->p20;
    } else if (p == 1) {
        l->raw = l->both ? scaled : l->p20;
    } else if (p <= 6) {
        l->raw = scaled;
    } else if (p == 7) {
        l->raw = l->p20;
    }

    if (p >= 2 && p <= 5 && l->encoder_used) {
        if (!l->referenced) {
            put(rep, 0x85);
            l->errors++;
            return;
        }
        if (l->leg_check && (!l->valid_rear || !l->valid_front)) {
            put(rep, 0x81);
            l->errors++;
            return;
        }
    }

    put(rep, (uint8_t)(0x10 | (l->raw >> 8)));
    put(rep, (uint8_t)(l->raw & 0xFF));
}

static void command(NlrLink *l, uint8_t c, NlrReply *rep)
{
    l->last_cmd = c;
    if (c & 0x80) {
        l->last_group = -1;
        put(rep, c == 0xFF ? 0x80 : 0xFF);
        return;
    }

    int g = (c >> 4) & 7, n = c & 0x0F;
    l->last_group = g;
    switch (g) {
    case 0:
        put(rep, 0x00);
        for (int i = 0; i < 6; i++) {
            put(rep, l->status[i]);
        }
        put(rep, 0x34);
        put(rep, 0x00);
        break;
    case 1:
        rep->ack = false;
        l->phase = n;
        angle_reply(l, rep);
        break;
    case 2:
        l->state = NLR_PARAM;
        l->block = n;
        l->pos = 0;
        break;
    case 3:
        l->state = NLR_DNC_HEAD;
        l->pos = 0;
        break;
    case 4:
        put(rep, 0x40);
        l->springback_queries++;
        break;
    default:
        put(rep, 0xFF);
        break;
    }
}

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

static void param_byte(NlrLink *l, uint8_t c, NlrReply *rep)
{
    static const int len[16] = {[0] = 8, [5] = 6, [6] = 4};
    int need = len[l->block];
    if (need == 0) {
        put(rep, 0xFF);
        l->state = NLR_CMD;
        return;
    }

    l->buf[l->pos++] = c;

    if (l->pos < need) {
        return;
    }

    const uint8_t *b = l->buf;
    if (l->block == 5) {
        l->p20 = le16(b);
        l->p28 = le16(b + 2);
        l->p30 = le16(b + 4);
    } else if (l->block == 6) {
        l->set_angle10 = le16(b);
    }

    put(rep, (uint8_t)(0x20 | l->block)); // block 0 is acknowledged only
    l->state = NLR_CMD;
}

static void dnc_head_byte(NlrLink *l, uint8_t c)
{
    l->buf[l->pos++] = c;

    if (l->pos == 2) {
        if (le16(l->buf) == 0) { // count
            l->state = NLR_CMD;
        }
    } else if (l->pos == 4) {
        if (l->buf[2] != '6') { // low byte of the type
            l->state = NLR_CMD;
        }
    } else if (l->pos == 6) {
        l->dnc_words = le16(l->buf + 4);
        l->dnc_len = 0;
        l->state = l->dnc_words == 0 ? NLR_CMD : NLR_DNC_DATA;
        l->dnc_size = 0;
        l->dnc_ready = false;
    }
}

static void dnc_data_byte(NlrLink *l, uint8_t c, NlrReply *rep)
{
    if (l->dnc_size < NLR_DNC_MAX) {
        l->dnc[l->dnc_size++] = c;
    }

    l->dnc_len++;
    if (l->dnc_len < 2u * l->dnc_words) {
        return;
    }

    for (size_t i = 0; i < sizeof(DNC_ACK); i++) {
        put(rep, DNC_ACK[i]);
    }

    put(rep, (uint8_t)('0' + l->dnc_len / 100 % 10));
    put(rep, (uint8_t)('0' + l->dnc_len / 10 % 10));
    put(rep, (uint8_t)('0' + l->dnc_len % 10));
    l->dnc_ready = true;
    l->dnc_messages++;
    l->state = NLR_CMD;
}

void nlr_receive(NlrLink *l, uint8_t byte, NlrReply *rep)
{
    rep->ntx = 0;
    rep->ack = true;
    l->last_rx = byte;
    l->rx_count++;

    switch (l->state) {
    case NLR_CMD:
        command(l, byte, rep);
        break;
    case NLR_PARAM:
        param_byte(l, byte, rep);
        break;
    case NLR_DNC_HEAD:
        dnc_head_byte(l, byte);
        break;
    case NLR_DNC_DATA:
        dnc_data_byte(l, byte, rep);
        break;
    }

    if (rep->ntx) {
        l->last_tx = rep->tx[rep->ntx - 1];
    }
}

int nlr_take_dnc(NlrLink *l, uint8_t *buf, int cap)
{
    if (!l->dnc_ready) {
        return 0;
    }

    int n = l->dnc_size < cap ? l->dnc_size : cap;
    memcpy(buf, l->dnc, (size_t)n);
    l->dnc_ready = false;

    return n;
}

bool linesup_step(LineSup *s, bool cts, bool dsr, uint32_t now_ms, bool *changed)
{
    LineSupState old = s->state;

    switch (s->state) {
    case LS_OK:
        if (!cts && !dsr) {
            s->state = LS_DOWN_OFF;
            s->since_ms = now_ms;
        }
        break;
    case LS_DOWN_OFF:
        if (now_ms - s->since_ms >= LINESUP_HALF_MS) {
            s->state = LS_DOWN_ON;
            s->since_ms = now_ms;
        }
        break;
    case LS_DOWN_ON:
        if (now_ms - s->since_ms >= LINESUP_HALF_MS) {
            if (!cts && !dsr) {
                s->state = LS_DOWN_OFF;
                s->since_ms = now_ms;
            } else {
                s->state = LS_OK;
            }
        }
        break;
    }

    *changed = s->state != old && (old == LS_OK || s->state == LS_OK);

    return s->state != LS_DOWN_OFF;
}
