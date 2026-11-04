#include "hal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "ecat.h"
#include "io.h"
#include "sim.h"

struct Hal {
    Ecat *ecat; // NULL = simulation
    Machine m;
    uint32_t now_ms;                      // model time, one exchange is 1 ms
    atomic_int press_ms[IO_CHANNELS + 1]; // remaining press time, indexed by input number
};

Hal *hal_open_ecat(const char *interface_name, char *err, size_t err_size)
{
    Ecat *ecat = ecat_open(interface_name, err, err_size);
    if (!ecat) {
        return NULL;
    }

    Hal *hal = calloc(1, sizeof(*hal));
    if (!hal) {
        ecat_close(ecat);
        snprintf(err, err_size, "out of memory");
        return NULL;
    }

    hal->ecat = ecat;

    return hal;
}

Hal *hal_open_sim(double start_pos)
{
    Hal *hal = calloc(1, sizeof(*hal));
    if (hal) {
        machine_init(&hal->m, start_pos);
    }

    return hal;
}

bool hal_exchange(Hal *hal, uint16_t out, uint16_t *in, uint32_t raw[UNITS])
{
    if (hal->ecat) {
        return ecat_exchange(hal->ecat, out, in, raw);
    }

    machine_step(&hal->m, out, hal->now_ms++);
    uint16_t pressed = 0;
    for (int input = 1; input <= IO_CHANNELS; input++) {
        if (atomic_load(&hal->press_ms[input]) > 0) {
            atomic_fetch_sub(&hal->press_ms[input], 1);
            pressed |= (uint16_t)BIT(input);
        }
    }

    hal->m.extra_inputs = pressed;
    *in = machine_inputs(&hal->m);
    raw[UNIT_REAR] = hal->m.raw[UNIT_REAR];
    raw[UNIT_FRONT] = hal->m.raw[UNIT_FRONT];

    return true;
}

void hal_close(Hal *hal)
{
    ecat_close(hal->ecat);
    free(hal);
}

void hal_describe(const Hal *hal, char *buf, size_t size)
{
    if (hal->ecat) {
        ecat_describe(hal->ecat, buf, size);
    } else {
        snprintf(buf, size, "machine model (simulation)\n");
    }
}

void hal_sim_press(Hal *hal, int input, int ms) { atomic_store(&hal->press_ms[input], ms); }
