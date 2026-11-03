#include "sensorproto.h"

#include <string.h>

#define IDLE_SHUTTER 1022 // fixed exposure while no scan runs
#define IDLE_GAIN 400

static size_t put_register(uint8_t *cmd, size_t len, int number, int value)
{
    cmd[len++] = (uint8_t)(number & 0x7F);
    cmd[len++] = (uint8_t)(0x80 | (value & 0x7F));
    return len;
}

static size_t put_fixed_exposure(uint8_t *cmd, size_t len, int shutter, int gain)
{
    len = put_register(cmd, len, SENSOR_REG_FIXED, 1);
    len = put_register(cmd, len, SENSOR_REG_SHUTTER, shutter);
    len = put_register(cmd, len, SENSOR_REG_SHUTTER + 1, (shutter >> 7) & 7);
    len = put_register(cmd, len, SENSOR_REG_GAIN, gain);
    return put_register(cmd, len, SENSOR_REG_GAIN + 1, (gain >> 7) & 7);
}

static size_t put_scan_mode(const SensorRegs *regs, uint8_t *cmd, size_t len)
{
    if (regs->internal_control) {
        return put_fixed_exposure(cmd, len, regs->shutter, regs->gain);
    }

    return put_register(cmd, len, SENSOR_REG_FIXED, 0);
}

size_t sensor_command(const SensorRegs *regs, SensorEvent event, uint8_t cmd[SENSOR_CMD_MAX])
{
    size_t len = 0;
    switch (event) {
    case SENSOR_CONNECTED:
        len = put_fixed_exposure(cmd, len, IDLE_SHUTTER, IDLE_GAIN);
        cmd[len++] = SENSOR_CMD_START_STREAMING;
        break;
    case SENSOR_START_SCAN:
        len = put_scan_mode(regs, cmd, len);
        break;
    case SENSOR_STOP_SCAN:
        if (regs->light_filter) {
            len = put_register(cmd, len, SENSOR_REG_LIGHT_FILTER, 0);
        }

        len = put_fixed_exposure(cmd, len, IDLE_SHUTTER, IDLE_GAIN);
        break;
    case SENSOR_START_REFERENCE:
        if (regs->light_filter) {
            len = put_register(cmd, len, SENSOR_REG_LIGHT_FILTER, 1);
        }

        len = put_scan_mode(regs, cmd, len);
        break;
    }

    return len;
}

void frame_assembler_reset(FrameAssembler *fa)
{
    fa->frame_len = 0;
    fa->invalid_run = 0;
}

static bool frame_is_valid(const uint8_t frame[FRAME_SIZE])
{
    for (int i = FRAME_ZERO_FIRST; i <= FRAME_ZERO_LAST; i++) {
        if (frame[i]) {
            return false;
        }
    }

    return true;
}

FrameStatus frame_assemble(FrameAssembler *fa, const uint8_t **data, size_t *len)
{
    size_t take = FRAME_SIZE - fa->frame_len;
    if (take > *len) {
        take = *len;
    }

    memcpy(fa->frame + fa->frame_len, *data, take);
    fa->frame_len += take;
    *data += take;
    *len -= take;

    if (fa->frame_len < FRAME_SIZE) {
        return FRAME_NONE;
    }

    fa->frame_len = 0;

    if (frame_is_valid(fa->frame)) {
        fa->invalid_run = 0;
        return FRAME_VALID;
    }

    if (++fa->invalid_run >= FRAME_RESYNC_INVALID) {
        fa->invalid_run = 0;
        return FRAME_RESYNC;
    }

    return FRAME_INVALID;
}
