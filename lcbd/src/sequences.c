#include "control_internal.h"

#include <stdio.h>

#include "clock.h"

#define HOMING_LIMIT_MS 9000

static void finish(Sequence *seq, SequenceReply reply)
{
    seq->reply = reply;
    seq->request = SEQUENCE_NONE;
    seq->state = 0; // idle
}

static bool both_home(const Io *io) { return io_at_reference(io, UNIT_REAR) && io_at_reference(io, UNIT_FRONT); }

static bool run_home(Io *io)
{
    bool running = false;
    for (int unit = 0; unit < UNITS; unit++) {
        if (!io_at_reference(io, unit)) {
            io_drive_back(io, unit);
            io_lamp_blink(io, unit, BLINK_REFERENCE_SWITCH_MS);
            running = true;
        }
    }

    return running;
}

static void check_homing_time(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms, const char *sequence)
{
    if (now_ms - seq->step_start_ms > HOMING_LIMIT_MS) {
        io_drive_off_all(io);
        io_lamp_set_all(io, false);
        ctl_say(ports, "%s: units not at the reference switches after %g s", sequence, ms_to_s(HOMING_LIMIT_MS));
        finish(seq, REPLY_ERROR);
    }
}

static void switch_off_unreferenced_lamps(Io *io, const bool referenced[UNITS])
{
    for (int unit = 0; unit < UNITS; unit++) {
        if (!referenced[unit]) {
            io_lamp_set(io, unit, false);
        }
    }
}

typedef enum { REF_IDLE, REF_HOMING, REF_FORWARD_START, REF_FORWARD, REF_STATIC } RefState;

#define FORWARD_LIMIT_MS 8000
#define DRIVE_CHECK_MS 2000 // a unit that has not counted a pulse by then fails the forward run
#define STATIC_LIMIT_MS 1000
#define REASON_MAX 40

static void start_sensor_reference(CtlPorts *ports)
{
    ctl_sensor_cmd(ports, SENSOR_START_REFERENCE);
    ports->sensor_referenced[UNIT_REAR] = ports->sensor_referenced[UNIT_FRONT] = false;
}

static void begin_static_reference(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    io_lamp_blink_all(io, BLINK_SENSOR_REFERENCE_MS);
    start_sensor_reference(ports);
    seq->state = REF_STATIC;
    seq->step_start_ms = now_ms;
}

static void begin_reference(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    seq->step_start_ms = now_ms;
    switch (seq->request) {
    case SEQUENCE_HOME:
        if (run_home(io)) {
            io_lamp_blink_all(io, BLINK_REFERENCE_SWITCH_MS); // also the lamp of a unit already home
            seq->state = REF_HOMING;
        } else {
            finish(seq, REPLY_HOME);
        }
        break;
    case SEQUENCE_FULL_REF:
        io_lamp_blink_all(io, BLINK_REFERENCE_SWITCH_MS);
        io->sensor_stop = false;
        run_home(io);
        seq->state = REF_HOMING;
        break;
    case SEQUENCE_STATIC_REF:
        begin_static_reference(seq, io, ports, now_ms);
        break;
    default:
        finish(seq, REPLY_ERROR);
        break;
    }
}

static void begin_forward_run(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    for (int unit = 0; unit < UNITS; unit++) {
        io_reset_counter(io, unit);
        seq->unit_referenced[unit] = false;
        seq->start_raw[unit] = io->raw[unit];
        io_drive_forward(io, unit);
    }

    start_sensor_reference(ports);
    io->sensor_stop = true;
    seq->state = REF_FORWARD;
    seq->step_start_ms = now_ms;
}

static void fail_forward_run(Sequence *seq, Io *io, CtlPorts *ports, const char *reason)
{
    const bool *referenced = seq->unit_referenced;
    io_drive_off_all(io);
    io->sensor_stop = false;
    ctl_sensor_cmd(ports, SENSOR_STOP_SCAN);
    ctl_say(ports, "reference: %s (rear %s, front %s)", reason, referenced[UNIT_REAR] ? "referenced" : "not referenced",
            referenced[UNIT_FRONT] ? "referenced" : "not referenced");
    switch_off_unreferenced_lamps(io, referenced);
    finish(seq, REPLY_ERROR);
}

