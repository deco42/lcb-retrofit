#include "io.h"

#include <stdlib.h>
#include <string.h>

#define SETTLE_READINGS 4 // after a reset: one baseline reading, then three changes taken as baseline

const int IO_FORWARD_OUTPUT[UNITS] = {[UNIT_REAR] = OUT_REAR_FORWARD, [UNIT_FRONT] = OUT_FRONT_FORWARD};
const int IO_BACK_OUTPUT[UNITS] = {[UNIT_REAR] = OUT_REAR_BACK, [UNIT_FRONT] = OUT_FRONT_BACK};
const int IO_LAMP_OUTPUT[UNITS] = {[UNIT_REAR] = OUT_LAMP_REAR, [UNIT_FRONT] = OUT_LAMP_FRONT};
static const int REF_INPUT[UNITS] = {[UNIT_REAR] = IN_REF_REAR, [UNIT_FRONT] = IN_REF_FRONT};

static void reset_counter(Unit *drive)
{
    drive->pos = 0;
    drive->settle_readings = SETTLE_READINGS;
}

static void count_pulses(Unit *drive, uint32_t raw, bool at_reference)
{
    if (at_reference) {
        reset_counter(drive);
    }

    if (drive->settle_readings == SETTLE_READINGS || (drive->settle_readings > 0 && raw != drive->previous_raw)) {
        drive->previous_raw = raw;
        drive->settle_readings--;
        return;
    }

    if (drive->settle_readings > 0) {
        return;
    }

    uint32_t delta = raw - drive->previous_raw; // unsigned wrap-around
    drive->previous_raw = raw;
    drive->pos += drive->direction * (int32_t)delta;
}

#define DEBOUNCE_MS 200 // a new level is accepted when it lasts longer than this

static void debounce_commands(Io *io)
{
    for (int n = IN_BUTTON_REFERENCE; n <= IN_CNC_MEASURING_OFF; n++) {
        Debounce *cmd = &io->cmd[n];
        bool level = io_input(io, n);
        if (level == cmd->level) {
            cmd->differs = false;
        } else if (!cmd->differs) {
            cmd->differs = true;
            cmd->since_ms = io->now_ms;
        } else if (io->now_ms - cmd->since_ms > DEBOUNCE_MS) {
            cmd->level = level;
            cmd->differs = false;
            cmd->rising = level;
            cmd->falling = !level;
        }
    }
}

#define POS_WINDOW_PULSES 25 // the run ends this close to the target, then the drive coasts
#define POS_COAST_MS 100
#define POS_TOLERANCE_PULSES 5
#define POS_PULSE_MS 3  // correction pulse
#define POS_PAUSE_MS 10 // pause after a correction pulse
#define POS_TIMEOUT_MS 8000

static int32_t deviation(const Unit *drive) { return abs(drive->pos - drive->target); }

static void stop_in_position(Unit *drive)
{
    drive->state = DRIVE_IN_POSITION;
    drive->target = drive->pos;
}

static void start_pulse(Unit *drive, uint32_t now_ms)
{
    drive->previous_deviation = deviation(drive);
    drive->direction = drive->target > drive->pos ? 1 : -1;
    drive->state = DRIVE_POS_PULSE;
    drive->phase_ms = now_ms;
}

static void step_positioning(Io *io, Unit *drive)
{
    uint32_t now_ms = io->now_ms, elapsed_ms = now_ms - drive->phase_ms;
    if (now_ms - drive->start_ms > POS_TIMEOUT_MS) {
        drive->state = DRIVE_TIMEOUT;
        return;
    }

    switch (drive->state) {
    case DRIVE_POS_RUN:
        if (drive->direction > 0 ? drive->target <= drive->pos + POS_WINDOW_PULSES
                                 : drive->pos - POS_WINDOW_PULSES <= drive->target) {
            drive->state = DRIVE_POS_COAST;
            drive->phase_ms = now_ms;
        }
        break;
    case DRIVE_POS_COAST:
        if (elapsed_ms < POS_COAST_MS) {
            break;
        }

        if (deviation(drive) <= POS_TOLERANCE_PULSES) {
            stop_in_position(drive);
        } else {
            start_pulse(drive, now_ms);
        }
        break;
    case DRIVE_POS_PULSE:
        if (elapsed_ms >= POS_PULSE_MS) {
            drive->state = DRIVE_POS_PAUSE;
            drive->phase_ms = now_ms;
        }
        break;
    case DRIVE_POS_PAUSE:
        if (elapsed_ms <= POS_PAUSE_MS &&
            (deviation(drive) < POS_TOLERANCE_PULSES || deviation(drive) > drive->previous_deviation)) {
            stop_in_position(drive);
        } else if (elapsed_ms >= POS_PAUSE_MS) {
            start_pulse(drive, now_ms);
        }
        break;
    default:
        break;
    }
}

static void step_drive(Io *io, int unit)
{
    Unit *drive = &io->unit[unit];
    bool stop = drive->stop_request && io->sensor_stop;
    drive->stop_request = false;
    if (stop && io_unit_running(io, unit)) {
        drive->state = DRIVE_OFF;
        return;
    }

    switch (drive->state) {
    case DRIVE_FORWARD:
        if (drive->pos >= FREE_RUN_LIMIT_PULSES) {
            drive->state = DRIVE_OFF;
        }
        break;
    case DRIVE_BACK:
        if (io_at_reference(io, unit)) {
            drive->state = DRIVE_OFF;
        }
        break;
    case DRIVE_POS_RUN:
    case DRIVE_POS_COAST:
    case DRIVE_POS_PULSE:
    case DRIVE_POS_PAUSE:
        step_positioning(io, drive);
        break;
    default:
        break;
    }
}

