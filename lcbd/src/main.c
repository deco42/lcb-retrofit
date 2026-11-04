#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "log.h"
#include "persist.h"
#include "sim.h"
#include "workers.h"
#include "x87.h"

#define IO_THREAD_PRIORITY 80 // SCHED_FIFO
#define LINK_THREAD_PRIORITY 70
#define MAIN_LOOP_MS 100
#define WATCHDOG_PERIOD_S 1.0
#define WATCHDOG_STALL_S 6
#define SIM_START_POS 1500 // pulses
#define SENSOR_HOST_MAX 64

typedef struct {
    const char *ecat, *com, *web, *data;
    char sensor_host[UNITS][SENSOR_HOST_MAX];
    int sensor_port[UNITS];
    bool sim;
} Options;

static const Options DEFAULT_OPTIONS = {.ecat = "ecat0",
                                        .com = "/dev/ttyNLR",
                                        .web = "0.0.0.0:80",
                                        .data = "/etc/lcbd",
                                        .sensor_host = {SENSOR_DEFAULT_REAR, SENSOR_DEFAULT_FRONT},
                                        .sensor_port = {SENSOR_DEFAULT_PORT, SENSOR_DEFAULT_PORT}};

static void usage(void)
{
    const Options *defaults = &DEFAULT_OPTIONS;
    fprintf(stderr,
            "usage: lcbd [--ecat IFACE] [--com DEVICE] [--sensor IP[:PORT] --sensor IP[:PORT]] [--sim]\n"
            "            [--web [ADDRESS:]PORT | --web 0] [--data DIR]\n"
            "defaults: --ecat %s --com %s --sensor %s:%d --sensor %s:%d\n"
            "          --web %s --data %s\n"
            "--sim: machine model instead of EtherCAT, a line \"E5\" on stdin presses input E5 for %d ms\n",
            defaults->ecat, defaults->com, defaults->sensor_host[UNIT_REAR], defaults->sensor_port[UNIT_REAR],
            defaults->sensor_host[UNIT_FRONT], defaults->sensor_port[UNIT_FRONT], defaults->web, defaults->data,
            SIM_BUTTON_MS);
}

static bool parse_sensor(const char *arg, char *host, size_t size, int *port)
{
    const char *colon = strchr(arg, ':');
    size_t host_len = colon ? (size_t)(colon - arg) : strlen(arg);
    if (host_len == 0 || host_len >= size) {
        return false;
    }

    memcpy(host, arg, host_len);
    host[host_len] = '\0';
    if (colon) {
        char *end;
        long value = strtol(colon + 1, &end, 10);
        if (*end != '\0' || value <= 0 || value > PORT_MAX) {
            return false;
        }

        *port = (int)value;
    }

    return true;
}

static bool parse_options(int argc, char **argv, Options *options)
{
    static const struct option LONG_OPTIONS[] = {{"ecat", required_argument, NULL, 'e'},
                                                 {"com", required_argument, NULL, 'c'},
                                                 {"sensor", required_argument, NULL, 's'},
                                                 {"sim", no_argument, NULL, 'm'},
                                                 {"web", required_argument, NULL, 'w'},
                                                 {"data", required_argument, NULL, 'd'},
                                                 {NULL, 0, NULL, 0}};
    int n_sensors = 0, option;
    *options = DEFAULT_OPTIONS;
    opterr = 0;
    while ((option = getopt_long(argc, argv, "", LONG_OPTIONS, NULL)) != -1) {
        switch (option) {
        case 'e':
            options->ecat = optarg;
            break;
        case 'c':
            options->com = optarg;
            break;
        case 's':
            if (n_sensors >= UNITS ||
                !parse_sensor(optarg, options->sensor_host[n_sensors], sizeof(options->sensor_host[0]),
                              &options->sensor_port[n_sensors])) {
                return false;
            }

            n_sensors++;
            break;
        case 'm':
            options->sim = true;
            break;
        case 'w':
            options->web = optarg;
            break;
        case 'd':
            options->data = optarg;
            break;
        default:
            return false;
        }
    }

    return optind == argc && n_sensors != 1; // --sensor twice or not at all
}

#define CONFIG_TEXT_MAX 8192

static bool read_config_file(const char *dir, const char *name, char *text, size_t size)
{
    if (!data_file_read(dir, name, text, size)) {
        log_msg("configuration: %s/%s missing, using defaults", dir, name);
        return false;
    }

    log_msg("configuration: %s/%s read", dir, name);

    return true;
}

