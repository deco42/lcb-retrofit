#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "clock.h"
#include "log.h"
#include "sensorproto.h"
#include "workers.h"

#define CONNECT_TIMEOUT_MS 2000
#define RETRY_MS 2000
#define RX_TIMEOUT_MS 1000
#define POLL_MS 20

typedef struct {
    Shared *sh;
    int unit;
    Worker worker;
    const char *name;
    SensorRegs regs;
    Evaluator ev;
    FrameAssembler fa;
    bool ref_requested, stop_when_found, ref_running;
    uint32_t ref_generation; // generation of the start reference being processed
} Client;

static int connect_with_timeout(const char *host, int port, char *err, size_t err_size)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *addr;
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    int result = getaddrinfo(host, service, &hints, &addr);
    if (result) {
        snprintf(err, err_size, "%s", gai_strerror(result));
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(err, err_size, "%s", strerror(errno));
        freeaddrinfo(addr);
        return -1;
    }

    result = connect(fd, addr->ai_addr, addr->ai_addrlen);
    freeaddrinfo(addr);
    if (result < 0 && errno == EINPROGRESS) {
        struct pollfd poll_fd = {fd, POLLOUT, 0};
        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        result = poll(&poll_fd, 1, CONNECT_TIMEOUT_MS);
        if (result == 0) {
            errno = ETIMEDOUT;
            result = -1;
        } else if (result > 0) {
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len);
            errno = socket_error;
            result = socket_error ? -1 : 0;
        }
    }

    if (result < 0) {
        snprintf(err, err_size, "%s", strerror(errno));
        close(fd);
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return fd;
}

static void send_event(const Client *client, int fd, SensorEvent event)
{
    uint8_t buf[SENSOR_CMD_MAX];
    size_t len = sensor_command(&client->regs, event, buf);
    if (send(fd, buf, len, MSG_NOSIGNAL) != (ssize_t)len) {
        log_msg("sensor %s: sending a command failed: %s", client->name, strerror(errno));
    }
}

static void take_commands(Client *client, int fd)
{
    Shared *sh = client->sh;
    SensorEvent cmd[CTL_CMD_MAX];
    pthread_mutex_lock(&sh->lock);
    int n_cmd = sh->sensor_n_cmd[client->unit];
    memcpy(cmd, sh->sensor_cmd[client->unit], (size_t)n_cmd * sizeof(cmd[0]));
    sh->sensor_n_cmd[client->unit] = 0;
    uint32_t generation = sh->ref_generation[client->unit];
    pthread_mutex_unlock(&sh->lock);
    if (fd < 0) {
        return; // dropped while disconnected
    }

    for (int i = 0; i < n_cmd; i++) {
        send_event(client, fd, cmd[i]);
        if (cmd[i] == SENSOR_START_REFERENCE) {
            client->ref_requested = client->stop_when_found = client->ref_running = true;
            client->ref_generation = generation;
        }
    }
}

static void publish_station(Shared *sh, const StationOutput *out)
{
    sh->link.angle = out->angle;
    sh->link.both = out->both;
    sh->link.valid_rear = out->valid_rear;
    sh->link.valid_front = out->valid_front;
    HistoryEntry entry = {.time_s = log_uptime(),
                          .rear = sh->station.leg_angle[UNIT_REAR],
                          .front = sh->station.leg_angle[UNIT_FRONT],
                          .combined = out->angle,
                          .phase = sh->link.phase};
    shared_history_push(sh, &entry);
}

static void handle_frame(Client *client, const uint8_t *frame)
{
    Shared *sh = client->sh;
    if (client->ref_requested) {
        client->ref_requested = false;
        evaluator_restart_reference(&client->ev);
    }

    const EvalResult *res = evaluator_process(&client->ev, frame);
    bool stop = false, referenced = false;
    if (client->ref_running) {
        if (client->stop_when_found && evaluator_die_found(&client->ev)) {
            client->stop_when_found = false;
            stop = true;
        }

        if (evaluator_reference_done(&client->ev)) {
            client->ref_running = false;
            referenced = true;
        }
    }

    if (stop) {
        atomic_store(&sh->stop_drive[client->unit], true);
    }

    pthread_mutex_lock(&sh->lock);
    SensorView *view = &sh->sensor[client->unit];
    view->valid_frame_count++;
    view->have_frame = true;
    memcpy(view->frame, frame, sizeof(view->frame));
    view->res = *res;
    if (referenced) {
        sh->ref_done_generation[client->unit] = client->ref_generation;
    }

    StationOutput station_out;
    if (station_update(&sh->station, client->unit, res->leg_angle, res->valid, &station_out)) {
        publish_station(sh, &station_out);
    }

    pthread_mutex_unlock(&sh->lock);
    if (stop) {
        log_msg("sensor %s: die found, stop drive", client->name);
    }

    if (referenced) {
        log_msg("sensor %s: referenced (die angle %.2f)", client->name, (double)res->die_angle);
    }
}

