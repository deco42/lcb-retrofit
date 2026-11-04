#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "httputil.h"
#include "log.h"
#include "persist.h"
#include "web_assets.h"
#include "workers.h"

#define REQUEST_MAX 65536 // head and body of one request
#define RECEIVE_TIMEOUT_S 2
#define ACCEPT_TIMEOUT_MS 200 // how often the thread looks at the stop flag
#define LISTEN_BACKLOG 8
#define ANY_ADDRESS "0.0.0.0"
#define INDEX_PATH "/index.html" // also served for "/"
#define SERVICE_USER "service"
#define PASSWORD_FILE_NAME "web.pass" // without it the page is read only
#define PASSWORD_MAX 200
#define DIE_TEXT_MAX (DIE_MAX * 64) // die.dat, a line is shorter than 64 bytes
#define REPLY_MAX 512

typedef enum {
    HTTP_OK = 200,
    HTTP_BAD_REQUEST = 400,
    HTTP_UNAUTHORIZED = 401,
    HTTP_FORBIDDEN = 403,
    HTTP_NOT_FOUND = 404,
} HttpStatus;

typedef struct {
    Io io;
    Controller ctl;
    NlrLink link;
    bool link_open, link_contact, link_modem;
    LineState line_state;
    Station station;
    SensorView sensor[UNITS];
    unsigned cycle_us, cycle_max_us, ctl_cycle_us;
    bool sim;
} Snapshot;

typedef struct {
    Shared *sh;
    int fd;
    HttpRequest req;
    const char *body;
} Client;

typedef struct {
    double x; // record x / FRAME_X_SCALE
    int y, quality;
} ProfilePoint;

typedef struct {
    const char *method, *path;
    void (*handle)(Client *client);
} Route;

static void take_snapshot(Shared *sh, Snapshot *snap)
{
    pthread_mutex_lock(&sh->lock);
    snap->io = sh->io;
    snap->ctl = sh->ctl;
    snap->link = sh->link;
    snap->link_open = sh->link_open;
    snap->link_contact = sh->link_contact;
    snap->link_modem = sh->link_modem;
    snap->line_state = sh->line_state;
    snap->station = sh->station;
    memcpy(snap->sensor, sh->sensor, sizeof(snap->sensor));
    snap->cycle_us = atomic_load(&sh->cycle_us);
    snap->cycle_max_us = atomic_load(&sh->cycle_max_us);
    snap->ctl_cycle_us = atomic_load(&sh->ctl_cycle_us);
    snap->sim = sh->sim;
    pthread_mutex_unlock(&sh->lock);
}

static bool save_data_file(const Shared *sh, const char *name, const char *text, size_t len, const char *saved,
                           char *reply, size_t size)
{
    char err[256] = "";
    bool permanent;
    if (!data_file_save(sh->data_dir, name, text, len, &permanent, err, sizeof(err))) {
        snprintf(reply, size, "refused: %s", err);
        return false;
    }

    snprintf(reply, size, "%s (%s)%s%s", saved, permanent ? "survives a power cycle" : "lost at the next power cycle",
             err[0] ? ": " : "", err);

    return true;
}

static void send_all(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t sent = send(fd, data, len, MSG_NOSIGNAL);
        if (sent <= 0) {
            return;
        }

        data += sent;
        len -= (size_t)sent;
    }
}

static void respond(Client *client, HttpStatus status, const char *type, const char *body, size_t len)
{
    const char *text = status == HTTP_OK             ? "OK"
                       : status == HTTP_UNAUTHORIZED ? "Unauthorized"
                       : status == HTTP_FORBIDDEN    ? "Forbidden"
                       : status == HTTP_NOT_FOUND    ? "Not Found"
                                                     : "Bad Request";
    char head[512];
    int head_len = snprintf(head, sizeof(head),
                            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                            "Connection: close\r\n\r\n",
                            status, text, type, len);
    send_all(client->fd, head, (size_t)head_len);
    send_all(client->fd, body, len);
}

