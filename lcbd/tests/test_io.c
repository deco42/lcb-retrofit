#include "check.h"
#include "io.h"

#define LAMP_GRID_MS 10

static const uint16_t BOTH_AWAY = BIT(IN_REF_FRONT) | BIT(IN_REF_REAR);

static uint16_t cycle(Io *io, uint16_t inputs, uint32_t rear_raw, uint32_t now_ms)
{
    uint32_t raw[UNITS] = {[UNIT_REAR] = rear_raw};
    io_read_inputs(io, inputs, raw, now_ms);

    return io_write_outputs(io);
}

static void test_debounce(void)
{
    Io io;
    io_init(&io);
    cycle(&io, BOTH_AWAY, 0, 0);
    for (uint32_t now_ms = 1; now_ms <= 201; now_ms++) { // different since now_ms = 1
        cycle(&io, BOTH_AWAY | BIT(IN_BUTTON_REFERENCE), 0, now_ms);
    }

    CHECK(!io_take_rising(&io, IN_BUTTON_REFERENCE)); // exactly 200 ms is not "more than 200 ms"
    cycle(&io, BOTH_AWAY | BIT(IN_BUTTON_REFERENCE), 0, 202);
    CHECK(io_take_rising(&io, IN_BUTTON_REFERENCE) && !io_take_rising(&io, IN_BUTTON_REFERENCE));
    for (uint32_t now_ms = 203; now_ms < 300; now_ms++) { // short dips do not count
        cycle(&io, BOTH_AWAY | ((now_ms % 50) ? BIT(IN_BUTTON_REFERENCE) : 0), 0, now_ms);
    }

    CHECK(!io_take_falling(&io, IN_BUTTON_REFERENCE));
    for (uint32_t now_ms = 300; now_ms < 600; now_ms++) {
        cycle(&io, BOTH_AWAY, 0, now_ms);
    }

    CHECK(io_take_falling(&io, IN_BUTTON_REFERENCE) && !io_take_rising(&io, IN_BUTTON_REFERENCE));

    Io io2; // an input already on at start gives one rising edge after the debounce
    io_init(&io2);
    for (uint32_t now_ms = 0; now_ms < 200; now_ms++) {
        cycle(&io2, BOTH_AWAY | BIT(IN_CNC_MEASURING_ON), 0, now_ms);
    }

    CHECK(!io_take_rising(&io2, IN_CNC_MEASURING_ON));
    for (uint32_t now_ms = 200; now_ms < 400; now_ms++) {
        cycle(&io2, BOTH_AWAY | BIT(IN_CNC_MEASURING_ON), 0, now_ms);
    }

    CHECK(io_take_rising(&io2, IN_CNC_MEASURING_ON) && !io_take_rising(&io2, IN_CNC_MEASURING_ON));
}

static void test_counter(void)
{
    Io io;
    io_init(&io);
    uint32_t raw = 0xFFFFFFF0u, now_ms = 0;
    cycle(&io, BOTH_AWAY, raw, now_ms++); // baseline
    for (int i = 0; i < 3; i++) {
        cycle(&io, BOTH_AWAY, ++raw, now_ms++); // three changes as baselines
    }

    CHECK(io.unit[UNIT_REAR].pos == 0);
    for (int i = 0; i < 20; i++) { // wraps around 2^32
        cycle(&io, BOTH_AWAY, ++raw, now_ms++);
    }

    CHECK(io.unit[UNIT_REAR].pos == 20);
    io_drive_back(&io, UNIT_REAR);
    raw += 5;
    cycle(&io, BOTH_AWAY, raw, now_ms++);
    CHECK(io.unit[UNIT_REAR].pos == 15 && (io.outputs & BIT(OUT_REAR_BACK)));
    cycle(&io, BIT(IN_REF_FRONT), raw, now_ms++); // rear reference switch reached: reset, drive off
    CHECK(io.unit[UNIT_REAR].pos == 0 && !(io.outputs & BIT(OUT_REAR_BACK)));
}

static void test_free_run(void)
{
    Io io;
    io_init(&io);
    uint32_t raw = 0, now_ms = 0;
    for (int i = 0; i < 4; i++) {
        cycle(&io, BOTH_AWAY, raw++, now_ms++);
    }

    io_drive_forward(&io, UNIT_REAR);
    uint16_t out = 0;
    while (io.unit[UNIT_REAR].pos < FREE_RUN_LIMIT_PULSES - 1) {
        out = cycle(&io, BOTH_AWAY, raw++, now_ms++);
        CHECK(out & BIT(OUT_REAR_FORWARD));
        if (!(out & BIT(OUT_REAR_FORWARD))) {
            break;
        }
    }

    out = cycle(&io, BOTH_AWAY, raw++, now_ms++);
    CHECK(io.unit[UNIT_REAR].pos == FREE_RUN_LIMIT_PULSES && !(out & BIT(OUT_REAR_FORWARD)) &&
          io.unit[UNIT_REAR].state == DRIVE_OFF);

    io_drive_forward(&io, UNIT_FRONT); // sensor stop only while enabled
    cycle(&io, BOTH_AWAY, raw, now_ms++);
    io_sensor_stop_request(&io, UNIT_FRONT);
    out = cycle(&io, BOTH_AWAY, raw, now_ms++);
    CHECK(out & BIT(OUT_FRONT_FORWARD));
    io.sensor_stop = true;
    io_sensor_stop_request(&io, UNIT_FRONT);
    out = cycle(&io, BOTH_AWAY, raw, now_ms++);
    CHECK(!(out & BIT(OUT_FRONT_FORWARD)));
}

static void test_lamps(void)
{
    Io io;
    io_init(&io);
    io_lamp_blink(&io, UNIT_REAR, BLINK_REFERENCE_SWITCH_MS);
    uint32_t first_on_ms = 0;
    for (uint32_t now_ms = LAMP_GRID_MS; now_ms <= 1000 && !first_on_ms; now_ms += LAMP_GRID_MS) {
        io_lamp_tick(&io, now_ms);
        if (io.lamp[UNIT_REAR].level) {
            first_on_ms = now_ms;
        }
    }

    CHECK(first_on_ms == BLINK_REFERENCE_SWITCH_MS + LAMP_GRID_MS); // period + one grid step
    io_lamp_set(&io, UNIT_REAR, true);
    io_lamp_tick(&io, 2000);
    CHECK(io.lamp[UNIT_REAR].level && io.lamp[UNIT_REAR].period_ms == 0);
}

int main(void)
{
    test_debounce();
    test_counter();
    test_free_run();
    test_lamps();

    return check_report("io");
}