static uint16_t compose_outputs(const Io *io)
{
    uint16_t out = (uint16_t)(io->aux_outputs & IO_AUX_OUTPUTS);
    for (int unit = 0; unit < UNITS; unit++) {
        const Unit *drive = &io->unit[unit];
        bool moving = drive->state == DRIVE_FORWARD || drive->state == DRIVE_BACK || drive->state == DRIVE_POS_RUN ||
                      drive->state == DRIVE_POS_PULSE;
        if (moving) {
            out |= (uint16_t)BIT(drive->direction > 0 ? IO_FORWARD_OUTPUT[unit] : IO_BACK_OUTPUT[unit]);
        }

        if (io->lamp[unit].level) {
            out |= (uint16_t)BIT(IO_LAMP_OUTPUT[unit]);
        }
    }

    if (io->cylinder == CYLINDER_DOWN) {
        out |= BIT(OUT_CYLINDER_DOWN);
    } else if (io->cylinder == CYLINDER_UP) {
        out |= BIT(OUT_CYLINDER_UP);
    }

    return out;
}

void io_init(Io *io)
{
    memset(io, 0, sizeof(*io));
    io->cylinder = CYLINDER_OFF;
    for (int unit = 0; unit < UNITS; unit++) {
        io->unit[unit].direction = 1;
        io->unit[unit].settle_readings = SETTLE_READINGS;
    }
}

void io_read_inputs(Io *io, uint16_t inputs, const uint32_t raw[UNITS], uint32_t now_ms)
{
    io->now_ms = now_ms;
    io->inputs = inputs;
    for (int unit = 0; unit < UNITS; unit++) {
        io->raw[unit] = raw[unit];
        count_pulses(&io->unit[unit], raw[unit], io_at_reference(io, unit));
    }

    debounce_commands(io);
}

uint16_t io_write_outputs(Io *io)
{
    for (int unit = 0; unit < UNITS; unit++) {
        step_drive(io, unit);
    }

    io->outputs = compose_outputs(io);

    return io->outputs;
}

void io_lamp_tick(Io *io, uint32_t now_ms)
{
    for (int unit = 0; unit < UNITS; unit++) {
        Lamp *lamp = &io->lamp[unit];
        if (lamp->period_ms != 0 && now_ms - lamp->toggled_ms > (uint32_t)lamp->period_ms) {
            lamp->level = !lamp->level;
            lamp->toggled_ms = now_ms;
        }
    }
}

bool io_at_reference(const Io *io, int unit) { return !io_input(io, REF_INPUT[unit]); }

bool io_unit_running(const Io *io, int unit)
{
    DriveState state = io->unit[unit].state;

    return state != DRIVE_OFF && state != DRIVE_IN_POSITION && state != DRIVE_TIMEOUT;
}

void io_drive_off(Io *io, int unit) { io->unit[unit].state = DRIVE_OFF; }

void io_drive_forward(Io *io, int unit)
{
    io->unit[unit].direction = 1;
    io->unit[unit].state = DRIVE_FORWARD;
}

void io_drive_back(Io *io, int unit)
{
    io->unit[unit].direction = -1;
    io->unit[unit].state = DRIVE_BACK;
}

bool io_drive_to(Io *io, int unit, int32_t target)
{
    Unit *drive = &io->unit[unit];
    if (io_unit_running(io, unit)) {
        return false;
    }

    drive->target = target;
    drive->direction = target > drive->pos ? 1 : -1;
    drive->state = DRIVE_POS_RUN;
    drive->start_ms = drive->phase_ms = io->now_ms;

    return true;
}

void io_reset_counter(Io *io, int unit) { reset_counter(&io->unit[unit]); }

void io_sensor_stop_request(Io *io, int unit) { io->unit[unit].stop_request = true; }

void io_lamp_blink(Io *io, int unit, int period_ms)
{
    Lamp *lamp = &io->lamp[unit];
    lamp->period_ms = period_ms;
    lamp->level = false;
    lamp->toggled_ms = io->now_ms;
}

void io_lamp_set(Io *io, int unit, bool on)
{
    io->lamp[unit].period_ms = 0;
    io->lamp[unit].level = on;
}

void io_drive_off_all(Io *io)
{
    for (int unit = 0; unit < UNITS; unit++) {
        io_drive_off(io, unit);
    }
}

void io_lamp_blink_all(Io *io, int period_ms)
{
    for (int unit = 0; unit < UNITS; unit++) {
        io_lamp_blink(io, unit, period_ms);
    }
}

void io_lamp_set_all(Io *io, bool on)
{
    for (int unit = 0; unit < UNITS; unit++) {
        io_lamp_set(io, unit, on);
    }
}

bool io_take_rising(Io *io, int n)
{
    bool rising = io->cmd[n].rising;
    io->cmd[n].rising = false;

    return rising;
}

bool io_take_falling(Io *io, int n)
{
    bool falling = io->cmd[n].falling;
    io->cmd[n].falling = false;

    return falling;
}

const char *io_drive_state_name(DriveState state)
{
    static const char *const names[] = {
        [DRIVE_OFF] = "off",
        [DRIVE_FORWARD] = "forward",
        [DRIVE_BACK] = "back",
        [DRIVE_POS_RUN] = "positioning",
        [DRIVE_POS_COAST] = "coasting",
        [DRIVE_POS_PULSE] = "correction",
        [DRIVE_POS_PAUSE] = "correction",
        [DRIVE_IN_POSITION] = "in position",
        [DRIVE_TIMEOUT] = "timeout",
    };

    return names[state];
}