static void load_config(Shared *sh, const char *dir)
{
    static char text[CONFIG_TEXT_MAX];
    setup_file_defaults(&sh->setup);
    sensor_file_defaults(&sh->sensor_file);
    snprintf(sh->data_dir, sizeof(sh->data_dir), "%s", dir);
    if (read_config_file(dir, SETUP_FILE_NAME, text, sizeof(text))) {
        setup_file_parse(&sh->setup, text, false);
    }

    if (read_config_file(dir, SENSOR_FILE_NAME, text, sizeof(text))) {
        sensor_file_parse(&sh->sensor_file, text);
    }

    config_describe(&sh->setup, &sh->sensor_file, text, sizeof(text));
    log_msg("configuration: %s", text);
}

// Threads in start order, the I/O thread last
enum { THREAD_LINK, THREAD_SENSOR, THREAD_WEB = THREAD_SENSOR + UNITS, THREAD_IO, THREADS };

static bool start_thread(const char *name, pthread_t *thread, int priority, void *(*thread_main)(void *), void *arg,
                         bool *rt_missing)
{
    if (priority > 0) {
        pthread_attr_t attr;
        struct sched_param param = {.sched_priority = priority};
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &param);
        int result = pthread_create(thread, &attr, thread_main, arg);
        pthread_attr_destroy(&attr);
        if (result == 0) {
            return true;
        }

        *rt_missing = true;
    }

    int result = pthread_create(thread, NULL, thread_main, arg);
    if (result != 0) {
        log_msg("cannot start the %s thread: %s", name, strerror(result));
    }

    return result == 0;
}

// systemd notification over $NOTIFY_SOCKET (no libsystemd)
static void notify(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || path[0] == '\0') {
        return;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        return;
    }

    memcpy(addr.sun_path, path, path_len);
    if (addr.sun_path[0] == '@') { // abstract socket
        addr.sun_path[0] = '\0';
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }

    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&addr,
           (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len));
    close(fd);
}

static const char *const WORKER_NAMES[WORKERS] = {"I/O cycle", "CNC link", "rear sensor", "front sensor"};

typedef struct {
    unsigned last_progress[WORKERS];
    double changed_s[WORKERS]; // uptime of the last progress
    bool stalled;
} Watchdog;

static void watchdog_check(Watchdog *watchdog, const Shared *sh, double now_s)
{
    if (watchdog->stalled) {
        return;
    }

    for (int i = 0; i < WORKERS; i++) {
        unsigned progress = atomic_load(&sh->progress[i]);
        if (progress != watchdog->last_progress[i]) {
            watchdog->last_progress[i] = progress;
            watchdog->changed_s[i] = now_s;
        } else if (now_s - watchdog->changed_s[i] > WATCHDOG_STALL_S) {
            log_msg("watchdog: %s loop stalled for more than %d s, stopping the systemd watchdog notification",
                    WORKER_NAMES[i], WATCHDOG_STALL_S);
            watchdog->stalled = true;
            return;
        }
    }

    notify("WATCHDOG=1");
}

#define SIM_LINE_MAX 32

static bool sim_stdin(Hal *hal)
{
    static char line[SIM_LINE_MAX]; // the line read so far
    static size_t line_len;
    char buf[64];
    ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));
    if (got <= 0) {
        return false;
    }

    for (ssize_t i = 0; i < got; i++) {
        if (buf[i] != '\n') {
            if (line_len < SIM_LINE_MAX - 1) {
                line[line_len++] = buf[i];
            }

            continue;
        }

        line[line_len] = '\0';
        line_len = 0;
        int input = (line[0] == 'E' || line[0] == 'e') ? atoi(line + 1) : 0;
        if (input >= 1 && input <= IO_CHANNELS) {
            hal_sim_press(hal, input, SIM_BUTTON_MS);
            log_msg("simulation: E%d pressed", input);
        } else if (line[0] != '\0') {
            log_msg("simulation: unknown input line '%s' (expected e.g. E5)", line);
        }
    }

    return true;
}

