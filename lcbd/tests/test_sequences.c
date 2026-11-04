#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "clock.h"
#include "control.h"
#include "io.h"
#include "nlr.h"
#include "sim.h"

#define LOG_LINES 400
#define LOG_TEXT_MAX 160
#define REPLY_MAX 160
#define NO_DIE (-1) // die position of a sensor that never sees a die

#define SENSOR_FRAME_MS 20
#define FRAMES_TO_REFERENCE 30 // frames that see the die until the sensor reports "referenced"

typedef struct {
    Machine m;
    Io io;
    Controller ctl;
    CtlPorts ports;
    uint32_t now_ms;
    int die_pos[UNITS];
    int visible_frames[UNITS];
    bool stop_pending[UNITS], referenced[UNITS], counting[UNITS];
    int start_scan_count;
    int phase;
    bool link_referenced;
    bool link_set;
    uint16_t button;
    uint32_t button_until_ms;
    uint16_t last_outputs;
    uint32_t lamp_change_ms; // last change of A7/A8
    bool both_directions_on;
    char log[LOG_LINES][LOG_TEXT_MAX];
    int n_log;
} Rig;

static void rig_log(void *ctx, const char *text)
{
    Rig *rig = ctx;
    if (rig->n_log < LOG_LINES) {
        snprintf(rig->log[rig->n_log++], sizeof(rig->log[0]), "%s", text);
    }
}

static int log_count(const Rig *rig, const char *needle)
{
    int count = 0;
    for (int i = 0; i < rig->n_log; i++) {
        count += strstr(rig->log[i], needle) != NULL;
    }

    return count;
}

static bool log_has(const Rig *rig, const char *needle) { return log_count(rig, needle) > 0; }

static void rig_init(Rig *rig, double start_pos, bool move_down_start, int die_rear, int die_front)
{
    memset(rig, 0, sizeof(*rig));
    machine_init(&rig->m, start_pos);
    io_init(&rig->io);
    CtlSetup setup = {.move_method = MOVE_METHOD_REF, .move_down_start = move_down_start};
    controller_init(&rig->ctl, &setup);
    rig->ports.log = rig_log;
    rig->ports.log_ctx = rig;
    rig->die_pos[UNIT_REAR] = die_rear;
    rig->die_pos[UNIT_FRONT] = die_front;
}

static void drain(Rig *rig)
{
    for (int i = 0; i < rig->ports.n_cmd; i++) {
        switch (rig->ports.cmd[i]) {
        case SENSOR_START_REFERENCE:
            for (int unit = 0; unit < UNITS; unit++) {
                rig->visible_frames[unit] = 0;
                rig->stop_pending[unit] = true;
                rig->referenced[unit] = false;
                rig->counting[unit] = true;
            }
            break;
        case SENSOR_START_SCAN:
            rig->start_scan_count++;
            break;
        case SENSOR_STOP_SCAN:
        case SENSOR_CONNECTED: // sent by the sensor client itself, never by the controller
            break;
        }
    }

    rig->ports.n_cmd = 0;
    if (rig->ports.link_flags_set) {
        rig->ports.link_flags_set = false;
        rig->link_set = true;
        rig->link_referenced = rig->ports.link_referenced;
    }
}

static void sensor_frame(Rig *rig)
{
    for (int unit = 0; unit < UNITS; unit++) {
        if (rig->die_pos[unit] == NO_DIE || rig->m.pos[unit] < rig->die_pos[unit] || !rig->counting[unit]) {
            continue;
        }

        rig->visible_frames[unit]++;
        if (rig->stop_pending[unit]) {
            rig->stop_pending[unit] = false;
            io_sensor_stop_request(&rig->io, unit);
        }

        if (rig->visible_frames[unit] >= FRAMES_TO_REFERENCE) {
            rig->referenced[unit] = true;
        }
    }
}

