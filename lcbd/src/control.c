#include "control_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "nlr.h"

static void request_sequence(Controller *ctl, Sequence *seq, SequenceRequest request, SequenceRequest follow)
{
    seq->request = request;
    seq->reply = REPLY_NONE;
    ctl->state = seq == &ctl->ref ? CTL_WAIT_REF : seq == &ctl->cylinder ? CTL_WAIT_CYLINDER : CTL_WAIT_DIE;
    ctl->follow = follow;
    ctl->wait_since_ms = ctl->now_ms;
    if (seq != &ctl->die) { // the error shows 0 while a reference or cylinder sequence runs
        ctl->error = CTL_ERROR_NONE;
    }
}

static void run_follow(Controller *ctl)
{
    SequenceRequest follow = ctl->follow;
    ctl->follow = SEQUENCE_NONE;
    if (follow == SEQUENCE_NONE) {
        ctl->state = CTL_IDLE;
    } else {
        request_sequence(ctl, &ctl->ref, follow, SEQUENCE_NONE);
    }
}

static void set_scanning(Controller *ctl, CtlPorts *ports, bool on)
{
    ctl_sensor_cmd(ports, on ? SENSOR_START_SCAN : SENSOR_STOP_SCAN);
    ctl->scanning = on;
}

static void start(Controller *ctl, CtlPorts *ports)
{
    set_scanning(ctl, ports, false);
    if (ctl->setup.move_down_start) {
        ctl->starting = true;
        request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_DOWN, SEQUENCE_NONE);
    } else {
        ctl->state = CTL_IDLE;
    }
}

#define SCAN_START_PHASE_LAST 4 // a scan starts in the phases NLR_PHASE_MEASURE_FIRST to this one

static void handle_phase(Controller *ctl, CtlPorts *ports)
{
    int phase = ports->phase;
    ports->sensor_phase = phase;

    if (phase >= NLR_PHASE_MEASURE_FIRST && phase <= SCAN_START_PHASE_LAST && !ctl->scanning) {
        set_scanning(ctl, ports, true);
        ports->link_flags_set = true;
        ports->link_referenced = ctl->referenced;
        ports->link_encoder_used = ctl->encoder_used;
        ports->link_leg_check = ctl->setup.secure_angle;
        ctl_say(ports, "measuring phase %d: scan started, link gets referenced %d, encoder used %d, leg check %d",
                phase, ctl->referenced, ctl->encoder_used, ctl->setup.secure_angle);
    } else if (phase == NLR_PHASE_END && ctl->scanning) {
        set_scanning(ctl, ports, false);
    } else if (phase == NLR_PHASE_IDLE) {
        bool continuous = ports->set_angle_x10 == NLR_CONTINUOUS_SCAN_ANGLE_X10;
        if (continuous != ctl->scanning) {
            set_scanning(ctl, ports, continuous);
        }
    }
}

#define DNC_RECORD_LEN 18 // binary record: DNC_RECORD_LEN bytes starting with two DNC_RECORD_MARK bytes
#define DNC_RECORD_MARK 0xAA
#define DNC_LINE_MAX 255
#define DNC_NUMBERS_MAX 8
#define DNC_EFFECT_MAX 80

typedef enum { DNC_MOVE_METHOD = 1, DNC_USE_TURN = 2, DNC_SECURE_ANGLE = 4, DNC_SET_VARIABLE = 100 } DncOption;

