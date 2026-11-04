#include <arpa/inet.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "sensorproto.h"
#include "station.h"
#include "unit.h"
#include "vectors.h"

#define PCAP_HEADER_SIZE 24
#define PCAP_MAGIC 0xa1b2c3d4 // classic format with microsecond time stamps
#define PCAP_LINKTYPE_OFFSET 20
#define PCAP_LINKTYPE_ETHERNET 1
#define PCAP_RECORD_HEADER_SIZE 16 // seconds, microseconds, captured length, original length
#define PACKET_MAX 262144          // largest pcap snapshot length

#define STREAMS_MAX 16
#define ANGLE_UNSET 1e9f // start value of the smallest and the largest angle of a measurement

typedef struct {
    uint32_t src, dst;
    uint16_t src_port, dst_port;
    uint32_t seq;
    bool syn;
    const uint8_t *data;
    size_t len;
} Segment;

typedef struct {
    uint32_t src, dst;
    uint16_t src_port, dst_port;
    uint32_t next_seq;
    bool synced;
    FrameAssembler fa;
} Stream;

// From "start scan" to "stop scan"
typedef struct {
    int number;
    long pairs, valid_pairs;
    float last_angle, min_angle, max_angle;
} Measurement;

typedef struct {
    uint32_t sensor_ip[UNITS];
    bool csv;
    Stream streams[STREAMS_MAX];
    int n_streams;
    Pipeline pipeline;
    long frame_count, gap_count;
    bool scanning;
    Measurement measurement;
    uint8_t held[UNITS][FRAME_SIZE]; // frames completed at the capture time held_s
    bool has_held[UNITS];
    double held_s;
} Replay;

static FILE *event_stream(const Replay *replay) { return replay->csv ? stderr : stdout; }

static Stream *find_stream(Replay *replay, const Segment *seg)
{
    for (int i = 0; i < replay->n_streams; i++) {
        Stream *stream = &replay->streams[i];
        if (stream->src == seg->src && stream->src_port == seg->src_port && stream->dst == seg->dst &&
            stream->dst_port == seg->dst_port) {
            return stream;
        }
    }

    if (replay->n_streams == STREAMS_MAX) {
        return NULL;
    }

    Stream *stream = &replay->streams[replay->n_streams++];
    *stream = (Stream){.src = seg->src, .dst = seg->dst, .src_port = seg->src_port, .dst_port = seg->dst_port};

    return stream;
}

static void start_measurement(Replay *replay)
{
    Measurement *m = &replay->measurement;
    station_start_scan(&replay->pipeline.st);
    replay->scanning = true;
    m->number++;
    m->pairs = m->valid_pairs = 0;
    m->min_angle = ANGLE_UNSET;
    m->max_angle = -ANGLE_UNSET;
}

static void end_measurement(Replay *replay)
{
    if (!replay->scanning) {
        return;
    }

    const Measurement *m = &replay->measurement;
    replay->scanning = false;
    fprintf(event_stream(replay),
            "measurement %d: %ld pairs, %ld with both legs valid, final angle %.3f, min %.3f, max %.3f\n", m->number,
            m->pairs, m->valid_pairs, (double)m->last_angle, (double)m->min_angle, (double)m->max_angle);
}

// Commands from lcbd to a sensor, register writes of two bytes each
static void read_commands(Replay *replay, int sensor, const uint8_t *data, size_t len, double time_s)
{
    bool light_on = false, scan_mode = false, light_off = false;
    for (size_t i = 0; i < len;) {
        if (data[i] == SENSOR_CMD_START_STREAMING || i + 1 == len) {
            i++;
            continue;
        }

        int reg = data[i], value = data[i + 1] & 0x7F;
        if (reg == SENSOR_REG_LIGHT_FILTER && value) {
            light_on = true;
        } else if (reg == SENSOR_REG_LIGHT_FILTER) {
            light_off = true;
        } else if (reg == SENSOR_REG_FIXED && value == 0) {
            scan_mode = true;
        }

        i += 2;
    }

    if (light_on) {
        evaluator_restart_reference(&replay->pipeline.ev[sensor]);
        fprintf(event_stream(replay), "%.6f sensor %d: start reference\n", time_s, sensor);
    } else if (scan_mode && sensor == UNIT_REAR) {
        start_measurement(replay);
        fprintf(event_stream(replay), "%.6f start scan (measurement %d)\n", time_s, replay->measurement.number);
    } else if (light_off && sensor == UNIT_REAR) {
        fprintf(event_stream(replay), "%.6f stop scan\n", time_s);
        end_measurement(replay);
    }
}

