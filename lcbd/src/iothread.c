#include <stdio.h>
#include <string.h>
#include <time.h>

#include "clock.h"
#include "log.h"
#include "workers.h"

#define CYCLE_NS NS_PER_MS
#define BUS_LOST_MS 100
#define LATE_RESYNC_NS (50 * NS_PER_MS) // a cycle this late starts a new cycle grid
#define STARTUP_MS 1000                 // the longest cycle is taken after the start-up
#define LOG_INPUT_MASK ((uint16_t) ~(BIT(IN_COUNT_FRONT) | BIT(IN_COUNT_REAR))) // count pulses are not logged
#define LOG_OUTPUT_MASK 0xFFFF

// Private copies of the I/O thread, published to the shared state every controller cycle
typedef struct {
    Shared *sh;
    Io io;
    Controller ctl;
    CtlPorts ports;
    uint8_t dnc[NLR_DNC_MAX];
} IoThread;

static void log_changes(const char *what, char letter, uint16_t previous, uint16_t current, uint16_t mask)
{
    char buf[200];
    size_t len = 0;
    for (int channel = 1; channel <= IO_CHANNELS; channel++) {
        if ((previous ^ current) & mask & BIT(channel)) {
            len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %c%d=%d", letter, channel,
                                    (current & BIT(channel)) != 0);
        }
    }

    if (len > 0) {
        log_msg("%s:%s", what, buf);
    }
}

static void controller_log(void *context, const char *text)
{
    (void)context;
    log_msg("%s", text);
}

static void ports_from_shared(IoThread *thread)
{
    Shared *sh = thread->sh;
    CtlPorts *ports = &thread->ports;
    ports->phase = sh->link.phase;
    ports->set_angle_x10 = sh->link.set_angle_x10;
    for (int unit = 0; unit < UNITS; unit++) {
        ports->sensor_referenced[unit] =
            sh->ref_generation[unit] != 0 && sh->ref_done_generation[unit] == sh->ref_generation[unit];
    }

    if (sh->dnc_pending && !ports->dnc) {
        memcpy(thread->dnc, sh->dnc, (size_t)sh->dnc_len);
        ports->dnc = thread->dnc;
        ports->dnc_len = sh->dnc_len;
        sh->dnc_pending = false;
    }
}

static void ports_to_shared(IoThread *thread)
{
    Shared *sh = thread->sh;
    CtlPorts *ports = &thread->ports;
    for (int i = 0; i < ports->n_cmd; i++) {
        SensorEvent cmd = ports->cmd[i];
        for (int unit = 0; unit < UNITS; unit++) {
            if (sh->sensor_n_cmd[unit] < CTL_CMD_MAX) {
                sh->sensor_cmd[unit][sh->sensor_n_cmd[unit]++] = cmd;
            }

            if (cmd == SENSOR_START_REFERENCE) {
                sh->ref_generation[unit]++;
            }
        }

        if (cmd == SENSOR_START_SCAN) {
            station_start_scan(&sh->station);
        }
    }

    ports->n_cmd = 0;
    if (ports->link_flags_set) {
        ports->link_flags_set = false;
        sh->link.referenced = ports->link_referenced;
        sh->link.encoder_used = ports->link_encoder_used;
        sh->link.leg_check = ports->link_leg_check;
    }

    for (int unit = 0; unit < UNITS; unit++) {
        sh->sensor[unit].phase = ports->sensor_phase;
    }
}

static void serve_request(IoThread *thread)
{
    Shared *sh = thread->sh;
    ServiceRequest *request = &sh->request;
    char *reply = sh->request_reply;
    size_t size = sizeof(sh->request_reply);
    if (sh->request_state != REQUEST_PENDING) {
        return;
    }

    switch (request->kind) {
    case SERVICE_IO:
        sh->request_ok = service_io(&thread->io, request->op, request->arg, request->value, reply, size);
        break;
    case SERVICE_SETUP:
        service_apply_setup(&thread->ctl, &request->setup, &thread->ports, reply, size);
        sh->request_ok = true;
        break;
    case SERVICE_DIES:
        service_set_dies(&thread->ctl, request->dies, request->n_dies, &thread->ports, reply, size);
        sh->request_ok = true;
        break;
    case SERVICE_DIE_LIST:
        service_read_die_list(&thread->ctl, reply, size);
        sh->request_ok = true;
        break;
    }

    sh->request_state = REQUEST_DONE;
}