static void respond_text(Client *client, HttpStatus status, const char *text)
{
    respond(client, status, "text/plain", text, strlen(text));
}

static void respond_buf(Client *client, const char *type, Buf *out)
{
    if (out->failed) {
        respond_text(client, HTTP_BAD_REQUEST, "out of memory");
    } else {
        respond(client, HTTP_OK, type, out->data, out->len);
    }

    buf_free(out);
}

static void respond_result(Client *client, HttpStatus status, bool ok, const char *text)
{
    Buf out = {0};
    buf_printf(&out, "{\"ok\":%s,\"text\":", ok ? "true" : "false");
    buf_json_str(&out, text);
    buf_puts(&out, "}");
    respond(client, status, "application/json", out.data, out.len);
    buf_free(&out);
}

static void json_bool(Buf *out, const char *key, bool value)
{
    buf_printf(out, "\"%s\":%s,", key, value ? "true" : "false");
}

static void json_int(Buf *out, const char *key, long value) { buf_printf(out, "\"%s\":%ld,", key, value); }

static void json_num(Buf *out, const char *key, double value)
{
    buf_printf(out, "\"%s\":", key);
    buf_json_num(out, value);
    buf_puts(out, ",");
}

static void json_str(Buf *out, const char *key, const char *value)
{
    buf_printf(out, "\"%s\":", key);
    buf_json_str(out, value);
    buf_puts(out, ",");
}

static void json_end(Buf *out, char close)
{
    if (out->len && out->data[out->len - 1] == ',') {
        out->data[--out->len] = '\0';
    }

    buf_put(out, &close, 1);
}

static void json_eval_result(Buf *out, const EvalResult *res)
{
    buf_puts(out, "{");
    json_num(out, "leg_angle", res->leg_angle);
    json_num(out, "die_angle", res->die_angle);
    json_num(out, "ref_x", res->ref.x);
    json_num(out, "ref_y", res->ref.y);
    json_bool(out, "done", res->done);
    json_int(out, "candidates", res->candidates);
    json_int(out, "good_refs", res->good_refs);
    json_int(out, "die_points", res->die_points);
    json_int(out, "die_fits", res->die_fits);
    json_int(out, "leg_points", res->leg_points);
    json_int(out, "filt_last", res->profile_last);
    json_bool(out, "ref_frame", res->ref_frame);
    json_num(out, "section1", res->section[0]);
    json_num(out, "section2", res->section[1]);
    json_num(out, "section3", res->section[2]);
    json_num(out, "full_line", res->full_line);
    json_num(out, "line_angle", res->line_angle);
    json_bool(out, "valid", res->valid);
    json_end(out, '}');
}

static void json_ctl_setup(Buf *out, const CtlSetup *ctl_setup)
{
    buf_puts(out, "{");
    json_str(out, "move_method", move_method_name(ctl_setup->move_method));
    json_bool(out, "move_down_start", ctl_setup->move_down_start);
    json_bool(out, "secure_angle", ctl_setup->secure_angle);
    json_bool(out, "use_turn", ctl_setup->use_turn);
    json_end(out, '}');
}

static void json_io(Buf *out, const Io *io)
{
    const Unit *rear = &io->unit[UNIT_REAR], *front = &io->unit[UNIT_FRONT];
    buf_puts(out, "\"io\":{");
    json_int(out, "in", io->inputs);
    json_int(out, "out", io->outputs);
    buf_printf(out, "\"pos\":[%d,%d],\"raw\":[%u,%u],\"target\":[%d,%d],\"drive\":[", (int)rear->pos, (int)front->pos,
               (unsigned)io->raw[UNIT_REAR], (unsigned)io->raw[UNIT_FRONT], (int)rear->target, (int)front->target);
    buf_json_str(out, io_drive_state_name(rear->state));
    buf_puts(out, ",");
    buf_json_str(out, io_drive_state_name(front->state));
    buf_puts(out, "]},");
}