static void count_pair(Measurement *m, const VectorRow *row)
{
    m->pairs++;
    m->valid_pairs += row->valid_rear && row->valid_front;
    m->last_angle = row->station_angle;
    if (row->station_angle < m->min_angle) {
        m->min_angle = row->station_angle;
    }

    if (row->station_angle > m->max_angle) {
        m->max_angle = row->station_angle;
    }
}

static void evaluate_frame(Replay *replay, int sensor, const uint8_t *frame, double time_s)
{
    VectorRow row = pipeline_process(&replay->pipeline, sensor, frame);
    row.n = replay->frame_count;
    row.time_s = time_s;

    if (replay->csv) {
        vector_row_print(&row);
    } else {
        printf("%.6f frame %ld sensor %d: valid %d leg %.3f die %.3f", time_s, row.n, sensor, row.valid,
               (double)row.leg_angle, (double)row.die_angle);
        if (row.paired) {
            printf("  -> angle %.3f (B %d, rear %d, front %d)", (double)row.station_angle, row.both_valid,
                   row.valid_rear, row.valid_front);
        }

        printf("\n");
    }

    if (row.paired && replay->scanning) {
        count_pair(&replay->measurement, &row);
    }

    replay->frame_count++;
}

static void flush_held(Replay *replay)
{
    for (int sensor = 0; sensor < UNITS; sensor++) {
        if (replay->has_held[sensor]) {
            evaluate_frame(replay, sensor, replay->held[sensor], replay->held_s);
        }

        replay->has_held[sensor] = false;
    }
}

static void hold_frame(Replay *replay, int sensor, const uint8_t *frame, double time_s)
{
    if (time_s != replay->held_s || replay->has_held[sensor]) {
        flush_held(replay);
    }

    memcpy(replay->held[sensor], frame, FRAME_SIZE);
    replay->has_held[sensor] = true;
    replay->held_s = time_s;
}

static int sensor_of(const Replay *replay, uint32_t ip)
{
    for (int sensor = 0; sensor < UNITS; sensor++) {
        if (replay->sensor_ip[sensor] == ip) {
            return sensor;
        }
    }

    return -1;
}

static void process_segment(Replay *replay, const Segment *seg, double time_s)
{
    int from = sensor_of(replay, seg->src), to = sensor_of(replay, seg->dst);
    if (from < 0 && to < 0) {
        return;
    }

    Stream *stream = find_stream(replay, seg);
    if (!stream) {
        return;
    }

    if (seg->syn) {
        stream->next_seq = seg->seq + 1;
        stream->synced = true;
        frame_assembler_reset(&stream->fa);
        return;
    }

    if (!seg->len) {
        return;
    }

    // A capture that starts inside the connection starts at a frame boundary
    if (!stream->synced) {
        stream->next_seq = seg->seq;
        stream->synced = true;
    }

    int32_t gap = (int32_t)(seg->seq - stream->next_seq);
    if (gap > 0) {
        replay->gap_count++;
        frame_assembler_reset(&stream->fa);
        stream->next_seq = seg->seq;
        gap = 0;
    }

    size_t retransmitted = (size_t)(-gap);
    if (retransmitted >= seg->len) {
        return;
    }

    const uint8_t *data = seg->data + retransmitted;
    size_t len = seg->len - retransmitted;
    stream->next_seq += (uint32_t)len;

    if (to >= 0) {
        flush_held(replay);
        read_commands(replay, to, data, len, time_s);
        return;
    }

    while (len) {
        if (frame_assemble(&stream->fa, &data, &len) == FRAME_VALID) {
            hold_frame(replay, from, stream->fa.frame, time_s);
        }
    }
}

