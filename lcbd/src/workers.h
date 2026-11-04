#ifndef LCBD_WORKERS_H
#define LCBD_WORKERS_H

#include "hal.h"
#include "shared.h"

#define PORT_MAX 65535

typedef struct {
    Shared *sh;
    Hal *hal;
} IoThreadArgs;

typedef struct {
    Shared *sh;
    int unit;
    const char *host;
    int port;
} SensorThreadArgs;

typedef struct {
    Shared *sh;
    const char *device;
} LinkThreadArgs;

typedef struct {
    Shared *sh;
    int listen_fd;
} WebThreadArgs;

void *io_thread_main(void *arg);     // 1 ms machine I/O cycle, controller, sequences
void *sensor_thread_main(void *arg); // TCP client of one laser sensor
void *link_thread_main(void *arg);   // serial CNC link
void *web_thread_main(void *arg);
int web_listen(const char *spec);

#endif