static void step(Rig *rig)
{
    uint16_t inputs = machine_inputs(&rig->m);
    if (rig->now_ms < rig->button_until_ms) {
        inputs |= rig->button;
    }

    io_read_inputs(&rig->io, inputs, rig->m.raw, rig->now_ms);
    if (rig->now_ms % SENSOR_FRAME_MS == 0) {
        sensor_frame(rig);
    }

    if (rig->now_ms % CTL_CYCLE_MS == 0) {
        rig->ports.phase = rig->phase;
        for (int unit = 0; unit < UNITS; unit++) {
            rig->ports.sensor_referenced[unit] = rig->referenced[unit];
        }

        controller_tick(&rig->ctl, &rig->io, &rig->ports, rig->now_ms);
        drain(rig);
    }

    if (rig->now_ms % CTL_SEQUENCE_CYCLE_MS == 0) {
        sequences_tick(&rig->ctl, &rig->io, &rig->ports, rig->now_ms);
        drain(rig);
    }

    uint16_t outputs = io_write_outputs(&rig->io);
    for (int unit = 0; unit < UNITS; unit++) {
        if ((outputs & BIT(IO_FORWARD_OUTPUT[unit])) && (outputs & BIT(IO_BACK_OUTPUT[unit]))) {
            rig->both_directions_on = true;
        }
    }

    if ((outputs ^ rig->last_outputs) & (BIT(OUT_LAMP_REAR) | BIT(OUT_LAMP_FRONT))) {
        rig->lamp_change_ms = rig->now_ms;
    }

    rig->last_outputs = outputs;
    machine_step(&rig->m, outputs, rig->now_ms);
    rig->now_ms++;
}

static void run(Rig *rig, uint32_t duration_ms)
{
    for (uint32_t i = 0; i < duration_ms; i++) {
        step(rig);
    }
}

#define WAIT(rig, cond, max_ms)                                                                                        \
    do {                                                                                                               \
        uint32_t until_ms_ = (rig)->now_ms + (max_ms);                                                                 \
        while (!(cond) && (rig)->now_ms < until_ms_) {                                                                 \
            step(rig);                                                                                                 \
        }                                                                                                              \
    } while (0)

static void hold(Rig *rig, int input, uint32_t duration_ms)
{
    rig->button = (uint16_t)BIT(input);
    rig->button_until_ms = rig->now_ms + duration_ms;
}

static void press(Rig *rig, int input) { hold(rig, input, SIM_BUTTON_MS); }

static bool output_on(const Rig *rig, int n) { return (rig->io.outputs & BIT(n)) != 0; }

static bool controller_idle(const Rig *rig)
{
    return rig->ctl.state == CTL_IDLE && rig->ctl.ref.request == SEQUENCE_NONE &&
           rig->ctl.cylinder.request == SEQUENCE_NONE;
}

static void check_timing(const char *what, uint32_t from_ms, uint32_t to_ms, double expected_s, double tolerance_s)
{
    double measured_s = ms_to_s(to_ms - from_ms);
    printf("  %-44s %6.3f s  (reference %.2f +/- %.2f)\n", what, measured_s, expected_s, tolerance_s);
    CHECK(measured_s >= expected_s - tolerance_s && measured_s <= expected_s + tolerance_s);
}