// A text line with only '-' and digits left: numbers separated by '-', the first is the option, a middle one (if
// three) the variable, the last one the value
static void handle_dnc_line(Controller *ctl, CtlPorts *ports, char *line)
{
    long numbers[DNC_NUMBERS_MAX];
    int n_numbers = 0;
    for (char *next = line; *next && n_numbers < DNC_NUMBERS_MAX;) {
        if (*next == '-') {
            next++;
        } else {
            numbers[n_numbers++] = strtol(next, &next, 10);
        }
    }

    if (n_numbers == 0) {
        return;
    }

    long option = numbers[0], variable = n_numbers >= 3 ? numbers[1] : -1, value = numbers[n_numbers - 1];
    bool switch_value = n_numbers >= 2 && (value == 0 || value == 1);
    const char *effect = "no effect";
    char effect_buf[DNC_EFFECT_MAX];
    if (switch_value && option == DNC_MOVE_METHOD) {
        ctl->setup.move_method = value ? MOVE_METHOD_POS : MOVE_METHOD_REF;
        effect = value ? "move method MV_POS" : "move method MV_REF";
    } else if (switch_value && option == DNC_USE_TURN) {
        ctl->setup.use_turn = value;
        effect = value ? "use turn on" : "use turn off";
    } else if (switch_value && option == DNC_SECURE_ANGLE) {
        ctl->setup.secure_angle = value;
        effect = value ? "secure angle and leg check on" : "secure angle and leg check off";
    } else if (n_numbers >= 3 && option == DNC_SET_VARIABLE) {
        struct {
            long variable;
            const char *name;
            int *dst;
        } variables[] = {{1734, "measure-this-bend", &ctl->measure_this_bend},
                         {55, "control-angle", &ctl->control_angle},
                         {2568, "measure-type", &ctl->measure_type}};
        for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]); i++) {
            if (variable != variables[i].variable) {
                continue;
            }

            *variables[i].dst = (int)value;
            ctl->encoder_used = false;
            snprintf(effect_buf, sizeof(effect_buf), "%s := %ld, encoder used 0", variables[i].name, value);
            effect = effect_buf;
        }
    }

    ctl_say(ports, "DNC: option %ld variable %ld value %ld: %s", option, variable, value, effect);
}

static void handle_dnc(Controller *ctl, CtlPorts *ports, const uint8_t *msg, int len)
{
    if (len == DNC_RECORD_LEN && msg[0] == DNC_RECORD_MARK && msg[1] == DNC_RECORD_MARK) {
        ctl->control_angle = msg[2];
        ctl->measure_type = msg[4];
        ctl->measure_this_bend = msg[6];
        ctl->encoder_used = ctl->control_angle == 1 && ctl->measure_type != 0 && ctl->measure_this_bend == 1;
        ctl_say(ports, "DNC: record control-angle %d measure-type %d measure-this-bend %d: encoder used %d",
                ctl->control_angle, ctl->measure_type, ctl->measure_this_bend, ctl->encoder_used);
        return;
    }

    for (int i = 0; i < len;) {
        char line[DNC_LINE_MAX + 1];
        int line_len = 0, n_kept = 0;
        while (i < len && msg[i] != '\n') {
            if (line_len < DNC_LINE_MAX) {
                line_len++;
                if (msg[i] == '-' || isdigit(msg[i])) {
                    line[n_kept++] = (char)msg[i];
                }
            }

            i++;
        }

        i++; // skip '\n'
        line[n_kept] = '\0';
        if (n_kept == 0) {
            break;
        }

        handle_dnc_line(ctl, ports, line);
    }
}

static bool handle_cylinder_button(Controller *ctl, Io *io, CtlPorts *ports)
{
    if (!io_take_rising(io, IN_BUTTON_CYLINDER)) {
        return false;
    }

    if (ctl->cylinder_up) {
        ctl_say(ports, "E6: cylinder down");
        request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_DOWN, SEQUENCE_NONE);
    } else if (!io_input(io, IN_SWIVEL_ZONE)) {
        ctl_say(ports, "E6: cylinder up");
        request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_UP, SEQUENCE_NONE);
    } else {
        ctl_say(ports, "E6: cylinder up not allowed (E1 = 1), invalid request: units home");
        request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_INVALID, SEQUENCE_NONE);
    }

    return true;
}

