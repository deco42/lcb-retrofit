#include "sensorproto.h"

#include <string.h>

#define REG_SHUTTER 0x00
#define REG_GAIN 0x06
#define REG_LIGHT_FILTER 0x0B
#define REG_FIXED 0x15
#define START_STREAMING 0x1C
#define CONNECT_SHUTTER 1022
#define CONNECT_GAIN 400

static size_t reg(uint8_t *o, size_t n, int r, int value)
{
    o[n++] = (uint8_t)(r & 0x7F);
    o[n++] = (uint8_t)(0x80 | (value & 0x7F));
    return n;
}

static size_t fixed_exposure(uint8_t *o, size_t n, int shutter, int gain)
{
    n = reg(o, n, REG_FIXED, 1);
    n = reg(o, n, REG_SHUTTER, shutter & 0x7F);
    n = reg(o, n, REG_SHUTTER + 1, (shutter >> 7) & 7);
    n = reg(o, n, REG_GAIN, gain & 0x7F);
    n = reg(o, n, REG_GAIN + 1, (gain >> 7) & 7);
    return n;
}

static size_t scan_mode(const SensorRegs *r, uint8_t *o, size_t n)
{
    return r->internal_ctrl ? fixed_exposure(o, n, r->shutter, r->gain) : reg(o, n, REG_FIXED, 0);
}

size_t sensor_command(const SensorRegs *r, SensorEvent ev, uint8_t out[SENSOR_CMD_MAX])
{
    size_t n = 0;
    switch (ev) {
    case SE_CONNECTED:
        n = fixed_exposure(out, n, CONNECT_SHUTTER, CONNECT_GAIN);
        out[n++] = START_STREAMING;
        break;
    case SE_START_SCAN:
        n = scan_mode(r, out, n);
        break;
    case SE_STOP_SCAN:
        if (r->light_filter) {
            n = reg(out, n, REG_LIGHT_FILTER, 0);
        }

        n = fixed_exposure(out, n, CONNECT_SHUTTER, CONNECT_GAIN);
        break;
    case SE_START_REFERENCE:
        if (r->light_filter) {
            n = reg(out, n, REG_LIGHT_FILTER, 1);
        }

        n = scan_mode(r, out, n);
        break;
    }

    return n;
}

void frame_assembler_reset(FrameAssembler *a)
{
    a->fill = 0;
    a->invalid_run = 0;
}

static bool frame_is_valid(const uint8_t frame[FRAME_SIZE])
{
    for (int i = 0x34; i <= 0x3B; i++) {
        if (frame[i]) {
            return false;
        }
    }

    return true;
}

FrameStatus frame_assemble(FrameAssembler *a, const uint8_t **data, size_t *len)
{
    size_t take = FRAME_SIZE - a->fill;
    if (take > *len) {
        take = *len;
    }

    memcpy(a->frame + a->fill, *data, take);
    a->fill += take;
    *data += take;
    *len -= take;

    if (a->fill < FRAME_SIZE) {
        return FRAME_NONE;
    }

    a->fill = 0;

    if (frame_is_valid(a->frame)) {
        a->invalid_run = 0;
        return FRAME_VALID;
    }

    if (++a->invalid_run >= FRAME_RESYNC_INVALID) {
        a->invalid_run = 0;
        return FRAME_RESYNC;
    }

    return FRAME_INVALID;
}