static void scenario_reference_without_die(Rig *rig)
{
    printf("Scenario 1: reference without die\n");
    rig_init(rig, 1500, true, NO_DIE, NO_DIE);
    WAIT(rig, output_on(rig, OUT_FRONT_BACK) && output_on(rig, OUT_REAR_BACK), 1000);
    uint32_t start_ms = rig->now_ms;
    WAIT(rig, !output_on(rig, OUT_REAR_BACK), 3000);
    check_timing("start: rear unit at reference (A4 off)", start_ms, rig->now_ms, 1.50, 0.15);
    WAIT(rig, output_on(rig, OUT_CYLINDER_DOWN), 3000);
    check_timing("start: A5 on", start_ms, rig->now_ms, 1.70, 0.15);
    WAIT(rig, controller_idle(rig), 3000);
    CHECK(!rig->ctl.cylinder_up && !rig->ctl.referenced);

    press(rig, IN_BUTTON_CYLINDER);
    start_ms = rig->now_ms;
    WAIT(rig, output_on(rig, OUT_CYLINDER_UP), 3000);
    check_timing("E6: A6 on (after 0.5 s down pulse)", start_ms, rig->now_ms, 0.92, 0.15);
    WAIT(rig, controller_idle(rig), 3000);
    CHECK(rig->ctl.cylinder_up);

    press(rig, IN_BUTTON_REFERENCE);
    start_ms = rig->now_ms;
    WAIT(rig, output_on(rig, OUT_LAMP_REAR) || output_on(rig, OUT_LAMP_FRONT), 2000);
    check_timing("E5: first lamp on", start_ms, rig->now_ms, 0.46, 0.15);
    WAIT(rig, output_on(rig, OUT_REAR_FORWARD), 3000);
    check_timing("E5: A3 on", start_ms, rig->now_ms, 1.70, 0.15);
    WAIT(rig, !output_on(rig, OUT_REAR_FORWARD), 6000);
    check_timing("E5: A3 off at 4000", start_ms, rig->now_ms, 5.72, 0.15);
    CHECK(rig->io.unit[UNIT_REAR].pos >= FREE_RUN_LIMIT_PULSES && rig->io.unit[UNIT_REAR].pos <= 4010);
    WAIT(rig, controller_idle(rig) && rig->ctl.error == CTL_ERROR_REFERENCE, 20000);
    run(rig, 1000);
    check_timing("E5: both lamps off for good", start_ms, rig->lamp_change_ms, 9.65, 0.25);
    CHECK(!rig->ctl.referenced && rig->ctl.error == CTL_ERROR_REFERENCE && !output_on(rig, OUT_LAMP_REAR) &&
          !output_on(rig, OUT_LAMP_FRONT));

    press(rig, IN_BUTTON_CYLINDER);
    start_ms = rig->now_ms;
    WAIT(rig, output_on(rig, OUT_REAR_BACK), 2000);
    check_timing("E6 park: A4 on", start_ms, rig->now_ms, 0.29, 0.15);
    WAIT(rig, !output_on(rig, OUT_REAR_BACK), 6000);
    check_timing("E6 park: A4 off", start_ms, rig->now_ms, 4.31, 0.15);
    WAIT(rig, output_on(rig, OUT_CYLINDER_DOWN), 2000);
    check_timing("E6 park: A5 on", start_ms, rig->now_ms, 4.49, 0.15);
    WAIT(rig, controller_idle(rig), 3000);
    CHECK(!rig->ctl.cylinder_up);
    CHECK(!rig->both_directions_on);
}

static void reference_with_die(Rig *rig)
{
    press(rig, IN_BUTTON_CYLINDER);
    WAIT(rig, controller_idle(rig) && rig->ctl.cylinder_up, 5000);
    CHECK(rig->ctl.cylinder_up);
    press(rig, IN_BUTTON_REFERENCE);
    uint32_t press_ms = rig->now_ms;
    WAIT(rig, output_on(rig, OUT_LAMP_REAR) || output_on(rig, OUT_LAMP_FRONT), 2000);
    uint32_t start_ms = rig->now_ms;
    printf("  (first lamp %.3f s after the press)\n", ms_to_s(start_ms - press_ms));
    WAIT(rig, output_on(rig, OUT_FRONT_FORWARD) && output_on(rig, OUT_REAR_FORWARD), 3000);
    check_timing("E5 die: A1+A3 on", start_ms, rig->now_ms, 1.24, 0.35);
    WAIT(rig, !output_on(rig, OUT_REAR_FORWARD), 4000);
    check_timing("E5 die: rear stop", start_ms, rig->now_ms, 2.88, 0.35);
    WAIT(rig, !output_on(rig, OUT_FRONT_FORWARD), 4000);
    check_timing("E5 die: front stop", start_ms, rig->now_ms, 3.27, 0.35);
    run(rig, 5);
    CHECK(rig->m.pos[UNIT_REAR] >= 1600 && rig->m.pos[UNIT_REAR] <= 1640);
    CHECK(rig->m.pos[UNIT_FRONT] >= 2000 && rig->m.pos[UNIT_FRONT] <= 2040);
    WAIT(rig, !output_on(rig, OUT_LAMP_REAR) && !output_on(rig, OUT_LAMP_FRONT), 4000);
    check_timing("E5 die: both referenced (steady lamp off)", start_ms, rig->now_ms, 3.94, 0.35);
    WAIT(rig,
         output_on(rig, OUT_LAMP_REAR) && output_on(rig, OUT_LAMP_FRONT) && rig->io.lamp[UNIT_REAR].period_ms == 0 &&
             rig->io.lamp[UNIT_FRONT].period_ms == 0,
         4000);
    check_timing("E5 die: lamps steady on", start_ms, rig->now_ms, 4.64, 0.35);
    WAIT(rig, controller_idle(rig), 3000);
    CHECK(rig->ctl.referenced && rig->ctl.error == CTL_ERROR_NONE);
}