static bool handle_reference_button(Controller *ctl, Io *io, CtlPorts *ports)
{
    if (!io_take_rising(io, IN_BUTTON_REFERENCE)) {
        return false;
    }

    if (!ctl->cylinder_up) {
        ctl_say(ports, "E5: ignored, cylinder is down");
        return false;
    }

    ctl_say(ports, "E5: reference");
    ctl->referenced = false;
    request_sequence(ctl, &ctl->ref, SEQUENCE_STATIC_REF, SEQUENCE_FULL_REF);

    return true;
}

static bool handle_cnc_measuring(Controller *ctl, Io *io, CtlPorts *ports)
{
    if (io_take_rising(io, IN_CNC_MEASURING_ON)) {
        if (io_input(io, IN_CNC_MEASURING_OFF) || (ctl->cylinder_up && ctl->referenced)) {
            ctl_say(ports, "E8: measuring system active, nothing to do");
            return true;
        }

        if (ctl->cylinder_up) {
            ctl_say(ports, "E8: measuring system active, full reference");
            request_sequence(ctl, &ctl->ref, SEQUENCE_FULL_REF, SEQUENCE_NONE);
            return true;
        }

        if (!io_input(io, IN_SWIVEL_ZONE)) {
            ctl_say(ports, "E8: measuring system active, cylinder up and full reference");
            request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_UP, SEQUENCE_FULL_REF);
            return true;
        }

        ctl_say(ports, "E8: cylinder cannot move up, backgauge in the swivel zone (E1)");
        return false;
    }

    if (io_take_rising(io, IN_CNC_MEASURING_OFF)) {
        if (!io_input(io, IN_CNC_MEASURING_ON) && ctl->cylinder_up) {
            ctl_say(ports, "E9: measuring system inactive, cylinder down");
            request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_DOWN, SEQUENCE_NONE);
        } else {
            ctl_say(ports, "E9: measuring system inactive, nothing to do");
        }

        return true;
    }

    return false;
}

static bool handle_die_shift(Controller *ctl, Io *io, CtlPorts *ports)
{
    if (io_take_rising(io, IN_NEW_DIE_POSITION)) {
        if (!ctl->cylinder_up) {
            return false;
        }

        if (ctl->setup.move_method == MOVE_METHOD_REF) {
            ctl_say(ports, "E7: die shift, units home");
            ctl->home_ref_pending = true;
            request_sequence(ctl, &ctl->ref, SEQUENCE_HOME, SEQUENCE_NONE);
        } else {
            ctl_say(ports, "E7: die shift, die request");
            io_lamp_blink_all(io, BLINK_SENSOR_REFERENCE_MS);
            request_sequence(ctl, &ctl->die, SEQUENCE_DIE_POS, SEQUENCE_STATIC_REF);
        }

        return true;
    }

    if (io_take_falling(io, IN_NEW_DIE_POSITION) && ctl->cylinder_up && ctl->setup.move_method == MOVE_METHOD_REF &&
        ctl->home_ref_pending) {
        ctl_say(ports, "E7 released: full reference");
        ctl->home_ref_pending = false;
        request_sequence(ctl, &ctl->ref, SEQUENCE_FULL_REF, SEQUENCE_NONE);
        return true;
    }

    return false;
}

static void idle(Controller *ctl, Io *io, CtlPorts *ports)
{
    handle_phase(ctl, ports);
    if (ports->phase != NLR_PHASE_IDLE) {
        return;
    }

    if (handle_cylinder_button(ctl, io, ports) || handle_reference_button(ctl, io, ports) ||
        handle_cnc_measuring(ctl, io, ports) || handle_die_shift(ctl, io, ports)) {
        return;
    }

    if (ports->dnc) {
        handle_dnc(ctl, ports, ports->dnc, ports->dnc_len);
        ports->dnc = NULL;
        return;
    }

    if (ctl->die_list_request) {
        ctl->die_list_request = false;
        if (sequence_idle(&ctl->die)) {
            ctl_say(ports, "die list requested from the CNC");
            ctl->die.request = SEQUENCE_DIE_LIST;
        } else {
            ctl_say(ports, "die list: request refused, die request sequence busy");
        }
    }
}

