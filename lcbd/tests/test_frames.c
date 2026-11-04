#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/vectors.h"
#include "clock.h"

#define FRAMES_MAGIC "LCBV"
#define FRAMES_HEADER_SIZE 8 // magic and version
#define RECORD_HEADER_SIZE 9 // sensor and time
#define CSV_LINE_MAX 512
#define LIST_DEFAULT 20
#define TIMING_RUNS 5

typedef enum { RECORD_FRAME, RECORD_START_REFERENCE, RECORD_START_SCAN } RecordType;

// The first FRAME_FIELDS fields belong to the frame, the others to the pair
static const char *const field_names[] = {"valid",      "leg",         "die",    "line",  "ref_done", "die_found",
                                          "leg_points", "filt_points", "paired", "flags", "station",  "raw"};
#define FIELD_COUNT ((int)(sizeof(field_names) / sizeof(field_names[0])))
#define FRAME_FIELDS 8

typedef struct {
    long frame_count, frame_differ_count, pair_count, pair_differ_count, listed_count;
    long field_differ_count[FIELD_COUNT];
    double time_sum_s, time_max_s;
} Tally;

static bool same_bits(float a, float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// Best of TIMING_RUNS runs from the same evaluator state, which leaves out the noise of the OS
static double evaluation_time_s(const Evaluator *ev, const uint8_t frame[FRAME_SIZE])
{
    static Evaluator probe;
    double best_s = 1;
    for (int run = 0; run < TIMING_RUNS; run++) {
        probe = *ev;
        double start_s = clock_s();
        evaluator_process(&probe, frame);
        double run_s = clock_s() - start_s;
        best_s = run_s < best_s ? run_s : best_s;
    }

    return best_s;
}

static void print_row(const char *label, const VectorRow *row)
{
    printf("\n   %s v%d leg %.9g die %.9g line %.9g done %d fits %d nP %d L %d paired %d", label, row->valid,
           (double)row->leg_angle, (double)row->die_angle, (double)row->line_angle, row->ref_done, row->die_found,
           row->leg_points, row->filt_points, row->paired);
    if (row->paired) {
        printf(" B%d %d%d st %.9g raw %u", row->both_valid, row->valid_rear, row->valid_front,
               (double)row->station_angle, row->nlr_raw);
    }
}

static void tally_frame(Tally *tally, const VectorRow *got, const VectorRow *want, long list)
{
    bool pair = got->paired && want->paired;
    bool differs[FIELD_COUNT] = {
        got->valid != want->valid,
        !same_bits(got->leg_angle, want->leg_angle),
        !same_bits(got->die_angle, want->die_angle),
        !same_bits(got->line_angle, want->line_angle),
        got->ref_done != want->ref_done,
        got->die_found != want->die_found,
        got->leg_points != want->leg_points,
        got->filt_points != want->filt_points,
        got->paired != want->paired,
        pair && (got->both_valid != want->both_valid || got->valid_rear != want->valid_rear ||
                 got->valid_front != want->valid_front),
        pair && !same_bits(got->station_angle, want->station_angle),
        pair && got->nlr_raw != want->nlr_raw,
    };
    bool frame_differs = false, pair_differs = false;
    for (int i = 0; i < FIELD_COUNT; i++) {
        tally->field_differ_count[i] += differs[i];
        frame_differs |= differs[i] && i < FRAME_FIELDS;
        pair_differs |= differs[i] && i >= FRAME_FIELDS;
    }

    tally->frame_count++;
    tally->frame_differ_count += frame_differs;
    if (got->paired || want->paired) {
        tally->pair_count++;
        tally->pair_differ_count += pair_differs;
    }

    if ((frame_differs || pair_differs) && tally->listed_count < list) {
        tally->listed_count++;
        printf("frame %ld s%d:", want->n, got->sensor);
        for (int i = 0; i < FIELD_COUNT; i++) {
            if (differs[i]) {
                printf(" %s", field_names[i]);
            }
        }

        print_row("got ", got);
        print_row("want", want);
        printf("\n");
    }
}

static bool compare_records(FILE *frames_file, FILE *expected_file, bool timing, long list, Tally *tally)
{
    Pipeline pipeline;
    pipeline_init(&pipeline);
    bool restart[UNITS] = {false, false};
    char line[CSV_LINE_MAX];
    int type;
    while ((type = fgetc(frames_file)) != EOF) {
        if (type == RECORD_START_REFERENCE) {
            restart[UNIT_REAR] = restart[UNIT_FRONT] = true;
            continue;
        }

        if (type == RECORD_START_SCAN) {
            station_start_scan(&pipeline.st);
            continue;
        }

        uint8_t record_header[RECORD_HEADER_SIZE], frame[FRAME_SIZE];
        if (type != RECORD_FRAME ||
            fread(record_header, 1, sizeof(record_header), frames_file) != sizeof(record_header) ||
            fread(frame, 1, FRAME_SIZE, frames_file) != FRAME_SIZE) {
            fprintf(stderr, "bad record\n");
            return false;
        }

        int sensor = record_header[0];
        if (sensor >= UNITS) {
            fprintf(stderr, "bad sensor number %d\n", sensor);
            return false;
        }

        if (restart[sensor]) {
            evaluator_restart_reference(&pipeline.ev[sensor]);
            restart[sensor] = false;
        }

        if (timing) {
            double time_s = evaluation_time_s(&pipeline.ev[sensor], frame);
            tally->time_sum_s += time_s;
            tally->time_max_s = time_s > tally->time_max_s ? time_s : tally->time_max_s;
        }

        VectorRow got = pipeline_process(&pipeline, sensor, frame);
        VectorRow want;
        if (!fgets(line, sizeof(line), expected_file) || !vector_row_parse(line, &want)) {
            fprintf(stderr, "expected file too short\n");
            return false;
        }

        tally_frame(tally, &got, &want, list);
    }

    return true;
}

static int report(const char *frames_path, const Tally *tally, bool timing, double min_percent)
{
    long frames = tally->frame_count, pairs = tally->pair_count;
    long identical_frames = frames - tally->frame_differ_count, identical_pairs = pairs - tally->pair_differ_count;
    double frame_percent = frames ? 100.0 * (double)identical_frames / (double)frames : 0;
    double pair_percent = pairs ? 100.0 * (double)identical_pairs / (double)pairs : 100;
    printf("%s: frames %ld identical %ld (%.3f %%), pairs %ld identical %ld (%.3f %%)\n", frames_path, frames,
           identical_frames, frame_percent, pairs, identical_pairs, pair_percent);
    for (int i = 0; i < FIELD_COUNT; i++) {
        if (tally->field_differ_count[i]) {
            printf("  field %-12s differs in %ld\n", field_names[i], tally->field_differ_count[i]);
        }
    }

    if (timing) {
        printf("  evaluation time per frame (best of %d runs): mean %.4f ms, worst frame %.4f ms\n", TIMING_RUNS,
               tally->time_sum_s / (double)frames * 1e3, tally->time_max_s * 1e3);
    }

    return frame_percent >= min_percent && pair_percent >= min_percent ? 0 : 1;
}

int main(int argc, char **argv)
{
    double min_percent = 100;
    long list = LIST_DEFAULT;
    bool timing = false;
    if (argc < 3) {
        fprintf(stderr, "usage: %s FRAMES EXPECTED [--min PERCENT] [--list N] [--time]\n", argv[0]);
        return 2;
    }

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--min") == 0 && i + 1 < argc) {
            min_percent = atof(argv[++i]);
        } else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) {
            list = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--time") == 0) {
            timing = true;
        }
    }

    FILE *frames_file = fopen(argv[1], "rb"), *expected_file = fopen(argv[2], "r");
    if (!frames_file || !expected_file) {
        perror("open");
        return 2;
    }

    char header[FRAMES_HEADER_SIZE], csv_header[CSV_LINE_MAX];
    if (fread(header, 1, sizeof(header), frames_file) != sizeof(header) ||
        memcmp(header, FRAMES_MAGIC, strlen(FRAMES_MAGIC)) != 0 ||
        !fgets(csv_header, sizeof(csv_header), expected_file)) {
        fprintf(stderr, "bad input files\n");
        return 2;
    }

    Tally tally = {0};
    if (!compare_records(frames_file, expected_file, timing, list, &tally)) {
        return 2;
    }

    return report(argv[1], &tally, timing, min_percent);
}