static void run_until_stop(Shared *sh, Hal *hal, bool sim, const sigset_t *stop_signals)
{
    Watchdog watchdog = {0};
    bool stdin_open = sim;
    double last_watchdog_s = 0;
    struct timespec wait_time = {0, MAIN_LOOP_MS * NS_PER_MS};
    while (!atomic_load(&sh->stop)) {
        if (stdin_open) {
            struct pollfd poll_fd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&poll_fd, 1, 0) > 0 && (poll_fd.revents & (POLLIN | POLLHUP))) {
                stdin_open = sim_stdin(hal);
            }
        }

        siginfo_t signal_info;
        if (sigtimedwait(stop_signals, &signal_info, &wait_time) > 0) {
            log_msg("signal %d: stopping", signal_info.si_signo);
            atomic_store(&sh->stop, true);
        }

        double now_s = log_uptime();
        if (now_s - last_watchdog_s >= WATCHDOG_PERIOD_S) {
            last_watchdog_s = now_s;
            watchdog_check(&watchdog, sh, now_s);
        }
    }
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage();
        return 2;
    }

    // Block the stop signals before the first thread starts, so only main takes them and switches the outputs off
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    // Set before any thread starts
    unsigned short x87_control_word = x87_set_extended_nearest();
    static Shared sh;
    shared_init(&sh);
    if (!log_start()) {
        fprintf(stderr, "lcbd: cannot start the log thread\n");
        return 1;
    }

    log_msg("lcbd %s starting%s", VERSION, options.sim ? " (simulation)" : "");
    if ((x87_control_word & X87_PRECISION_ROUNDING_MASK) != X87_EXTENDED_NEAREST) {
        log_msg("x87 control word was 0x%04x, set to extended precision and round to nearest", x87_control_word);
    }

    load_config(&sh, options.data);
    sh.sim = options.sim;

    int web_fd = -1;
    if (strcmp(options.web, "0") != 0) {
        web_fd = web_listen(options.web);
        if (web_fd < 0) {
            log_msg("service page: cannot listen on %s: %s (page off)", options.web, strerror(errno));
        } else {
            log_msg("service page on %s", options.web);
        }
    }

    char msg[2048];
    Hal *hal = options.sim ? hal_open_sim(SIM_START_POS) : hal_open_ecat(options.ecat, msg, sizeof(msg));
    if (!hal) {
        log_msg("EtherCAT on %s: %s", options.ecat, options.sim ? "out of memory" : msg);
        log_stop();
        return 1;
    }

    hal_describe(hal, msg, sizeof(msg));
    for (char *p = strchr(msg, '\n'); p; p = strchr(p, '\n')) { // one log line
        *p = p[1] ? ',' : '\0';
    }

    log_msg("process image: %s", msg);

    bool rt_missing = mlockall(MCL_CURRENT | MCL_FUTURE) != 0;
    signal(SIGPIPE, SIG_IGN);

    IoThreadArgs io_args = {&sh, hal};
    LinkThreadArgs link_args = {&sh, options.com};
    SensorThreadArgs sensor_args[UNITS];
    WebThreadArgs web_args = {&sh, web_fd};
    pthread_t threads[THREADS];
    bool started[THREADS] = {false};
    started[THREAD_LINK] =
        start_thread("link", &threads[THREAD_LINK], LINK_THREAD_PRIORITY, link_thread_main, &link_args, &rt_missing);
    for (int unit = 0; unit < UNITS; unit++) {
        sensor_args[unit] = (SensorThreadArgs){
            .sh = &sh, .unit = unit, .host = options.sensor_host[unit], .port = options.sensor_port[unit]};
        started[THREAD_SENSOR + unit] = start_thread("sensor", &threads[THREAD_SENSOR + unit], 0, sensor_thread_main,
                                                     &sensor_args[unit], &rt_missing);
    }

    if (web_fd >= 0) {
        started[THREAD_WEB] = start_thread("web", &threads[THREAD_WEB], 0, web_thread_main, &web_args, &rt_missing);
    }

    started[THREAD_IO] =
        start_thread("I/O", &threads[THREAD_IO], IO_THREAD_PRIORITY, io_thread_main, &io_args, &rt_missing);
    bool all_started = true;
    for (int i = 0; i < THREADS; i++) {
        all_started &= started[i] || (i == THREAD_WEB && web_fd < 0);
    }

    if (rt_missing) {
        log_msg("hint: no real-time rights (mlockall/SCHED_FIFO refused), running without priorities, "
                "start as root or with LimitRTPRIO/LimitMEMLOCK");
    }

    if (all_started) {
        run_until_stop(&sh, hal, options.sim, &stop_signals);
    } else {
        atomic_store(&sh.exit_code, 1);
        atomic_store(&sh.stop, true);
    }

    for (int i = THREADS - 1; i >= 0; i--) {
        if (started[i]) {
            pthread_join(threads[i], NULL);
        }
    }

    hal_close(hal);
    int exit_code = atomic_load(&sh.exit_code);
    log_msg("lcbd stopped (exit code %d)", exit_code);
    log_stop();

    return exit_code;
}
