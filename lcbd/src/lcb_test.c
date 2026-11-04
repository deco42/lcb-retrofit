// Commands (line + Enter):
//   7, 8      lamp A7 / A8 on/off
//   5         cylinder down (A5 on, A6 off)
//   6         cylinder up (A6 on, A5 off), only with E1 = 0
//   z         cylinder outputs off
//   tN MS     jog drive output A1..A4 for MS milliseconds (max. 500),
//             A2/A4 (back) also stop at the reference switch (E2/E3 = 0)
//   0         all outputs off
//   q         quit (all off)
// Interlocks: never A1+A2 or A3+A4 together, never A5+A6 together, with --read-only no outputs at all. If the bus
// fails, all outputs go off (the EL2809 also switches off after 100 ms by itself)
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "ecat.h"
#include "io.h"

#define RT_PRIORITY 80 // SCHED_FIFO
#define CYCLE_NS NS_PER_MS
#define JOG_MAX_MS 500
#define BUS_FAILURE_S 0.05 // outputs off after this long without an answer
#define COUNTER_PERIOD_S 1.0

// Indexed by channel number
static const char *const INPUT_NAME[IO_CHANNELS + 1] = {"",
                                                        "backgauge in swivel zone (1 = cylinder up locked)",
                                                        "reference front B153 (0 = at reference)",
                                                        "reference rear B154 (0 = at reference)",
                                                        "cylinder down",
                                                        "button S156 reference",
                                                        "button S157 cylinder",
                                                        "new die position (PLC)",
                                                        "CNC measuring system active",
                                                        "CNC measuring system inactive",
                                                        "count pulses front",
                                                        "-",
                                                        "-",
                                                        "-",
                                                        "count pulses rear",
                                                        "-",
                                                        "-"};
static const char *const OUTPUT_NAME[IO_CHANNELS + 1] = {"",
                                                         "drive front forward",
                                                         "drive front back",
                                                         "drive rear forward",
                                                         "drive rear back",
                                                         "cylinder down",
                                                         "cylinder up",
                                                         "lamp rear H121",
                                                         "lamp front H120",
                                                         "-",
                                                         "-",
                                                         "-",
                                                         "-",
                                                         "-",
                                                         "-",
                                                         "-",
                                                         "-"};

static volatile sig_atomic_t stop;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop = 1;
}

typedef struct {
    uint16_t in, out;
    int jog_output; // output being jogged, 0 = none
    double jog_end_s;
    bool read_only;
} State;

static void command(State *state, const char *line)
{
    int output, ms;
    if (line[0] == 'q') {
        stop = 1;
    } else if (line[0] == '0') {
        state->out = 0;
        state->jog_output = 0;
    } else if (state->read_only && line[0] != '\0') {
        puts("          read only: command ignored");
    } else if (strcmp(line, "7") == 0 || strcmp(line, "8") == 0) {
        state->out ^= (uint16_t)BIT(line[0] - '0'); // OUT_LAMP_REAR, OUT_LAMP_FRONT
    } else if (strcmp(line, "5") == 0) {
        state->out = (uint16_t)((state->out & ~BIT(OUT_CYLINDER_UP)) | BIT(OUT_CYLINDER_DOWN));
    } else if (strcmp(line, "6") == 0) {
        if (state->in & BIT(IN_SWIVEL_ZONE)) {
            puts("          locked: E1 = 1 (backgauge in the swivel zone)");
        } else {
            state->out = (uint16_t)((state->out & ~BIT(OUT_CYLINDER_DOWN)) | BIT(OUT_CYLINDER_UP));
        }
    } else if (strcmp(line, "z") == 0) {
        state->out &= (uint16_t) ~(BIT(OUT_CYLINDER_DOWN) | BIT(OUT_CYLINDER_UP));
    } else if (sscanf(line, "t%d %d", &output, &ms) == 2 && output >= OUT_FRONT_FORWARD && output <= OUT_REAR_BACK &&
               ms > 0) {
        int other = output % 2 ? output + 1 : output - 1; // forward (odd) and back (even) of the same drive
        if (state->jog_output || (state->out & BIT(other))) {
            puts("          locked: the other drive output is on");
        } else {
            state->jog_output = output;
            state->jog_end_s = clock_s() + ms_to_s(ms > JOG_MAX_MS ? JOG_MAX_MS : ms);
            state->out |= (uint16_t)BIT(output);
        }
    } else if (line[0] != '\0') {
        puts("          commands: 5 6 7 8 z tN MS 0 q");
    }
}

static void print_changes(const char *kind, char letter, uint16_t previous, uint16_t current, const char *const *names,
                          double time_s)
{
    for (int channel = 1; channel <= IO_CHANNELS; channel++) {
        if ((previous ^ current) & BIT(channel)) {
            printf("%8.3f  %s %c%-2d %s  %s\n", time_s, kind, letter, channel, current & BIT(channel) ? "1" : "0",
                   names[channel]);
        }
    }
}