#define WAIT_REF_MS 17000
#define WAIT_CYLINDER_MS 9500
#define WAIT_DIE_MS 11000
#define RETRY_DELAY_MS 110 // after a failed reference sequence

static SequenceReply take_reply(const Controller *ctl, Sequence *seq, uint32_t timeout_ms)
{
    SequenceReply reply = seq->reply;
    seq->reply = REPLY_NONE;
    if (reply == REPLY_NONE && ctl->now_ms - ctl->wait_since_ms >= timeout_ms) {
        return REPLY_TIMEOUT;
    }

    return reply;
}

static void report_error(Controller *ctl, CtlPorts *ports, CtlError error, const char *sequence, SequenceReply reply,
                         uint32_t timeout_ms)
{
    ctl->error = error;
    if (reply == REPLY_TIMEOUT) {
        ctl_say(ports, "%s: timeout %g s (error %d)", sequence, ms_to_s(timeout_ms), error);
    } else {
        ctl_say(ports, "%s: failed (error %d)", sequence, error);
    }
}

static void wait_reference(Controller *ctl, const Io *io, CtlPorts *ports)
{
    SequenceReply reply = take_reply(ctl, &ctl->ref, WAIT_REF_MS);
    if (reply == REPLY_HOME || reply == REPLY_REFERENCED) {
        ctl->referenced = reply == REPLY_REFERENCED;
        ctl_say(ports, "reference sequence: %s, counters rear %d front %d",
                reply == REPLY_HOME ? "units home" : "referenced", (int)io->unit[UNIT_REAR].pos,
                (int)io->unit[UNIT_FRONT].pos);
        ctl->follow = SEQUENCE_NONE;
        ctl->state = CTL_IDLE;
    } else if (reply == REPLY_ERROR || reply == REPLY_TIMEOUT) {
        ctl->referenced = false;
        report_error(ctl, ports, CTL_ERROR_REFERENCE, "reference sequence", reply, WAIT_REF_MS);
        if (reply == REPLY_ERROR) {
            ctl->state = CTL_DELAY;
            ctl->delay_until_ms = ctl->now_ms + RETRY_DELAY_MS;
        } else {
            run_follow(ctl);
        }
    }
}

static void wait_cylinder(Controller *ctl, CtlPorts *ports)
{
    SequenceReply reply = take_reply(ctl, &ctl->cylinder, WAIT_CYLINDER_MS);
    if (reply == REPLY_UP || reply == REPLY_DOWN) {
        ctl->cylinder_up = reply == REPLY_UP;
        ctl->referenced = false;
        ctl_say(ports, "cylinder %s confirmed", reply == REPLY_UP ? "up" : "down");
        ctl->starting = false;
        if (reply == REPLY_UP) {
            run_follow(ctl);
        } else {
            ctl->state = CTL_IDLE;
        }
    } else if (reply == REPLY_ERROR || reply == REPLY_TIMEOUT) {
        report_error(ctl, ports, CTL_ERROR_CYLINDER, "cylinder sequence", reply, WAIT_CYLINDER_MS);
        ctl->follow = SEQUENCE_NONE;
        if (ctl->starting) {
            ctl_say(ports, "start: repeating cylinder down");
            request_sequence(ctl, &ctl->cylinder, SEQUENCE_CYLINDER_DOWN, SEQUENCE_NONE);
        } else {
            ctl->state = CTL_IDLE;
        }
    }
}

// On this machine the die request never succeeds (no DNC die names)
static void wait_die(Controller *ctl, Io *io, CtlPorts *ports)
{
    SequenceReply reply = take_reply(ctl, &ctl->die, WAIT_DIE_MS);
    if (reply == REPLY_NONE) {
        return;
    }

    report_error(ctl, ports, CTL_ERROR_DIE, "die request", reply, WAIT_DIE_MS);
    io_lamp_set_all(io, false);
    run_follow(ctl);
}

static bool refuse(char *reply, size_t size, const char *reason)
{
    snprintf(reply, size, "refused: %s", reason);

    return false;
}

