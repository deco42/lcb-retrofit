#ifndef LCBD_CONTROL_INTERNAL_H
#define LCBD_CONTROL_INTERNAL_H

#include "control.h"

#define DIE_ANSWER_MS 1500 // the die request sequence waits this long for the answer of the CNC

void ctl_say(CtlPorts *ports, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ctl_sensor_cmd(CtlPorts *ports, SensorEvent cmd);

// State 0 is the idle state of every sequence
static inline bool sequence_idle(const Sequence *seq) { return seq->request == SEQUENCE_NONE && seq->state == 0; }

#endif