int main(int argc, char **argv)
{
    char msg[2048], line[128];
    int line_len = 0, bus_errors = 0;
    State state = {0};
    uint16_t last_in = 0, last_out = 0;
    uint32_t raw[UNITS] = {0}, raw_start[UNITS] = {0}, last_raw[UNITS] = {0};
    double longest_cycle_s = 0, last_counter_s = 0, bus_lost_s = 0;
    bool first = true;
    struct timespec next_cycle;
    struct sched_param param = {.sched_priority = RT_PRIORITY};

    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--read-only") != 0)) {
        fprintf(stderr, "usage: sudo %s <network port, e.g. ecat0> [--read-only]\n", argv[0]);
        return 2;
    }

    state.read_only = argc == 3;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) || sched_setscheduler(0, SCHED_FIFO, &param)) {
        fprintf(stderr, "hint: no real-time rights (%s), the display is valid anyway\n", strerror(errno));
    }

    Ecat *ecat = ecat_open(argv[1], msg, sizeof(msg));
    if (!ecat) {
        fprintf(stderr, "EtherCAT: %s\n", msg);
        return 1;
    }

    ecat_describe(ecat, msg, sizeof(msg));
    printf("EtherCAT running:\n%s%s\n", msg,
           state.read_only ? "READ ONLY: no outputs.\n" : "commands: 5 6 7 8 z tN MS 0 q\n");
    double start_s = clock_s();
    clock_gettime(CLOCK_MONOTONIC, &next_cycle);

    while (!stop) {
        double now_s = clock_s(), time_s = now_s - start_s;
        struct pollfd poll_fd = {STDIN_FILENO, POLLIN, 0};

        if (state.jog_output &&
            (now_s >= state.jog_end_s || (state.jog_output == OUT_FRONT_BACK && !(state.in & BIT(IN_REF_FRONT))) ||
             (state.jog_output == OUT_REAR_BACK && !(state.in & BIT(IN_REF_REAR))))) {
            state.out &= (uint16_t)~BIT(state.jog_output);
            state.jog_output = 0;
        }

        if (!ecat_exchange(ecat, state.out, &state.in, raw)) {
            bus_errors++;
            if (!bus_lost_s) {
                bus_lost_s = now_s;
            }

            if (now_s - bus_lost_s > BUS_FAILURE_S && state.out) {
                state.out = 0;
                state.jog_output = 0;
                printf("%8.3f  BUS FAILURE: all outputs off\n", time_s);
            }
        } else if (bus_lost_s) {
            printf("%8.3f  bus back (%.0f ms lost)\n", time_s, (now_s - bus_lost_s) * MS_PER_S);
            bus_lost_s = 0;
        }

        if (first && !bus_lost_s) {
            first = false;
            raw_start[UNIT_REAR] = last_raw[UNIT_REAR] = raw[UNIT_REAR];
            raw_start[UNIT_FRONT] = last_raw[UNIT_FRONT] = raw[UNIT_FRONT];
            last_in = state.in;
            for (int input = 1; input <= IN_CNC_MEASURING_OFF; input++) { // the switch and button inputs
                printf("          E%-2d %d  %s\n", input, (state.in & BIT(input)) != 0, INPUT_NAME[input]);
            }
        }

        print_changes("in ", 'E', last_in, state.in, INPUT_NAME, time_s);
        last_in = state.in;
        print_changes("out", 'A', last_out, state.out, OUTPUT_NAME, time_s);
        last_out = state.out;
        if (now_s - last_counter_s >= COUNTER_PERIOD_S &&
            (raw[UNIT_REAR] != last_raw[UNIT_REAR] || raw[UNIT_FRONT] != last_raw[UNIT_FRONT])) {
            printf("%8.3f  counter rear %u (+%u)  front %u (+%u)  [pulses since start]\n", time_s,
                   raw[UNIT_REAR] - raw_start[UNIT_REAR], raw[UNIT_REAR] - last_raw[UNIT_REAR],
                   raw[UNIT_FRONT] - raw_start[UNIT_FRONT], raw[UNIT_FRONT] - last_raw[UNIT_FRONT]);
            last_raw[UNIT_REAR] = raw[UNIT_REAR];
            last_raw[UNIT_FRONT] = raw[UNIT_FRONT];
            last_counter_s = now_s;
        }

        while (poll(&poll_fd, 1, 0) > 0 && read(STDIN_FILENO, line + line_len, 1) == 1) {
            if (line[line_len] != '\n') {
                if (line_len < (int)sizeof(line) - 2) {
                    line_len++;
                }

                continue;
            }

            line[line_len] = '\0';
            line_len = 0;
            command(&state, line);
        }

        double cycle_s = clock_s() - now_s;
        if (cycle_s > longest_cycle_s) {
            longest_cycle_s = cycle_s;
        }

        fflush(stdout);
        clock_add_ns(&next_cycle, CYCLE_NS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_cycle, NULL);
    }

    ecat_close(ecat);
    printf("end, outputs off. longest cycle %.2f ms, bus errors %d\n", longest_cycle_s * MS_PER_S, bus_errors);

    return 0;
}
