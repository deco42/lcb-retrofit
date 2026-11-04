#ifndef LCBD_SIM_H
#define LCBD_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "io.h"

#define SIM_BUTTON_MS 300

typedef struct {
    double pos[UNITS];   // position in pulses
    uint32_t raw[UNITS]; // EL1502 counters, counting up
    bool cylinder_down;
    CylinderOutput cylinder_target; // valve position last commanded
    uint32_t cylinder_ms;           // time of the last valve change
    uint16_t extra_inputs;          // buttons and external signals (E1, E5..E9)
    int coast_ms;                   // run-on after a longer run (positioning tests), 0 = none
    bool cylinder_stuck;            // E4 never becomes 1 (test "cylinder stuck")
    int run_ms[UNITS], coast_left_ms[UNITS], last_direction[UNITS];
} Machine;

void machine_init(Machine *m, double start_pos);
uint16_t machine_inputs(const Machine *m);
void machine_step(Machine *m, uint16_t outputs, uint32_t now_ms); // one millisecond of mechanics

#endif