static void scenario_reference_with_die(Rig *rig)
{
    printf("Scenario 2: reference with die\n");
    rig_init(rig, 0, true, 1600, 2000);
    WAIT(rig, controller_idle(rig) && rig->now_ms > 500, 5000);
    reference_with_die(rig);

    int scans = rig->start_scan_count;
    rig->phase = NLR_PHASE_MEASURE_FIRST;
    run(rig, 50);
    CHECK(rig->ctl.scanning && rig->start_scan_count == scans + 1);
    CHECK(rig->link_set && rig->link_referenced);
    rig->phase = NLR_PHASE_END;
    run(rig, 50);
    CHECK(!rig->ctl.scanning);
    rig->phase = NLR_PHASE_IDLE;
    run(rig, 50);
    CHECK(!rig->ctl.scanning); // set angle not 150.0: no continuous scan

    press(rig, IN_BUTTON_CYLINDER);
    WAIT(rig, controller_idle(rig) && !rig->ctl.cylinder_up, 8000);
    CHECK(!rig->ctl.cylinder_up && io_at_reference(&rig->io, UNIT_REAR) && io_at_reference(&rig->io, UNIT_FRONT) &&
          !rig->ctl.referenced);
}

static void scenario_cnc_commands(Rig *rig)
{
    printf("Scenario 3: CNC commands\n");
    rig_init(rig, 0, true, 1600, 2000);
    WAIT(rig, controller_idle(rig) && rig->now_ms > 500, 5000);
    press(rig, IN_CNC_MEASURING_ON);
    run(rig, 400);
    WAIT(rig, controller_idle(rig), 15000);
    CHECK(rig->ctl.cylinder_up && rig->ctl.referenced);

    hold(rig, IN_NEW_DIE_POSITION, 5000);
    run(rig, 400);
    WAIT(rig, controller_idle(rig), 12000);
    CHECK(io_at_reference(&rig->io, UNIT_REAR) && io_at_reference(&rig->io, UNIT_FRONT) && !rig->ctl.referenced);
    WAIT(rig, rig->now_ms >= rig->button_until_ms, 6000);
    run(rig, 400);
    WAIT(rig, controller_idle(rig), 15000);
    CHECK(rig->ctl.referenced);

    press(rig, IN_CNC_MEASURING_OFF);
    run(rig, 400);
    WAIT(rig, controller_idle(rig), 8000);
    CHECK(!rig->ctl.cylinder_up);
}

static void scenario_dnc(Rig *rig)
{
    static const uint8_t record[18] = {0xAA, 0xAA, 0x01, 0x00, 0x01, 0x00, 0x01};
    static const char text[] = "1-0-1\n4-0-1\n100-1734-1\n";
    printf("Scenario 4: DNC\n");
    rig_init(rig, 0, false, NO_DIE, NO_DIE);
    run(rig, 50);
    rig->ports.dnc = record;
    rig->ports.dnc_len = sizeof(record);
    run(rig, 20);
    CHECK(rig->ports.dnc == NULL && rig->ctl.encoder_used);

    rig->ports.dnc = (const uint8_t *)text;
    rig->ports.dnc_len = (int)strlen(text);
    run(rig, 20);
    CHECK(rig->ctl.setup.move_method == MOVE_METHOD_POS && rig->ctl.setup.secure_angle &&
          rig->ctl.measure_this_bend == 1 && !rig->ctl.encoder_used);
    CHECK(log_count(rig, "DNC: option") == 3); // each text line logged
}

static void scenario_cylinder_stuck(Rig *rig)
{
    printf("Scenario 5: cylinder stuck\n");
    rig_init(rig, 0, true, NO_DIE, NO_DIE);
    rig->m.cylinder_stuck = true;
    run(rig, 6000);
    CHECK(log_has(rig, "(error 2)"));
    CHECK(output_on(rig, OUT_CYLINDER_DOWN));
    CHECK(log_count(rig, "start: repeating cylinder down") >= 2);
}

static bool service(Rig *rig, IoServiceOp op, int arg, int32_t value)
{
    char reply[REPLY_MAX];

    return service_io(&rig->io, op, arg, value, reply, sizeof(reply));
}