static void json_controller(Buf *out, const Snapshot *snap)
{
    const Controller *ctl = &snap->ctl;
    buf_puts(out, "\"ctl\":{");
    json_str(out, "state", controller_state_name(ctl));
    json_int(out, "error", ctl->error);
    json_str(out, "error_text", controller_error_text(ctl->error));
    json_int(out, "ctl_cycle_us", snap->ctl_cycle_us);
    json_int(out, "io_cycle_us", snap->cycle_us);
    json_int(out, "io_cycle_max_us", snap->cycle_max_us);
    json_str(out, "seq_reference", sequence_state_name(&ctl->ref));
    json_str(out, "seq_cylinder", sequence_state_name(&ctl->cylinder));
    json_str(out, "seq_die", sequence_state_name(&ctl->die));
    json_bool(out, "cylinder_up", ctl->cylinder_up);
    json_bool(out, "referenced", ctl->referenced);
    json_bool(out, "scanning", ctl->scanning);
    json_bool(out, "encoder_used", ctl->encoder_used);
    json_bool(out, "leg_check", ctl->setup.secure_angle);
    json_int(out, "control_angle", ctl->control_angle);
    json_int(out, "measure_type", ctl->measure_type);
    json_int(out, "measure_this_bend", ctl->measure_this_bend);
    json_int(out, "dies", ctl->n_dies);
    buf_puts(out, "\"setup\":");
    json_ctl_setup(out, &ctl->setup);
    buf_puts(out, "},");
}

static void json_link(Buf *out, const Snapshot *snap)
{
    const NlrLink *link = &snap->link;
    char dnc[NLR_DNC_MAX + 1];
    memcpy(dnc, link->dnc, (size_t)link->dnc_len);
    dnc[link->dnc_len] = '\0';
    buf_puts(out, "\"link\":{");
    json_bool(out, "open", snap->link_open);
    json_bool(out, "contact", snap->link_contact);
    json_bool(out, "modem", snap->link_modem);
    json_str(out, "supervision", snap->line_state == LINE_OK ? "ok" : "CTS/DSR inactive");
    json_int(out, "last_rx", link->last_rx);
    json_int(out, "last_tx", link->last_tx);
    json_int(out, "last_cmd", link->last_cmd);
    json_int(out, "group", link->last_group);
    json_str(out, "parser", nlr_parser_state_name(link->state));
    json_int(out, "block", link->block);
    json_int(out, "pos", link->state == NLR_DNC_DATA ? (long)link->dnc_received : link->pos);
    json_int(out, "rx_count", (long)link->rx_count);
    json_int(out, "phase", link->phase);
    json_num(out, "set_angle", link->set_angle_x10 / 10.0);
    json_num(out, "angle", link->angle);
    json_int(out, "raw", link->raw_angle);
    json_int(out, "p20", link->param20);
    json_int(out, "p28", link->param28);
    json_int(out, "p30", link->param30);
    json_bool(out, "referenced", link->referenced);
    json_bool(out, "encoder_used", link->encoder_used);
    json_bool(out, "leg_check", link->leg_check);
    json_str(out, "dnc_text", dnc);
    json_int(out, "dnc_count", link->dnc_count);
    json_int(out, "errors", link->error_count);
    json_int(out, "springback", link->springback_query_count);
    json_end(out, '}');
    buf_puts(out, ",");
}

static void json_sensors(Buf *out, const SensorView sensor[UNITS])
{
    buf_puts(out, "\"sensors\":[");
    for (int unit = 0; unit < UNITS; unit++) {
        const SensorView *view = &sensor[unit];
        buf_puts(out, "{");
        json_str(out, "addr", view->addr);
        json_bool(out, "connected", view->connected);
        json_int(out, "frames", (long)(view->valid_frame_count + view->invalid_frame_count));
        json_int(out, "valid_frames", (long)view->valid_frame_count);
        json_int(out, "invalid_frames", (long)view->invalid_frame_count);
        json_int(out, "phase", view->phase);
        buf_puts(out, "\"res\":");
        json_eval_result(out, &view->res);
        buf_puts(out, "},");
    }

    json_end(out, ']');
    buf_puts(out, ",");
}

