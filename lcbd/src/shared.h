#ifndef LCBD_SHARED_H
#define LCBD_SHARED_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "control.h"
#include "evaluator.h"
#include "io.h"
#include "nlr.h"
#include "station.h"
#include "unit.h"

#define HISTORY_MAX 1024
#define SENSOR_ADDR_MAX 96
#define SERVICE_REPLY_MAX 256
#define DATA_DIR_MAX 256

// The sensor clients are in unit order, the worker of a unit is WORKER_REAR + unit
typedef enum { WORKER_IO, WORKER_LINK, WORKER_REAR, WORKER_FRONT = WORKER_REAR + UNIT_FRONT, WORKERS } Worker;

typedef struct {
    double time_s;
    float rear, front, combined;
    int phase;
} HistoryEntry;

typedef struct {
    char addr[SENSOR_ADDR_MAX];
    bool connected;
    unsigned long valid_frame_count, invalid_frame_count;
    bool have_frame;
    uint8_t frame[FRAME_SIZE];
    EvalResult res;
    int phase; // link phase told by the controller (display only)
} SensorView;

typedef enum { SERVICE_IO, SERVICE_SETUP, SERVICE_DIES, SERVICE_DIE_LIST } ServiceKind;

typedef struct {
    ServiceKind kind;
    IoServiceOp op;
    int arg;
    int32_t value;
    CtlSetup setup;
    DieEntry dies[DIE_MAX];
    int n_dies;
} ServiceRequest;

typedef enum { REQUEST_IDLE, REQUEST_PENDING, REQUEST_DONE } RequestState;

typedef struct {
    pthread_mutex_t lock;

    // I/O thread -> others: snapshot refreshed every controller cycle
    Io io;
    Controller ctl;
    atomic_uint cycle_us, cycle_max_us, ctl_cycle_us; // I/O cycle statistics (display)
    bool sim;

    // Controller -> sensor clients
    SensorEvent sensor_cmd[UNITS][CTL_CMD_MAX];
    int sensor_n_cmd[UNITS];
    uint32_t ref_generation[UNITS]; // number of "start reference" commands issued

    // Sensor clients -> controller and I/O
    uint32_t ref_done_generation[UNITS]; // generation for which the sensor reported "referenced"
    atomic_bool stop_drive[UNITS];

    // CNC link: protocol state owned by the link thread, inputs written by the others
    NlrLink link;
    bool link_open, link_contact, link_modem;
    LineState line_state;
    uint8_t dnc[NLR_DNC_MAX]; // last complete DNC message, waiting for the controller
    int dnc_len;
    bool dnc_pending;

    Station station;
    SensorView sensor[UNITS];
    HistoryEntry history[HISTORY_MAX];
    int history_head, n_history;

    // Service request mailbox (web -> I/O thread)
    ServiceRequest request;
    RequestState request_state;
    bool request_ok;
    char request_reply[SERVICE_REPLY_MAX];

    char data_dir[DATA_DIR_MAX];
    SetupFile setup;
    SensorFile sensor_file;

    atomic_uint progress[WORKERS]; // loop passes of every worker, watched by main
    atomic_bool stop;
    atomic_int exit_code;
} Shared;

void shared_init(Shared *sh);
bool shared_request(Shared *sh, const ServiceRequest *request, char *reply, size_t size);

static inline void shared_progress(Shared *sh, Worker worker) { atomic_fetch_add(&sh->progress[worker], 1); }

void shared_pause(Shared *sh, Worker worker, int ms);

// The caller holds sh->lock for both history functions
void shared_history_push(Shared *sh, const HistoryEntry *entry);
int shared_history_copy(const Shared *sh, HistoryEntry out[HISTORY_MAX]);

#endif