static void scenario_service(Rig *rig)
{
    printf("Scenario 6: service\n");
    rig_init(rig, 0, false, NO_DIE, NO_DIE);
    run(rig, 100);

    const Unit *rear = &rig->io.unit[UNIT_REAR], *front = &rig->io.unit[UNIT_FRONT];
    rig->m.coast_ms = 15;
    CHECK(service(rig, IO_DRIVE_TO, UNIT_REAR, 2000));
    run(rig, 500);
    CHECK(!service(rig, IO_DRIVE_TO, UNIT_REAR, 1000)); // second order while running
    WAIT(rig, rear->state == DRIVE_IN_POSITION || rear->state == DRIVE_TIMEOUT, 9000);
    run(rig, 50);
    CHECK(rear->state == DRIVE_IN_POSITION && abs(rear->pos - 2000) <= 5);

    rig->m.coast_ms = 60;
    CHECK(service(rig, IO_DRIVE_TO, UNIT_FRONT, 1500));
    bool corrected_back = false;
    while (io_unit_running(&rig->io, UNIT_FRONT) && rig->now_ms < 30000) {
        step(rig);
        corrected_back |= front->state == DRIVE_POS_PULSE && front->direction < 0;
    }

    run(rig, 50);
    CHECK(front->state == DRIVE_IN_POSITION && abs(front->pos - 1500) <= 5 && corrected_back);
    CHECK(service(rig, IO_DRIVE_TO, UNIT_FRONT, 1000)); // new order after reaching the position
    WAIT(rig, !io_unit_running(&rig->io, UNIT_FRONT), 9000);
    CHECK(!rig->both_directions_on);

    rig->m.coast_ms = 0;
    CHECK(service(rig, IO_OUTPUT_ON, OUT_REAR_FORWARD, 0));
    run(rig, 20);
    CHECK(!service(rig, IO_OUTPUT_ON, OUT_REAR_BACK, 0)); // opposite direction on
    WAIT(rig, !output_on(rig, OUT_REAR_FORWARD), 5000);
    CHECK(rear->pos >= FREE_RUN_LIMIT_PULSES && rear->pos <= 4010);
    CHECK(service(rig, IO_OUTPUT_ON, OUT_REAR_BACK, 0) && (run(rig, 2), output_on(rig, OUT_REAR_BACK)));
    WAIT(rig, !output_on(rig, OUT_REAR_BACK), 6000);
    CHECK(io_at_reference(&rig->io, UNIT_REAR));

    CHECK(service(rig, IO_OUTPUT_ON, OUT_CYLINDER_DOWN, 0));
    run(rig, 2);
    CHECK(!service(rig, IO_OUTPUT_ON, OUT_CYLINDER_UP, 0)); // A5 is on
    CHECK(service(rig, IO_OUTPUT_OFF, OUT_CYLINDER_DOWN, 0));
    rig->m.extra_inputs = BIT(IN_SWIVEL_ZONE);
    run(rig, 2);
    CHECK(!service(rig, IO_OUTPUT_ON, OUT_CYLINDER_UP, 0));
    rig->m.extra_inputs = 0;
    run(rig, 2);
    CHECK(service(rig, IO_OUTPUT_ON, OUT_CYLINDER_UP, 0) && (run(rig, 2), output_on(rig, OUT_CYLINDER_UP)));
    CHECK(service(rig, IO_OUTPUT_ON, OUT_LAMP_REAR, 0) && (run(rig, 2), output_on(rig, OUT_LAMP_REAR)));
    CHECK(service(rig, IO_OUTPUT_OFF, OUT_LAMP_REAR, 0) && (run(rig, 2), !output_on(rig, OUT_LAMP_REAR)));

    char reply[REPLY_MAX];
    CtlSetup setup = {.move_method = MOVE_METHOD_POS, .move_down_start = false, .secure_angle = true};
    service_apply_setup(&rig->ctl, &setup, &rig->ports, reply, sizeof(reply));
    CHECK(rig->ctl.setup.secure_angle && rig->ctl.setup.move_method == MOVE_METHOD_POS);

    service_read_die_list(&rig->ctl, reply, sizeof(reply));
    uint32_t start_ms = rig->now_ms;
    WAIT(rig, log_has(rig, "die list: timeout"), 3000);
    check_timing("die list request times out", start_ms, rig->now_ms, 1.5, 0.11);
}

int main(void)
{
    static Rig rig; // too large for the stack
    scenario_reference_without_die(&rig);
    scenario_reference_with_die(&rig);
    scenario_cnc_commands(&rig);
    scenario_dnc(&rig);
    scenario_cylinder_stuck(&rig);
    scenario_service(&rig);

    return check_report("sequences");
}