static int output_unit(const int outputs[UNITS], int n)
{
    for (int unit = 0; unit < UNITS; unit++) {
        if (outputs[unit] == n) {
            return unit;
        }
    }

    return -1;
}

static bool output_on(Io *io, int n, char *reply, size_t size)
{
    int forward_unit = output_unit(IO_FORWARD_OUTPUT, n);
    int back_unit = output_unit(IO_BACK_OUTPUT, n);
    int lamp_unit = output_unit(IO_LAMP_OUTPUT, n);
    if ((forward_unit >= 0 && (io->outputs & BIT(IO_BACK_OUTPUT[forward_unit]))) ||
        (back_unit >= 0 && (io->outputs & BIT(IO_FORWARD_OUTPUT[back_unit])))) {
        return refuse(reply, size, "opposite direction of that unit is on");
    }

    if (forward_unit >= 0) {
        io_drive_forward(io, forward_unit);
    } else if (back_unit >= 0) {
        io_drive_back(io, back_unit);
    } else if (lamp_unit >= 0) {
        io_lamp_set(io, lamp_unit, true);
    } else if (n == OUT_CYLINDER_DOWN) {
        if (io->cylinder == CYLINDER_UP) {
            return refuse(reply, size, "A6 is on");
        }

        io->cylinder = CYLINDER_DOWN;
    } else if (n == OUT_CYLINDER_UP) {
        if (io->cylinder == CYLINDER_DOWN) {
            return refuse(reply, size, "A5 is on");
        }

        if (io_input(io, IN_SWIVEL_ZONE)) {
            return refuse(reply, size, "E1 = 1 (backgauge in the swivel zone)");
        }

        io->cylinder = CYLINDER_UP;
    } else {
        io->aux_outputs |= (uint16_t)BIT(n);
    }

    snprintf(reply, size, "A%d on", n);

    return true;
}

static bool output_off(Io *io, int n, char *reply, size_t size)
{
    int forward_unit = output_unit(IO_FORWARD_OUTPUT, n);
    int back_unit = output_unit(IO_BACK_OUTPUT, n);
    int lamp_unit = output_unit(IO_LAMP_OUTPUT, n);
    if (forward_unit >= 0) {
        io_drive_off(io, forward_unit);
    } else if (back_unit >= 0) {
        io_drive_off(io, back_unit);
    } else if (lamp_unit >= 0) {
        io_lamp_set(io, lamp_unit, false);
    } else if ((n == OUT_CYLINDER_DOWN && io->cylinder == CYLINDER_DOWN) ||
               (n == OUT_CYLINDER_UP && io->cylinder == CYLINDER_UP)) {
        io->cylinder = CYLINDER_OFF;
    } else if (BIT(n) & IO_AUX_OUTPUTS) {
        io->aux_outputs &= (uint16_t)~BIT(n);
    }

    snprintf(reply, size, "A%d off", n);

    return true;
}

static bool reset_counter(Io *io, int unit, char *reply, size_t size)
{
    io_reset_counter(io, unit);
    snprintf(reply, size, "counter %d reset", unit);

    return true;
}

#define DRIVE_TARGET_MAX 65535

static bool drive_to(Io *io, int unit, int32_t target, char *reply, size_t size)
{
    if (target < 0 || target > DRIVE_TARGET_MAX) {
        return refuse(reply, size, "target must be 0..65535");
    }

    if (!io_drive_to(io, unit, target)) {
        return refuse(reply, size, "drive is running");
    }

    snprintf(reply, size, "unit %d driving to %d", unit, (int)target);

    return true;
}

void controller_init(Controller *ctl, const CtlSetup *setup)
{
    memset(ctl, 0, sizeof(*ctl));
    ctl->setup = *setup;
    ctl->state = CTL_START;
}

