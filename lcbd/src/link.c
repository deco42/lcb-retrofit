#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "clock.h"
#include "log.h"
#include "workers.h"

#define REOPEN_MS 1000
#define POLL_MS 1
#define FTDI_LATENCY_MS "1" // instead of the default 16 ms

typedef struct {
    Shared *sh;
    int fd;
    bool modem;
    bool dtr;
    LineSupervision sup;
} Line;

// FTDI adapters buffer received bytes for the latency time, ignored if not possible
static void set_ftdi_low_latency(const char *device)
{
    char real_path[PATH_MAX], path[PATH_MAX + 64];
    if (!realpath(device, real_path)) {
        return;
    }

    snprintf(path, sizeof(path), "/sys/bus/usb-serial/devices/%s/latency_timer", basename(real_path));
    FILE *file = fopen(path, "w");
    if (file) {
        fputs(FTDI_LATENCY_MS, file);
        fclose(file);
    }
}

static int open_line(const char *device)
{
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    // 9600 baud 8N1, raw, without flow control, reads return at once
    struct termios settings;
    if (tcgetattr(fd, &settings) == 0) {
        cfmakeraw(&settings);
        cfsetispeed(&settings, B9600);
        cfsetospeed(&settings, B9600);
        settings.c_cflag &= (tcflag_t) ~(CSTOPB | PARENB | CRTSCTS | CSIZE);
        settings.c_cflag |= CS8 | CLOCAL | CREAD;
        settings.c_iflag &= (tcflag_t) ~(IXON | IXOFF | IXANY);
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &settings);
    }

    set_ftdi_low_latency(device);

    return fd;
}

static bool set_modem_lines(int fd, int lines, bool on) { return ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &lines) == 0; }

static void handle_byte(Line *line, uint8_t byte)
{
    Shared *sh = line->sh;
    NlrReply reply;
    pthread_mutex_lock(&sh->lock);
    nlr_receive(&sh->link, byte, &reply);
    int dnc_len = nlr_take_dnc(&sh->link, sh->dnc);
    if (dnc_len) {
        sh->dnc_len = dnc_len;
        sh->dnc_pending = true;
    }

    bool first_contact = !sh->link_contact;
    sh->link_contact = true;
    pthread_mutex_unlock(&sh->lock);

    if (reply.ack && line->modem) { // acknowledge the byte first, then reply
        line->dtr = !line->dtr;
        set_modem_lines(line->fd, TIOCM_DTR, line->dtr);
    }

    if (reply.tx_len && write(line->fd, reply.tx, (size_t)reply.tx_len) != reply.tx_len) {
        log_msg("CNC link: write failed: %s", strerror(errno));
    }

    if (first_contact) {
        log_msg("CNC link: first contact (byte 0x%02x)", byte);
    }

    if (dnc_len) {
        log_msg("CNC link: DNC message of %d bytes received", dnc_len);
    }
}

static void supervise(Line *line)
{
    int status;
    if (!line->modem || ioctl(line->fd, TIOCMGET, &status) != 0) {
        return;
    }

    bool changed;
    LineState before = line->sup.state;
    bool lines_on = line_supervision_step(&line->sup, status & TIOCM_CTS, status & TIOCM_DSR, clock_ms(), &changed);
    if (line->sup.state != before) {
        set_modem_lines(line->fd, TIOCM_DTR | TIOCM_RTS, lines_on);
        line->dtr = lines_on;
        pthread_mutex_lock(&line->sh->lock);
        line->sh->line_state = line->sup.state;
        pthread_mutex_unlock(&line->sh->lock);
    }

    if (changed) {
        log_msg("CNC link: line supervision: %s",
                line->sup.state == LINE_OK ? "CTS/DSR active again" : "CTS and DSR inactive, cycling DTR/RTS");
    }
}

static void serve(Line *line)
{
    uint32_t last_check_ms = clock_ms();
    while (!atomic_load(&line->sh->stop)) {
        shared_progress(line->sh, WORKER_LINK);
        struct pollfd poll_fd = {line->fd, POLLIN, 0};
        int ready = poll(&poll_fd, 1, POLL_MS);
        if (ready < 0 && errno != EINTR) {
            log_msg("CNC link: poll failed: %s", strerror(errno));
            return;
        }

        if (ready > 0 && (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            log_msg("CNC link: device lost");
            return;
        }

        if (ready > 0) {
            uint8_t buf[64];
            ssize_t n = read(line->fd, buf, sizeof(buf));
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                log_msg("CNC link: device lost (%s)", strerror(errno));
                return;
            }

            for (ssize_t i = 0; i < n; i++) {
                handle_byte(line, buf[i]);
            }
        }

        if (clock_ms() != last_check_ms) {
            last_check_ms = clock_ms();
            supervise(line);
        }
    }
}

void *link_thread_main(void *arg)
{
    const LinkThreadArgs *args = arg;
    Shared *sh = args->sh;
    bool missing_logged = false;
    while (!atomic_load(&sh->stop)) {
        shared_progress(sh, WORKER_LINK);
        Line line = {.sh = sh, .fd = open_line(args->device)};
        if (line.fd < 0) {
            if (!missing_logged) {
                log_msg("CNC link: cannot open %s: %s, retrying every second", args->device, strerror(errno));
            }

            missing_logged = true;
            shared_pause(sh, WORKER_LINK, REOPEN_MS);
            continue;
        }

        missing_logged = false;
        line.modem = set_modem_lines(line.fd, TIOCM_DTR | TIOCM_RTS, true);
        line.dtr = true;
        log_msg("CNC link: %s opened, 9600 8N1%s", args->device,
                line.modem ? ", DTR and RTS on" : " (no modem control lines: DTR acknowledge and supervision off)");
        pthread_mutex_lock(&sh->lock);
        sh->link_open = true;
        sh->link_modem = line.modem;
        sh->line_state = LINE_OK; // a newly opened line starts supervised as OK
        pthread_mutex_unlock(&sh->lock);

        serve(&line);

        if (line.modem) {
            set_modem_lines(line.fd, TIOCM_DTR | TIOCM_RTS, false);
        }

        close(line.fd);
        pthread_mutex_lock(&sh->lock);
        sh->link_open = false;
        pthread_mutex_unlock(&sh->lock);
        if (atomic_load(&sh->stop)) {
            break;
        }

        log_msg("CNC link: %s closed, reopening every second", args->device);
        shared_pause(sh, WORKER_LINK, REOPEN_MS);
    }

    return NULL;
}
