#ifndef LCBD_IO_H
#define LCBD_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "unit.h"

#define IO_CHANNELS 16
#define BIT(n) (1u << ((n)-1)) // input En / output An -> bit mask

// Inputs E1..E16 (EL1819)
#define IN_SWIVEL_ZONE 1 // backgauge in the swivel zone: cylinder up is locked
#define IN_REF_FRONT 2   // reference switches: 0 = at reference
#define IN_REF_REAR 3
#define IN_CYLINDER_DOWN 4
#define IN_BUTTON_REFERENCE 5 // E5..E9 are debounced command inputs
#define IN_BUTTON_CYLINDER 6
#define IN_NEW_DIE_POSITION 7 // from the PLC
#define IN_CNC_MEASURING_ON 8
#define IN_CNC_MEASURING_OFF 9
#define IN_COUNT_FRONT 10
#define IN_COUNT_REAR 14

// Outputs A1..A16 (EL2809)
#define OUT_FRONT_FORWARD 1
#define OUT_FRONT_BACK 2
#define OUT_REAR_FORWARD 3
#define OUT_REAR_BACK 4
#define OUT_CYLINDER_DOWN 5
#define OUT_CYLINDER_UP 6
#define OUT_LAMP_REAR 7
#define OUT_LAMP_FRONT 8
#define IO_AUX_OUTPUTS 0xFF00u // A9..A16, switched by the service page only

extern const int IO_FORWARD_OUTPUT[UNITS], IO_BACK_OUTPUT[UNITS], IO_LAMP_OUTPUT[UNITS];

#define FREE_RUN_LIMIT_PULSES 4000

#define BLINK_REFERENCE_SWITCH_MS 300
#define BLINK_SENSOR_REFERENCE_MS 150

typedef enum {
    DRIVE_OFF,
    DRIVE_FORWARD, // free run forward
    DRIVE_BACK,    // free run back to the reference switch
    DRIVE_POS_RUN,
    DRIVE_POS_COAST,
    DRIVE_POS_PULSE, // correction pulse
    DRIVE_POS_PAUSE,
    DRIVE_IN_POSITION,
    DRIVE_TIMEOUT,
} DriveState;

typedef enum {
    CYLINDER_OFF = -1, // A5 and A6 off
    CYLINDER_UP,       // A6 on
    CYLINDER_DOWN,     // A5 on
} CylinderOutput;

typedef struct {
    DriveState state;
    int direction; // direction last commanded: +1 forward, -1 back
    int32_t pos;   // position in pulses
    uint32_t previous_raw;
    int settle_readings; // > 0: readings still taken as baseline only
    bool stop_request;
    int32_t target;              // pulses
    uint32_t start_ms, phase_ms; // positioning: start of the order and of its current step
    int32_t previous_deviation;  // deviation after the previous correction pulse
} Unit;

typedef struct {
    int period_ms; // blink half period, 0 = steady
    bool level;
    uint32_t toggled_ms;
} Lamp;

typedef struct {
    bool level; // accepted level
    bool rising, falling;
    uint32_t since_ms; // start of the current difference (valid while differs)
    bool differs;
} Debounce;

typedef struct {
    uint32_t now_ms;
    uint16_t inputs, outputs;
    uint32_t raw[UNITS];
    Unit unit[UNITS];
    Lamp lamp[UNITS];
    Debounce cmd[IO_CHANNELS + 1]; // indexed by input number
    CylinderOutput cylinder;
    bool sensor_stop; // sensor stop enabled (forward run of a full reference)
    uint16_t aux_outputs;
} Io;

void io_init(Io *io);
// One 1 ms cycle
void io_read_inputs(Io *io, uint16_t inputs, const uint32_t raw[UNITS], uint32_t now_ms);
uint16_t io_write_outputs(Io *io);
void io_lamp_tick(Io *io, uint32_t now_ms);

static inline bool io_input(const Io *io, int n) { return (io->inputs & BIT(n)) != 0; }
// Reference switch of a unit: true = at reference (input 0)
bool io_at_reference(const Io *io, int unit);
bool io_unit_running(const Io *io, int unit);

void io_drive_off(Io *io, int unit);
void io_drive_forward(Io *io, int unit);
void io_drive_back(Io *io, int unit);
bool io_drive_to(Io *io, int unit, int32_t target);
void io_reset_counter(Io *io, int unit);
void io_sensor_stop_request(Io *io, int unit);

void io_lamp_blink(Io *io, int unit, int period_ms);
void io_lamp_set(Io *io, int unit, bool on);

void io_drive_off_all(Io *io);
void io_lamp_blink_all(Io *io, int period_ms);
void io_lamp_set_all(Io *io, bool on);

// Consumes an edge flag of a debounced command input (E5..E9)
bool io_take_rising(Io *io, int n);
bool io_take_falling(Io *io, int n);

const char *io_drive_state_name(DriveState state);

#endif
