#include <string.h>

#include "check.h"
#include "config.h"
#include "sensorproto.h"

static bool command_is(const SensorRegs *regs, SensorEvent event, const uint8_t *expected, size_t len)
{
    uint8_t cmd[SENSOR_CMD_MAX];
    size_t cmd_len = sensor_command(regs, event, cmd);
    return cmd_len == len && memcmp(cmd, expected, len) == 0;
}

static void test_sensor_commands(void)
{
    static const uint8_t connect[] = {0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83, 0x1C};
    static const uint8_t scan_external[] = {0x15, 0x80};
    static const uint8_t reference_filter[] = {0x0B, 0x81, 0x15, 0x80};
    static const uint8_t stop_filter[] = {0x0B, 0x80, 0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83};
    static const uint8_t scan_internal[] = {0x15, 0x81, 0x00, 0xB6, 0x01, 0x82, 0x06, 0xCA, 0x07, 0x82};
    static const uint8_t stop_no_filter[] = {0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83};

    // This machine's sensor.dat: filter TRUE, internal control FALSE
    SensorFile sensor_file;
    sensor_file_defaults(&sensor_file);
    SensorRegs machine = sensor_file.regs[UNIT_REAR];
    CHECK(command_is(&machine, SENSOR_CONNECTED, connect, sizeof(connect)));
    CHECK(command_is(&machine, SENSOR_START_SCAN, scan_external, sizeof(scan_external)));
    CHECK(command_is(&machine, SENSOR_START_REFERENCE, reference_filter, sizeof(reference_filter)));
    CHECK(command_is(&machine, SENSOR_STOP_SCAN, stop_filter, sizeof(stop_filter)));

    // Other variant: filter FALSE, internal control TRUE, 310 / 330
    sensor_file_parse(&sensor_file, "USE_EXTERNAL_LIGHT_FILTER = FALSE\nUSE_INTERNAL_SENSOR_CTRL=TRUE\n");
    SensorRegs variant = sensor_file.regs[UNIT_FRONT];
    CHECK(command_is(&variant, SENSOR_CONNECTED, connect, sizeof(connect)));
    CHECK(command_is(&variant, SENSOR_START_SCAN, scan_internal, sizeof(scan_internal)));
    CHECK(command_is(&variant, SENSOR_START_REFERENCE, scan_internal, sizeof(scan_internal)));
    CHECK(command_is(&variant, SENSOR_STOP_SCAN, stop_no_filter, sizeof(stop_no_filter)));

    static const uint8_t fixed_1022_400[] = {0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83};
    SensorRegs fixed = {.light_filter = false, .internal_control = true, .shutter = 1022, .gain = 400};
    CHECK(command_is(&fixed, SENSOR_START_SCAN, fixed_1022_400, sizeof(fixed_1022_400)));
}

static void test_frame_assembler(void)
{
    static uint8_t stream[3 * FRAME_SIZE];
    FrameAssembler fa;
    frame_assembler_reset(&fa);
    stream[FRAME_SIZE + FRAME_ZERO_FIRST] = 1; // second frame invalid
    stream[2 * FRAME_SIZE + FRAME_POINTS_OFFSET] = 7;

    // TCP delivers a frame in pieces, typically 1460 + 588 bytes
    const uint8_t *data = stream;
    size_t len = 1460;
    CHECK(frame_assemble(&fa, &data, &len) == FRAME_NONE && len == 0);
    len = 588;
    CHECK(frame_assemble(&fa, &data, &len) == FRAME_VALID && len == 0);
    len = 2 * FRAME_SIZE;
    CHECK(frame_assemble(&fa, &data, &len) == FRAME_INVALID && len == FRAME_SIZE);
    CHECK(frame_assemble(&fa, &data, &len) == FRAME_VALID && fa.frame[FRAME_POINTS_OFFSET] == 7 && len == 0);

    uint8_t invalid[FRAME_SIZE];
    memset(invalid, 0, sizeof(invalid));
    invalid[FRAME_ZERO_LAST] = 0x55;
    FrameStatus status = FRAME_NONE;
    for (int i = 0; i < FRAME_RESYNC_INVALID; i++) {
        data = invalid;
        len = FRAME_SIZE;
        status = frame_assemble(&fa, &data, &len);
        if (i < FRAME_RESYNC_INVALID - 1) {
            CHECK(status == FRAME_INVALID);
        }
    }

    CHECK(status == FRAME_RESYNC);
}

int main(void)
{
    test_sensor_commands();
    test_frame_assembler();
    return check_report("sensorproto");
}