static void json_station(Buf *out, const Snapshot *snap)
{
    const Station *st = &snap->station;
    buf_puts(out, "\"station\":{");
    json_bool(out, "both", st->both);
    json_bool(out, "valid_rear", st->valid[UNIT_REAR]);
    json_bool(out, "valid_front", st->valid[UNIT_FRONT]);
    json_num(out, "rear", st->leg_angle[UNIT_REAR]);
    json_num(out, "front", st->leg_angle[UNIT_FRONT]);
    json_num(out, "combined", snap->link.angle);
    json_num(out, "held", st->held_angle);
    json_int(out, "pairs", st->pair_count);
    json_end(out, '}');
}

static void get_status(Client *client)
{
    static Snapshot snap; // large, and only the web thread uses it
    take_snapshot(client->sh, &snap);
    Buf out = {0};
    buf_puts(&out, "{");
    json_str(&out, "version", VERSION);
    json_num(&out, "uptime", log_uptime());
    json_bool(&out, "sim", snap.sim);
    json_io(&out, &snap.io);
    json_controller(&out, &snap);
    json_link(&out, &snap);
    json_sensors(&out, snap.sensor);
    json_station(&out, &snap);
    json_end(&out, '}');
    respond_buf(client, "application/json", &out);
}

static ProfilePoint profile_point(const SensorView *view, int i)
{
    ProfilePoint point = {0};
    if (view->have_frame) {
        const uint8_t *record = frame_record(view->frame, i);
        point.x = record_x(record) / FRAME_X_SCALE;
        point.y = record_y(record);
        point.quality = record_quality(record);
    }

    return point;
}

static void get_profile(Client *client)
{
    char sensor[8]; // "s=1" is the front sensor, the rear one otherwise
    int unit =
        http_form_value(client->req.query, "s", sensor, sizeof(sensor)) && sensor[0] == '1' ? UNIT_FRONT : UNIT_REAR;
    static SensorView view;
    pthread_mutex_lock(&client->sh->lock);
    view = client->sh->sensor[unit];
    pthread_mutex_unlock(&client->sh->lock);

    Buf out = {0};
    buf_printf(&out, "{\"sensor\":%d,\"points\":[", unit);
    for (int i = 0; i < FRAME_RECORDS; i++) {
        ProfilePoint point = profile_point(&view, i);
        if (point.x != 0 || point.y || point.quality) { // the unused records are all zero
            buf_printf(&out, "[%.2f,%d,%d],", point.x, point.y, point.quality);
        }
    }

    json_end(&out, ']');
    buf_puts(&out, ",\"res\":");
    json_eval_result(&out, &view.res);
    json_end(&out, '}');
    respond_buf(client, "application/json", &out);
}

static int copy_history(Shared *sh, HistoryEntry entries[HISTORY_MAX])
{
    pthread_mutex_lock(&sh->lock);
    int n_entries = shared_history_copy(sh, entries);
    pthread_mutex_unlock(&sh->lock);

    return n_entries;
}

static void get_history(Client *client)
{
    static HistoryEntry entries[HISTORY_MAX];
    int n_entries = copy_history(client->sh, entries);
    Buf out = {0};
    buf_puts(&out, "{\"entries\":[");
    for (int i = 0; i < n_entries; i++) {
        const HistoryEntry *entry = &entries[i];
        buf_printf(&out, "[%.3f,", entry->time_s);
        buf_json_num(&out, entry->rear);
        buf_puts(&out, ",");
        buf_json_num(&out, entry->front);
        buf_puts(&out, ",");
        buf_json_num(&out, entry->combined);
        buf_printf(&out, ",%d],", entry->phase);
    }

    json_end(&out, ']');
    json_end(&out, '}');
    respond_buf(client, "application/json", &out);
}

