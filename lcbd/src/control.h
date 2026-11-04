#ifndef LCBD_CONTROL_H
#define LCBD_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "io.h"
#include "sensorproto.h"

#define CTL_CMD_MAX 16
#define CTL_CYCLE_MS 10
#define CTL_SEQUENCE_CYCLE_MS 100

typedef struct {
    // Inputs, refreshed by the caller before each tick
    int phase;
    uint16_t set_angle_x10;
    bool sensor_referenced[UNITS];
    const uint8_t *dnc; // pending DNC message (NULL = none), consumed by the controller
    int dnc_len;
    // Outputs, drained by the caller after each tick
    SensorEvent cmd[CTL_CMD_MAX]; // for both sensors
    int n_cmd;
    bool link_flags_set; // hand the flags below to the CNC link
    bool link_referenced, link_encoder_used, link_leg_check;
    int sensor_phase; // phase shown with the sensors (display only)
    void (*log)(void *ctx, const char *text);
    void *log_ctx;
} CtlPorts;

typedef enum {
    SEQUENCE_NONE,
    SEQUENCE_HOME,
    SEQUENCE_FULL_REF,
    SEQUENCE_STATIC_REF,
    SEQUENCE_CYLINDER_DOWN,
    SEQUENCE_CYLINDER_UP,
    SEQUENCE_CYLINDER_INVALID,
    SEQUENCE_DIE_POS,
    SEQUENCE_DIE_LIST
} SequenceRequest;

typedef enum {
    REPLY_NONE,
    REPLY_HOME,
    REPLY_REFERENCED,
    REPLY_DOWN,
    REPLY_UP,
    REPLY_ERROR,
    REPLY_TIMEOUT, // no reply in time, set by the controller itself
} SequenceReply;

typedef struct {
    int state; // own states of each sequence, 0 = idle
    SequenceRequest request;
    SequenceReply reply;
    uint32_t step_start_ms;
    bool unit_referenced[UNITS]; // full reference: unit referenced during the forward run
    int32_t ref_pos[UNITS];      // full reference: position where the sensor stopped the unit
    uint32_t start_raw[UNITS];   // raw counters at the start of the forward run
} Sequence;

typedef enum { CTL_START, CTL_IDLE, CTL_WAIT_REF, CTL_WAIT_CYLINDER, CTL_WAIT_DIE, CTL_DELAY } CtlState;
typedef enum { CTL_ERROR_NONE = 0, CTL_ERROR_CYLINDER = 2, CTL_ERROR_REFERENCE = 3, CTL_ERROR_DIE = 4 } CtlError;

typedef struct {
    CtlState state;
    SequenceRequest follow; // reference request after the awaited reply, SEQUENCE_NONE = back to idle
    uint32_t wait_since_ms, delay_until_ms;
    bool starting; // waiting for the start cylinder sequence
    bool cylinder_up, referenced, scanning, home_ref_pending;
    CtlError error;
    bool encoder_used;
    int control_angle, measure_type, measure_this_bend; // from DNC
    CtlSetup setup;
    bool die_list_request;
    int n_dies;         // received from the service page
    bool first_up_done; // cylinder: first "up" since program start
    Sequence ref, cylinder, die;
    uint32_t now_ms;
} Controller;

typedef enum { IO_OUTPUT_ON, IO_OUTPUT_OFF, IO_RESET_COUNTER, IO_DRIVE_TO } IoServiceOp;

void controller_init(Controller *ctl, const CtlSetup *setup);
// Every CTL_CYCLE_MS
void controller_tick(Controller *ctl, Io *io, CtlPorts *ports, uint32_t now_ms);
// Every CTL_SEQUENCE_CYCLE_MS
void sequences_tick(Controller *ctl, Io *io, CtlPorts *ports, uint32_t now_ms);

bool service_io(Io *io, IoServiceOp op, int arg, int32_t value, char *reply, size_t size);
void service_apply_setup(Controller *ctl, const CtlSetup *setup, CtlPorts *ports, char *reply, size_t size);
void service_set_dies(Controller *ctl, const DieEntry *dies, int n_dies, CtlPorts *ports, char *reply, size_t size);
void service_read_die_list(Controller *ctl, char *reply, size_t size);

const char *controller_state_name(const Controller *ctl);
const char *sequence_state_name(const Sequence *seq);
const char *controller_error_text(CtlError error);

#endif