void controller_tick(Controller *ctl, Io *io, CtlPorts *ports, uint32_t now_ms)
{
    ctl->now_ms = now_ms;
    io_lamp_tick(io, now_ms);
    switch (ctl->state) {
    case CTL_START:
        start(ctl, ports);
        break;
    case CTL_IDLE:
        idle(ctl, io, ports);
        break;
    case CTL_WAIT_REF:
        wait_reference(ctl, io, ports);
        break;
    case CTL_WAIT_CYLINDER:
        wait_cylinder(ctl, ports);
        break;
    case CTL_WAIT_DIE:
        wait_die(ctl, io, ports);
        break;
    case CTL_DELAY:
        if ((int32_t)(now_ms - ctl->delay_until_ms) >= 0) {
            run_follow(ctl);
        }
        break;
    }
}

bool service_io(Io *io, IoServiceOp op, int arg, int32_t value, char *reply, size_t size)
{
    switch (op) {
    case IO_OUTPUT_ON:
    case IO_OUTPUT_OFF:
        if (arg < 1 || arg > IO_CHANNELS) {
            return refuse(reply, size, "output number must be 1..16");
        }

        return op == IO_OUTPUT_ON ? output_on(io, arg, reply, size) : output_off(io, arg, reply, size);
    case IO_RESET_COUNTER:
    case IO_DRIVE_TO:
        if (arg < 0 || arg >= UNITS) {
            return refuse(reply, size, "unit must be 0 or 1");
        }

        return op == IO_RESET_COUNTER ? reset_counter(io, arg, reply, size) : drive_to(io, arg, value, reply, size);
    }

    return refuse(reply, size, "unknown command");
}

void service_apply_setup(Controller *ctl, const CtlSetup *setup, CtlPorts *ports, char *reply, size_t size)
{
    ctl->setup.move_method = setup->move_method;
    ctl->setup.move_down_start = setup->move_down_start;
    ctl->setup.secure_angle = setup->secure_angle;
    snprintf(reply, size, "setup applied: %s, move down at start %d, secure angle %d",
             move_method_name(setup->move_method), setup->move_down_start, setup->secure_angle);
    ctl_say(ports, "service: %s", reply);
}

void service_set_dies(Controller *ctl, const DieEntry *dies, int n_dies, CtlPorts *ports, char *reply, size_t size)
{
    ctl->n_dies = n_dies;
    for (int i = 0; i < n_dies; i++) {
        ctl_say(ports, "service: die %d '%s' rear %d front %d", i + 1, dies[i].name, (int)dies[i].rear,
                (int)dies[i].front);
    }

    snprintf(reply, size, "%d dies sent to the controller", n_dies);
}

void service_read_die_list(Controller *ctl, char *reply, size_t size)
{
    ctl->die_list_request = true;
    snprintf(reply, size, "die list requested from the CNC (answer expected within %g s)", ms_to_s(DIE_ANSWER_MS));
}

const char *controller_state_name(const Controller *ctl)
{
    static const char *const names[] = {
        [CTL_START] = "start",
        [CTL_IDLE] = "idle",
        [CTL_WAIT_REF] = "waiting for reference",
        [CTL_WAIT_CYLINDER] = "waiting for cylinder",
        [CTL_WAIT_DIE] = "waiting for die request",
        [CTL_DELAY] = "retry delay",
    };

    return names[ctl->state];
}

const char *controller_error_text(CtlError error)
{
    static const char *const texts[] = {
        [CTL_ERROR_NONE] = "no error",
        [CTL_ERROR_CYLINDER] = "cylinder",
        [CTL_ERROR_REFERENCE] = "reference",
        [CTL_ERROR_DIE] = "die request",
    };

    return texts[error];
}

#define SAY_MAX 300

void ctl_say(CtlPorts *ports, const char *fmt, ...)
{
    char text[SAY_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ports->log(ports->log_ctx, text);
}

void ctl_sensor_cmd(CtlPorts *ports, SensorEvent cmd)
{
    if (ports->n_cmd < CTL_CMD_MAX) {
        ports->cmd[ports->n_cmd++] = cmd;
    }
}
