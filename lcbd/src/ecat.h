#ifndef LCBD_ECAT_H
#define LCBD_ECAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unit.h"

typedef struct Ecat Ecat;

Ecat *ecat_open(const char *interface_name, char *err, size_t err_size);
bool ecat_exchange(Ecat *ecat, uint16_t out, uint16_t *in, uint32_t raw[UNITS]);
void ecat_close(Ecat *ecat);
void ecat_describe(const Ecat *ecat, char *buf, size_t size);

#endif
