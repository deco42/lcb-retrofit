#include <string.h>

#include "check.h"
#include "config.h"
#include "sensorproto.h"

static bool bytes(const SensorRegs *r, SensorEvent ev, const uint8_t *want, size_t n)
{
    uint8_t out[SENSOR_CMD_MAX];
    size_t got = sensor_command(r, ev, out);
    return got == n && !memcmp(out, want, n);
}

static void test_commands(void)
{
    static const uint8_t connect[] = {0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83, 0x1C};
    static const uint8_t scan_ext[] = {0x15, 0x80};
    static const uint8_t ref_filter[] = {0x0B, 0x81, 0x15, 0x80};
    static const uint8_t stop_filter[] = {0x0B, 0x80, 0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83};
    static const uint8_t scan_int[] = {0x15, 0x81, 0x00, 0xB6, 0x01, 0x82, 0x06, 0xCA, 0x07, 0x82};
    static const uint8_t stop_nofilter[] = {0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83};

    // this machines sensor.dat: filter TRUE, internal control FALSE
    SensorFile f;
    sensor_defaults(&f);
    SensorRegs m = f.regs[0];
    CHECK(bytes(&m, SE_CONNECTED, connect, sizeof connect));
    CHECK(bytes(&m, SE_START_SCAN, scan_ext, sizeof scan_ext));
    CHECK(bytes(&m, SE_START_REFERENCE, ref_filter, sizeof ref_filter));
    CHECK(bytes(&m, SE_STOP_SCAN, stop_filter, sizeof stop_filter));

    // other variant: filter FALSE, internal control TRUE, 310 / 330
    sensor_parse(&f, "USE_EXTERNAL_LIGHT_FILTER = FALSE\nUSE_INTERNAL_SENSOR_CTRL=TRUE\n");
    SensorRegs v = f.regs[1];
    CHECK(bytes(&v, SE_CONNECTED, connect, sizeof connect));
    CHECK(bytes(&v, SE_START_SCAN, scan_int, sizeof scan_int));
    CHECK(bytes(&v, SE_START_REFERENCE, scan_int, sizeof scan_int));
    CHECK(bytes(&v, SE_STOP_SCAN, stop_nofilter, sizeof stop_nofilter));

    static const uint8_t ex1022[] = {0x15, 0x81, 0x00, 0xFE, 0x01, 0x87, 0x06, 0x90, 0x07, 0x83};
    SensorRegs fixed = {false, true, 1022, 400};
    CHECK(bytes(&fixed, SE_START_SCAN, ex1022, sizeof ex1022));
}

static void test_assembler(void)
{
    static uint8_t stream[3 * FRAME_SIZE];
    FrameAssembler a;
    frame_assembler_reset(&a);
    memset(stream, 0, sizeof stream);
    stream[FRAME_SIZE + 0x34] = 1; // second frame invalid
    stream[2 * FRAME_SIZE + 0x42] = 7;
    const uint8_t *p = stream;
    size_t len = 1460;
    CHECK(frame_assemble(&a, &p, &len) == FRAME_NONE && len == 0);
    len = 588;
    CHECK(frame_assemble(&a, &p, &len) == FRAME_VALID && len == 0);
    len = 2 * FRAME_SIZE;
    CHECK(frame_assemble(&a, &p, &len) == FRAME_INVALID && len == FRAME_SIZE);
    CHECK(frame_assemble(&a, &p, &len) == FRAME_VALID && a.frame[0x42] == 7 && len == 0);

    uint8_t bad[FRAME_SIZE];
    memset(bad, 0, sizeof bad);
    bad[0x3B] = 0x55;
    FrameStatus st = FRAME_NONE;
    for (int i = 0; i < 10; i++) {
        p = bad;
        len = FRAME_SIZE;
        st = frame_assemble(&a, &p, &len);
        if (i < 9) {
            CHECK(st == FRAME_INVALID);
        }
    }
    CHECK(st == FRAME_RESYNC);
}

int main(void)
{
    test_commands();
    test_assembler();
    return check_report("sensorproto");
}
