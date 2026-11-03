#ifndef LCBD_UNIT_H
#define LCBD_UNIT_H

enum { UNIT_REAR = 0, UNIT_FRONT = 1, UNITS = 2 };

static inline const char *unit_name(int unit) { return unit == UNIT_REAR ? "rear" : "front"; }

#endif