static void get_log(Client *client)
{
    static char lines[LOG_RECENT_MAX][LOG_LINE_MAX];
    int n_lines = log_recent(lines);
    Buf out = {0};
    buf_puts(&out, "{\"lines\":[");
    for (int i = 0; i < n_lines; i++) {
        buf_json_str(&out, lines[i]);
        buf_puts(&out, ",");
    }

    json_end(&out, ']');
    json_end(&out, '}');
    respond_buf(client, "application/json", &out);
}

static void json_setup_file(Buf *out, const SetupFile *setup)
{
    buf_puts(out, "{");
    json_str(out, "MV_METHOD", move_method_name(setup->ctl.move_method));
    json_bool(out, "MOVE_DOWN_START", setup->ctl.move_down_start);
    json_bool(out, "USE_SECURE_ANGLE", setup->ctl.secure_angle);
    json_int(out, "SB_TIMEOUT", setup->sb_timeout_s);
    json_int(out, "SB_MEANTILT", setup->sb_meantilt);
    json_int(out, "LE_POINTS_FIXL3", setup->leg_line.last_section_points);
    json_int(out, "LE_POINTS_MIN", setup->leg_line.min_points);
    json_int(out, "LE_COVER", leg_line_cover_percent(&setup->leg_line));
    // %g as in the file: 0.4, not the float's 0.400000006
    buf_printf(out, "\"LE_TOLERANCE\":%g,", (double)setup->leg_line.tolerance);
    json_str(out, "LE_LINE_MODE", leg_line_mode_name(&setup->leg_line));
    json_end(out, '}');
}

// The only keys a POST may change
static const char *const SETUP_FORM_KEYS[] = {"MV_METHOD",    "MOVE_DOWN_START", "USE_SECURE_ANGLE", "SB_TIMEOUT",
                                              "SB_MEANTILT",  "LE_POINTS_FIXL3", "LE_POINTS_MIN",    "LE_COVER",
                                              "LE_TOLERANCE", "LE_LINE_MODE"};

static void get_setup(Client *client)
{
    SetupFile setup;
    CtlSetup active;
    pthread_mutex_lock(&client->sh->lock);
    setup = client->sh->setup;
    active = client->sh->ctl.setup;
    pthread_mutex_unlock(&client->sh->lock);

    Buf out = {0};
    buf_puts(&out, "{\"file\":");
    json_setup_file(&out, &setup);
    buf_puts(&out, ",\"active\":");
    json_ctl_setup(&out, &active);
    json_end(&out, '}');
    respond_buf(client, "application/json", &out);
}

static const char *read_setup_form(Client *client, SetupFile *setup)
{
    Buf lines = {0};
    for (size_t i = 0; i < sizeof(SETUP_FORM_KEYS) / sizeof(SETUP_FORM_KEYS[0]); i++) {
        char value[64];
        if (!http_form_value(client->body, SETUP_FORM_KEYS[i], value, sizeof(value))) {
            continue;
        }

        if (strpbrk(value, "\r\n")) {
            buf_free(&lines);
            return SETUP_FORM_KEYS[i];
        }

        buf_printf(&lines, "%s=%s\n", SETUP_FORM_KEYS[i], value);
    }

    pthread_mutex_lock(&client->sh->lock);
    *setup = client->sh->setup;
    pthread_mutex_unlock(&client->sh->lock);
    setup_file_parse(setup, lines.data ? lines.data : "", true);
    buf_free(&lines);

    return NULL;
}