static void controller_cycle(IoThread *thread, uint32_t now_ms, bool run_sequences)
{
    Shared *sh = thread->sh;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pthread_mutex_lock(&sh->lock);
    ports_from_shared(thread);
    serve_request(thread);
    controller_tick(&thread->ctl, &thread->io, &thread->ports, now_ms);
    if (run_sequences) {
        sequences_tick(&thread->ctl, &thread->io, &thread->ports, now_ms);
    }

    ports_to_shared(thread);
    sh->io = thread->io;
    sh->ctl = thread->ctl;
    pthread_mutex_unlock(&sh->lock);
    clock_gettime(CLOCK_MONOTONIC, &end);
    atomic_store(&sh->ctl_cycle_us, (unsigned)clock_diff_us(&end, &start));
}

static void end_cycle(Shared *sh, const struct timespec *start, struct timespec *next, uint32_t now_ms)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    unsigned cycle_us = (unsigned)(clock_diff_ns(&end, start) / NS_PER_US);
    atomic_store(&sh->cycle_us, cycle_us);
    if (cycle_us > atomic_load(&sh->cycle_max_us) && now_ms > STARTUP_MS) {
        atomic_store(&sh->cycle_max_us, cycle_us);
    }

    clock_add_ns(next, CYCLE_NS);
    if (clock_diff_ns(&end, next) > LATE_RESYNC_NS) {
        *next = end;
    }

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

void *io_thread_main(void *arg)
{
    const IoThreadArgs *args = arg;
    Shared *sh = args->sh;
    static IoThread io_thread;
    IoThread *thread = &io_thread;
    thread->sh = sh;
    io_init(&thread->io);
    pthread_mutex_lock(&sh->lock);
    controller_init(&thread->ctl, &sh->setup.ctl);
    pthread_mutex_unlock(&sh->lock);
    thread->ports.log = controller_log;

    uint32_t now_ms = 0, lost_since_ms = 0, raw[UNITS] = {0};
    uint16_t out = 0, in = 0, last_in = 0, last_out = 0;
    bool bus_lost = false, first_exchange = true;
    struct timespec next_cycle, cycle_start;
    clock_gettime(CLOCK_MONOTONIC, &next_cycle);
    while (!atomic_load(&sh->stop)) {
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);
        if (hal_exchange(args->hal, out, &in, raw)) {
            if (bus_lost) {
                log_msg("EtherCAT: answering again after %u ms", (unsigned)(now_ms - lost_since_ms));
            }

            bus_lost = false;
            if (first_exchange) {
                log_msg("inputs at start: 0x%04x", in);
            } else {
                log_changes("inputs", 'E', last_in, in, LOG_INPUT_MASK);
            }

            first_exchange = false;
            last_in = in;
            io_read_inputs(&thread->io, in, raw, now_ms);
        } else if (!bus_lost) {
            bus_lost = true;
            lost_since_ms = now_ms;
        } else if (now_ms - lost_since_ms > BUS_LOST_MS) {
            log_msg("EtherCAT: no answer for more than %d ms, outputs off, exit", BUS_LOST_MS);
            hal_exchange(args->hal, 0, &in, raw);
            atomic_store(&sh->exit_code, 1);
            atomic_store(&sh->stop, true);
            break;
        }

        for (int unit = 0; unit < UNITS; unit++) {
            if (atomic_exchange(&sh->stop_drive[unit], false)) {
                io_sensor_stop_request(&thread->io, unit);
            }
        }

        if (now_ms % CTL_CYCLE_MS == 0) {
            controller_cycle(thread, now_ms, now_ms % CTL_SEQUENCE_CYCLE_MS == 0);
        }

        out = io_write_outputs(&thread->io);
        log_changes("outputs", 'A', last_out, out, LOG_OUTPUT_MASK);
        last_out = out;
        shared_progress(sh, WORKER_IO);
        end_cycle(sh, &cycle_start, &next_cycle, now_ms);
        now_ms++;
    }

    if (!bus_lost) {
        hal_exchange(args->hal, 0, &in, raw);
        hal_exchange(args->hal, 0, &in, raw);
    }

    log_msg("I/O: outputs off");

    return NULL;
}