static void step_forward_run(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    for (int unit = 0; unit < UNITS; unit++) {
        if (ports->sensor_referenced[unit] && !seq->unit_referenced[unit]) {
            io_drive_off(io, unit);
            seq->unit_referenced[unit] = true;
            seq->ref_pos[unit] = io->unit[unit].pos;
            io_lamp_set(io, unit, true);
        }
    }

    if (seq->unit_referenced[UNIT_REAR] && seq->unit_referenced[UNIT_FRONT]) {
        ctl_sensor_cmd(ports, SENSOR_STOP_SCAN);
        io->sensor_stop = false;
        ctl_say(ports, "reference: both units found the die, rear %d, front %d", (int)seq->ref_pos[UNIT_REAR],
                (int)seq->ref_pos[UNIT_FRONT]);
        begin_static_reference(seq, io, ports, now_ms);
        return;
    }

    if (now_ms - seq->step_start_ms >= FORWARD_LIMIT_MS) {
        char reason[REASON_MAX];
        snprintf(reason, sizeof(reason), "forward run timeout (%g s)", ms_to_s(FORWARD_LIMIT_MS));
        fail_forward_run(seq, io, ports, reason);
        return;
    }

    if (now_ms - seq->step_start_ms >= DRIVE_CHECK_MS) {
        for (int unit = 0; unit < UNITS; unit++) {
            if (!seq->unit_referenced[unit] && io->raw[unit] == seq->start_raw[unit]) {
                ctl_say(ports, "reference: %s drive not moving", unit_name(unit));
                fail_forward_run(seq, io, ports, "drive not moving");
                return;
            }
        }
    }
}

static void step_static_reference(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    const bool *referenced = ports->sensor_referenced;
    for (int unit = 0; unit < UNITS; unit++) {
        if (referenced[unit]) {
            io_lamp_set(io, unit, true);
        }
    }

    if (referenced[UNIT_REAR] && referenced[UNIT_FRONT]) {
        ctl_sensor_cmd(ports, SENSOR_STOP_SCAN);
        finish(seq, REPLY_REFERENCED);
    } else if (now_ms - seq->step_start_ms >= STATIC_LIMIT_MS) {
        ctl_sensor_cmd(ports, SENSOR_STOP_SCAN);
        ctl_say(ports, "reference: sensors not referenced within %g s (rear %d, front %d)", ms_to_s(STATIC_LIMIT_MS),
                referenced[UNIT_REAR], referenced[UNIT_FRONT]);
        switch_off_unreferenced_lamps(io, referenced);
        finish(seq, REPLY_ERROR);
    }
}

static void tick_reference(Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    switch (seq->state) {
    case REF_IDLE:
        if (seq->request != SEQUENCE_NONE) {
            begin_reference(seq, io, ports, now_ms);
        }
        break;
    case REF_HOMING:
        if (!both_home(io)) {
            check_homing_time(seq, io, ports, now_ms, "reference");
        } else if (seq->request == SEQUENCE_HOME) {
            io_lamp_set_all(io, false);
            finish(seq, REPLY_HOME);
        } else {
            seq->state = REF_FORWARD_START;
        }
        break;
    case REF_FORWARD_START:
        begin_forward_run(seq, io, ports, now_ms);
        break;
    case REF_FORWARD:
        step_forward_run(seq, io, ports, now_ms);
        break;
    case REF_STATIC:
        step_static_reference(seq, io, ports, now_ms);
        break;
    }
}

typedef enum {
    CYLINDER_IDLE,
    CYLINDER_HOMING,
    CYLINDER_MOVE,
    CYLINDER_WAIT_DOWN,
    CYLINDER_UP_PULSE,
    CYLINDER_WAIT_UP
} CylinderState;

#define CYLINDER_DOWN_LIMIT_MS 1500
#define CYLINDER_FIRST_UP_PULSE_MS 500 // down pulse before the first "up" since program start
#define CYLINDER_UP_CHECK_MS 1000      // E4 is checked from this time after switching up ...
#define CYLINDER_UP_LIMIT_MS 1200      // ... until this time

static void move_cylinder(Controller *ctl, Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    seq->step_start_ms = now_ms;
    if (seq->request == SEQUENCE_CYLINDER_DOWN) {
        io->cylinder = CYLINDER_DOWN;
        seq->state = CYLINDER_WAIT_DOWN;
    } else if (seq->request == SEQUENCE_CYLINDER_UP) {
        ctl_say(ports, "cylinder: moving up");
        if (io_input(io, IN_SWIVEL_ZONE)) {
            ctl_say(ports, "cylinder: up refused, backgauge in the swivel zone (E1)");
            finish(seq, REPLY_ERROR);
        } else if (!ctl->first_up_done) {
            io->cylinder = CYLINDER_DOWN;
            seq->state = CYLINDER_UP_PULSE;
        } else {
            io->cylinder = CYLINDER_UP;
            seq->state = CYLINDER_WAIT_UP;
        }
    } else {
        ctl_say(ports, "cylinder: invalid request");
        finish(seq, REPLY_ERROR);
    }
}