static void post_setup(Client *client)
{
    SetupFile setup;
    char text[1024], reply[REPLY_MAX];
    const char *bad_key = read_setup_form(client, &setup);
    if (bad_key) {
        snprintf(reply, sizeof(reply), "refused: line break in %s", bad_key);
        log_msg("service: save setup: %s", reply);
        respond_result(client, HTTP_OK, false, reply);
        return;
    }

    size_t len = setup_file_format(&setup, text, sizeof(text));
    bool saved = save_data_file(client->sh, SETUP_FILE_NAME, text, len, SETUP_FILE_NAME " saved", reply, sizeof(reply));
    if (saved) {
        pthread_mutex_lock(&client->sh->lock);
        client->sh->setup = setup;
        pthread_mutex_unlock(&client->sh->lock);

        ServiceRequest request = {.kind = SERVICE_SETUP, .setup = setup.ctl};
        char applied[SERVICE_REPLY_MAX];
        bool ok = shared_request(client->sh, &request, applied, sizeof(applied));
        size_t reply_len = strlen(reply);
        snprintf(reply + reply_len, sizeof(reply) - reply_len, ", %s", ok ? applied : "not applied");
    }

    log_msg("service: save setup: %s", reply);
    respond_result(client, HTTP_OK, saved, reply);
}

static void get_dies(Client *client)
{
    static char text[DIE_TEXT_MAX];
    static DieEntry dies[DIE_MAX];
    int n_dies =
        data_file_read(client->sh->data_dir, DIE_FILE_NAME, text, sizeof(text)) ? dies_parse(text, dies, DIE_MAX) : 0;
    Buf out = {0};
    buf_puts(&out, "{\"dies\":[");
    for (int i = 0; i < n_dies; i++) {
        buf_puts(&out, "{");
        json_str(&out, "name", dies[i].name);
        json_int(&out, "rear", dies[i].rear);
        json_int(&out, "front", dies[i].front);
        json_end(&out, '}');
        buf_puts(&out, ",");
    }

    json_end(&out, ']');
    json_end(&out, '}');
    respond_buf(client, "application/json", &out);
}

static void post_dies(Client *client)
{
    static char text[DIE_TEXT_MAX];
    static DieEntry dies[DIE_MAX];
    char action[16] = "", reply[REPLY_MAX];
    http_form_value(client->body, "action", action, sizeof(action));
    if (!http_form_value(client->body, "dies", text, sizeof(text))) {
        text[0] = '\0';
    }

    int n_dies = dies_parse(text, dies, DIE_MAX);
    bool send = strcmp(action, "send") == 0, ok = false;
    if (strcmp(action, "save") == 0) {
        size_t len = dies_format(dies, n_dies, text, sizeof(text));
        char saved[REPLY_MAX];
        snprintf(saved, sizeof(saved), DIE_FILE_NAME " saved, %d dies", n_dies);
        ok = save_data_file(client->sh, DIE_FILE_NAME, text, len, saved, reply, sizeof(reply));
    } else if (send || strcmp(action, "list") == 0) {
        static ServiceRequest request; // large (die table), and only the web thread uses it
        memset(&request, 0, sizeof(request));
        request.kind = send ? SERVICE_DIES : SERVICE_DIE_LIST;
        memcpy(request.dies, dies, (size_t)n_dies * sizeof(dies[0]));
        request.n_dies = n_dies;
        ok = shared_request(client->sh, &request, reply, sizeof(reply));
    } else {
        snprintf(reply, sizeof(reply), "refused: unknown action");
    }

    log_msg("service: die table %s: %s", action, reply);
    respond_result(client, HTTP_OK, ok, reply);
}

static void dl_points(Client *client)
{
    static SensorView view[UNITS];
    pthread_mutex_lock(&client->sh->lock);
    memcpy(view, client->sh->sensor, sizeof(view));
    pthread_mutex_unlock(&client->sh->lock);

    Buf out = {0};
    buf_puts(&out, "BACKX_0, BACKZ_0, FRONTX_0, FRONTZ_0 BACKX_1, BACKZ_1, FRONTX_1, FRONTZ_1\r\n");
    for (int i = 0; i < FRAME_RECORDS; i++) {
        ProfilePoint rear = profile_point(&view[UNIT_REAR], i), front = profile_point(&view[UNIT_FRONT], i);
        // The _1 columns of the file format stay zero
        buf_printf(&out, "%-4.2f, %-4.2f, %-4.2f, %-4.2f, %-4.2f, %-4.2f, %-4.2f, %-4.2f\r\n", rear.x, (double)rear.y,
                   front.x, (double)front.y, 0.0, 0.0, 0.0, 0.0);
    }

    respond_buf(client, "text/plain", &out);
}