// IPv4 and TCP only
static void process_packet(Replay *replay, const uint8_t *packet, size_t len, double time_s)
{
    struct ether_header ethernet;
    struct iphdr ip;
    struct tcphdr tcp;
    if (len < sizeof(ethernet) + sizeof(ip)) {
        return;
    }

    memcpy(&ethernet, packet, sizeof(ethernet));
    memcpy(&ip, packet + sizeof(ethernet), sizeof(ip));
    size_t ip_header_len = ip.ihl * 4u, ip_len = ntohs(ip.tot_len);
    if (ntohs(ethernet.ether_type) != ETHERTYPE_IP || ip.protocol != IPPROTO_TCP || ip_len > len - sizeof(ethernet) ||
        ip_len < ip_header_len + sizeof(tcp)) {
        return;
    }

    const uint8_t *tcp_start = packet + sizeof(ethernet) + ip_header_len;
    memcpy(&tcp, tcp_start, sizeof(tcp));
    size_t tcp_header_len = tcp.doff * 4u;
    if (ip_header_len + tcp_header_len > ip_len) {
        return;
    }

    Segment seg = {
        .src = ip.saddr,
        .dst = ip.daddr,
        .src_port = ntohs(tcp.source),
        .dst_port = ntohs(tcp.dest),
        .seq = ntohl(tcp.seq),
        .syn = tcp.syn,
        .data = tcp_start + tcp_header_len,
        .len = ip_len - ip_header_len - tcp_header_len,
    };
    process_segment(replay, &seg, time_s);
}

int main(int argc, char **argv)
{
    static Replay replay;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0) {
            replay.csv = true;
        } else if (!path && argv[i][0] != '-') {
            path = argv[i];
        } else {
            path = NULL;
            break;
        }
    }

    if (!path) {
        fprintf(stderr, "usage: %s CAPTURE.pcap [--csv]\n", argv[0]);
        return 2;
    }

    inet_pton(AF_INET, SENSOR_DEFAULT_REAR, &replay.sensor_ip[UNIT_REAR]);
    inet_pton(AF_INET, SENSOR_DEFAULT_FRONT, &replay.sensor_ip[UNIT_FRONT]);

    FILE *f = fopen(path, "rb");
    uint8_t header[PCAP_HEADER_SIZE];
    if (!f || fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fprintf(stderr, "%s: cannot read\n", path);
        return 2;
    }

    if (le32(header) != PCAP_MAGIC || le32(header + PCAP_LINKTYPE_OFFSET) != PCAP_LINKTYPE_ETHERNET) {
        fprintf(stderr, "%s: not a classic microsecond pcap of Ethernet frames\n", path);
        return 2;
    }

    pipeline_init(&replay.pipeline);
    if (replay.csv) {
        fputs(VECTORS_CSV_HEADER, stdout);
    }

    static uint8_t packet[PACKET_MAX];
    uint8_t record[PCAP_RECORD_HEADER_SIZE];
    while (fread(record, 1, sizeof(record), f) == sizeof(record)) {
        uint32_t len = le32(record + 8);
        if (len > sizeof(packet) || fread(packet, 1, len, f) != len) {
            break;
        }

        process_packet(&replay, packet, len, le32(record) + le32(record + 4) * 1e-6);
    }

    fclose(f);
    flush_held(&replay);
    end_measurement(&replay);
    fprintf(event_stream(&replay), "%ld frames evaluated, %ld pairs, %ld capture gaps\n", replay.frame_count,
            replay.pipeline.st.pair_count, replay.gap_count);

    return 0;
}