static void tick_cylinder(Controller *ctl, Sequence *seq, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    uint32_t step_ms = now_ms - seq->step_start_ms;
    switch (seq->state) {
    case CYLINDER_IDLE:
        if (seq->request != SEQUENCE_NONE) {
            seq->step_start_ms = now_ms;
            run_home(io);
            seq->state = CYLINDER_HOMING;
        }
        break;
    case CYLINDER_HOMING:
        for (int unit = 0; unit < UNITS; unit++) {
            if (io_at_reference(io, unit) && io->lamp[unit].period_ms == BLINK_REFERENCE_SWITCH_MS) {
                io_lamp_set(io, unit, false);
            }
        }

        if (both_home(io)) {
            seq->state = CYLINDER_MOVE;
        } else {
            check_homing_time(seq, io, ports, now_ms, "cylinder");
        }
        break;
    case CYLINDER_MOVE:
        move_cylinder(ctl, seq, io, ports, now_ms);
        break;
    case CYLINDER_WAIT_DOWN:
        if (io_input(io, IN_CYLINDER_DOWN)) {
            finish(seq, REPLY_DOWN);
        } else if (step_ms >= CYLINDER_DOWN_LIMIT_MS) {
            ctl_say(ports, "cylinder: no 'down' confirmation (E4) within %g s", ms_to_s(CYLINDER_DOWN_LIMIT_MS));
            finish(seq, REPLY_ERROR);
        }
        break;
    case CYLINDER_UP_PULSE:
        if (step_ms >= CYLINDER_FIRST_UP_PULSE_MS) {
            io->cylinder = CYLINDER_UP;
            ctl->first_up_done = true;
            seq->step_start_ms = now_ms;
            seq->state = CYLINDER_WAIT_UP;
        }
        break;
    case CYLINDER_WAIT_UP:
        if (step_ms >= CYLINDER_UP_CHECK_MS && !io_input(io, IN_CYLINDER_DOWN)) {
            finish(seq, REPLY_UP);
        } else if (step_ms >= CYLINDER_UP_LIMIT_MS) {
            ctl_say(ports, "cylinder: still down (E4) %g s after switching up", ms_to_s(CYLINDER_UP_LIMIT_MS));
            finish(seq, REPLY_ERROR);
        }
        break;
    }
}

typedef enum { DIE_IDLE, DIE_WAIT } DieState;

// The CNC sends die names only over the DNC line, which is not connected on this machine: no answer ever comes
static void tick_die(Sequence *seq, CtlPorts *ports, uint32_t now_ms)
{
    if (seq->state == DIE_IDLE) {
        if (seq->request != SEQUENCE_NONE) {
            seq->step_start_ms = now_ms;
            seq->state = DIE_WAIT;
        }

        return;
    }

    if (now_ms - seq->step_start_ms < DIE_ANSWER_MS) {
        return;
    }

    if (seq->request == SEQUENCE_DIE_LIST) { // no reply to the controller
        ctl_say(ports, "die list: timeout, no answer from the CNC (DNC line not connected)");
        seq->request = SEQUENCE_NONE;
        seq->state = DIE_IDLE;
    } else {
        ctl_say(ports, "die request: no die description");
        finish(seq, REPLY_ERROR);
    }
}

void sequences_tick(Controller *ctl, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    tick_reference(&ctl->ref, io, ports, now_ms);
    tick_cylinder(ctl, &ctl->cylinder, io, ports, now_ms);
    tick_die(&ctl->die, ports, now_ms);
}

const char *sequence_state_name(const Sequence *seq)
{
    if (sequence_idle(seq)) {
        return "idle";
    }

    static const char *const names[] = {
        [SEQUENCE_NONE] = "",
        [SEQUENCE_HOME] = "home",
        [SEQUENCE_FULL_REF] = "full reference",
        [SEQUENCE_STATIC_REF] = "static reference",
        [SEQUENCE_CYLINDER_DOWN] = "cylinder down",
        [SEQUENCE_CYLINDER_UP] = "cylinder up",
        [SEQUENCE_CYLINDER_INVALID] = "cylinder (invalid)",
        [SEQUENCE_DIE_POS] = "die request",
        [SEQUENCE_DIE_LIST] = "die list",
    };

    return names[seq->request];
}