static void dl_angles(Client *client)
{
    static HistoryEntry entries[HISTORY_MAX];
    int n_entries = copy_history(client->sh, entries);
    Buf out = {0};
    for (int i = 0; i < HISTORY_MAX; i++) {
        // Always HISTORY_MAX lines, zero lines first while the history is not full
        int k = i - (HISTORY_MAX - n_entries);
        HistoryEntry entry = {0};
        if (k >= 0) {
            entry = entries[k];
        }

        // BA1, FA1 and TA1 of the file format stay zero
        buf_printf(&out, "NLR: %u BA0: %-4.2f, FA0: %-4.2f, TA0: %-4.2f,  BA1: %-4.2f, FA1: %-4.2f, TA1: %-4.2f\r\n",
                   (unsigned)entry.phase, (double)entry.rear, (double)entry.front, (double)entry.combined, 0.0, 0.0,
                   0.0);
    }

    respond_buf(client, "text/plain", &out);
}

static int form_int(const char *form, const char *key, int fallback)
{
    char value[32], *end;
    if (!http_form_value(form, key, value, sizeof(value)) || !value[0]) {
        return fallback;
    }

    long number = strtol(value, &end, 10);

    return *end ? fallback : (int)number;
}

static void post_cmd(Client *client)
{
    char op[16] = "", reply[SERVICE_REPLY_MAX], what[64];
    ServiceRequest request = {.kind = SERVICE_IO};
    http_form_value(client->body, "op", op, sizeof(op));
    bool on = strcmp(op, "on") == 0, ok = true;
    if (on || strcmp(op, "off") == 0) {
        request.op = on ? IO_OUTPUT_ON : IO_OUTPUT_OFF;
        request.arg = form_int(client->body, "n", 0);
        snprintf(what, sizeof(what), "output A%d %s", request.arg, op);
    } else if (strcmp(op, "reset") == 0) {
        request.op = IO_RESET_COUNTER;
        request.arg = form_int(client->body, "unit", -1);
        snprintf(what, sizeof(what), "reset counter %d", request.arg);
    } else if (strcmp(op, "drive") == 0) {
        request.op = IO_DRIVE_TO;
        request.arg = form_int(client->body, "unit", -1);
        request.value = form_int(client->body, "target", -1);
        snprintf(what, sizeof(what), "drive unit %d to %d", request.arg, (int)request.value);
    } else {
        ok = false;
        snprintf(what, sizeof(what), "command '%s'", op);
        snprintf(reply, sizeof(reply), "refused: unknown command");
    }

    if (ok && form_int(client->body, "enable", 0) != 1) { // outputs, counters and drives act on the machine
        ok = false;
        snprintf(reply, sizeof(reply), "refused: switching is not enabled on the page");
    }

    if (ok) {
        ok = shared_request(client->sh, &request, reply, sizeof(reply));
    }

    log_msg("service: %s: %s", what, reply);
    respond_result(client, HTTP_OK, ok, reply);
}

static bool authorized(Client *client)
{
    char password[PASSWORD_MAX + 2] = ""; // one character more shows a password that is too long
    if (data_file_read(client->sh->data_dir, PASSWORD_FILE_NAME, password, sizeof(password))) {
        password[strcspn(password, "\r\n")] = '\0';
    }

    if (strlen(password) > PASSWORD_MAX) {
        log_msg("service: POST %s refused: password in " PASSWORD_FILE_NAME " longer than %d characters",
                client->req.path, PASSWORD_MAX);
        respond_result(client, HTTP_FORBIDDEN, false, "password in " PASSWORD_FILE_NAME " too long");
        return false;
    }

    if (!password[0]) {
        log_msg("service: POST %s refused: read only (no " PASSWORD_FILE_NAME ")", client->req.path);
        respond_result(client, HTTP_FORBIDDEN, false, "read only: no " PASSWORD_FILE_NAME);
        return false;
    }

    if (!http_basic_auth_ok(client->req.auth, SERVICE_USER, password)) {
        log_msg("service: POST %s refused: wrong or missing credentials", client->req.path);
        // 401 without a WWW-Authenticate challenge, so the browser shows no login dialog of its own
        respond_result(client, HTTP_UNAUTHORIZED, false, "wrong or missing service password");
        return false;
    }

    return true;
}

