#ifndef LCBD_HAL_H
#define LCBD_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unit.h"

typedef struct Hal Hal;

Hal *hal_open_ecat(const char *interface_name, char *err, size_t err_size);
Hal *hal_open_sim(double start_pos);
bool hal_exchange(Hal *hal, uint16_t out, uint16_t *in, uint32_t raw[UNITS]);
void hal_close(Hal *hal);
void hal_describe(const Hal *hal, char *buf, size_t size);
// Simulation only, input 1..16
void hal_sim_press(Hal *hal, int input, int ms);

#endif