static bool assemble_frames(Client *client, const uint8_t *data, size_t len)
{
    while (len > 0) {
        FrameStatus status = frame_assemble(&client->fa, &data, &len);
        if (status == FRAME_VALID) {
            handle_frame(client, client->fa.frame);
        } else if (status == FRAME_INVALID) {
            pthread_mutex_lock(&client->sh->lock);
            client->sh->sensor[client->unit].invalid_frame_count++;
            pthread_mutex_unlock(&client->sh->lock);
        } else if (status == FRAME_RESYNC) {
            log_msg("sensor %s: %d invalid frames in a row, resync (reconnect)", client->name, FRAME_RESYNC_INVALID);
            return true;
        }
    }

    return false;
}

static void set_connected(const Client *client, bool connected)
{
    pthread_mutex_lock(&client->sh->lock);
    client->sh->sensor[client->unit].connected = connected;
    pthread_mutex_unlock(&client->sh->lock);
}

static bool run_session(Client *client, int fd)
{
    uint8_t buf[8192];
    uint32_t last_rx_ms = clock_ms();
    frame_assembler_reset(&client->fa);
    send_event(client, fd, SENSOR_CONNECTED);
    while (!atomic_load(&client->sh->stop)) {
        shared_progress(client->sh, client->worker);
        take_commands(client, fd);
        struct pollfd poll_fd = {fd, POLLIN, 0};
        int ready = poll(&poll_fd, 1, POLL_MS);
        if (ready > 0) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                    continue;
                }

                log_msg("sensor %s: disconnected (%s)", client->name,
                        n == 0 ? "closed by the sensor" : strerror(errno));
                return false;
            }

            last_rx_ms = clock_ms();
            if (assemble_frames(client, buf, (size_t)n)) {
                return true;
            }
        } else if (ready < 0 && errno != EINTR) {
            log_msg("sensor %s: poll failed: %s", client->name, strerror(errno));
            return false;
        }

        if (clock_ms() - last_rx_ms > RX_TIMEOUT_MS) {
            log_msg("sensor %s: timeout, no data for %g s", client->name, ms_to_s(RX_TIMEOUT_MS));
            return false;
        }
    }

    return false;
}

void *sensor_thread_main(void *arg)
{
    const SensorThreadArgs *args = arg;
    Shared *sh = args->sh;
    Client client = {.sh = sh, .unit = args->unit, .worker = WORKER_REAR + args->unit, .name = unit_name(args->unit)};
    char err[128];
    bool outage_logged = false;
    pthread_mutex_lock(&sh->lock);
    client.regs = sh->sensor_file.regs[args->unit];
    LegLineParams par = sh->sensor_file.leg_line;
    snprintf(sh->sensor[args->unit].addr, sizeof(sh->sensor[0].addr), "%s:%d", args->host, args->port);
    pthread_mutex_unlock(&sh->lock);
    evaluator_init(&client.ev, &par);

    while (!atomic_load(&sh->stop)) {
        shared_progress(sh, client.worker);
        int fd = connect_with_timeout(args->host, args->port, err, sizeof(err));
        if (fd < 0) {
            if (!outage_logged) {
                log_msg("sensor %s (%s:%d): not reachable: %s, retrying every %g s", client.name, args->host,
                        args->port, err, ms_to_s(RETRY_MS));
            }

            outage_logged = true;
            take_commands(&client, -1);
            shared_pause(sh, client.worker, RETRY_MS);
            continue;
        }

        outage_logged = false;
        log_msg("sensor %s (%s:%d): connected", client.name, args->host, args->port);
        set_connected(&client, true);
        bool resync = run_session(&client, fd);
        close(fd);
        set_connected(&client, false);
        if (!resync) {
            shared_pause(sh, client.worker, RETRY_MS);
        }
    }

    return NULL;
}