static const Route ROUTES[] = {
    {"GET", "/api/status", get_status},   {"GET", "/api/profile", get_profile}, {"GET", "/api/history", get_history},
    {"GET", "/api/log", get_log},         {"GET", "/api/setup", get_setup},     {"GET", "/api/dies", get_dies},
    {"GET", "/dl/Points.txt", dl_points}, {"GET", "/dl/Angle.txt", dl_angles},  {"POST", "/api/cmd", post_cmd},
    {"POST", "/api/setup", post_setup},   {"POST", "/api/dies", post_dies},
};

static bool respond_asset(Client *client)
{
    const char *path = strcmp(client->req.path, "/") == 0 ? INDEX_PATH : client->req.path;
    for (size_t i = 0; i < n_web_assets; i++) {
        const WebAsset *asset = &web_assets[i];
        if (strcmp(path, asset->path) == 0) {
            respond(client, HTTP_OK, asset->type, (const char *)asset->data, asset->len);
            return true;
        }
    }

    return false;
}

static void handle_request(Client *client)
{
    bool get = strcmp(client->req.method, "GET") == 0, post = strcmp(client->req.method, "POST") == 0;
    if (!get && !post) {
        respond_text(client, HTTP_BAD_REQUEST, "bad request");
        return;
    }

    if (get && respond_asset(client)) {
        return;
    }

    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        const Route *route = &ROUTES[i];
        if (strcmp(client->req.method, route->method) == 0 && strcmp(client->req.path, route->path) == 0) {
            if (!post || authorized(client)) {
                route->handle(client);
            }

            return;
        }
    }

    respond_text(client, HTTP_NOT_FOUND, "not found");
}

static void serve_client(Shared *sh, int fd)
{
    static char buf[REQUEST_MAX + 1];
    struct timeval timeout = {RECEIVE_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    Client client = {.sh = sh, .fd = fd};
    size_t len = 0;
    bool have_head = false;
    while (len < REQUEST_MAX) {
        ssize_t received = recv(fd, buf + len, REQUEST_MAX - len, 0);
        if (received <= 0) {
            return;
        }

        len += (size_t)received;
        buf[len] = '\0';
        if (!have_head && http_parse_head(buf, len, &client.req)) {
            have_head = true;
        }

        if (have_head &&
            len >= client.req.header_len + (size_t)(client.req.content_length > 0 ? client.req.content_length : 0)) {
            break;
        }
    }

    if (!have_head) {
        respond_text(&client, HTTP_BAD_REQUEST, "bad request");
        return;
    }

    client.body = buf + client.req.header_len;
    handle_request(&client);
}

int web_listen(const char *spec)
{
    char host[64] = ANY_ADDRESS;
    const char *colon = strrchr(spec, ':');
    int port = atoi(colon ? colon + 1 : spec);
    if (colon) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - spec), spec);
    }

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (port <= 0 || port > PORT_MAX || inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void *web_thread_main(void *arg)
{
    WebThreadArgs *args = arg;
    while (!atomic_load(&args->sh->stop)) {
        struct pollfd listener = {args->listen_fd, POLLIN, 0};
        if (poll(&listener, 1, ACCEPT_TIMEOUT_MS) <= 0) {
            continue;
        }

        int fd = accept(args->listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }

        serve_client(args->sh, fd);
        close(fd);
    }

    close(args->listen_fd);

    return NULL;
}
