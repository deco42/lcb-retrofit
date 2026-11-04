#ifndef LCBD_VECTORS_H
#define LCBD_VECTORS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "evaluator.h"
#include "nlr.h"
#include "station.h"
#include "unit.h"

#define VECTORS_CSV_HEADER                                                                                             \
    "n,sensor,t,valid,leg_angle,die_angle,line_angle,ref_done,die_found,leg_points,filt_points,paired,both_valid,"     \
    "valid_rear,valid_front,station_angle,nlr_raw\n"
#define VECTORS_CSV_COLUMNS 17
#define VECTORS_PARAM20 1000 // p20 and p28 of the nlr_raw column: raw angles of 180 and 90 degrees
#define VECTORS_PARAM28 2000

typedef struct {
    Evaluator ev[UNITS];
    Station st;
} Pipeline;

// One frame, the fields are the CSV columns
typedef struct {
    long n;
    int sensor;
    double time_s;
    bool valid;
    float leg_angle, die_angle, line_angle;
    bool ref_done;
    int die_found, leg_points, filt_points;
    bool paired;
    bool both_valid, valid_rear, valid_front; // this and the next columns are empty without a pair
    float station_angle;
    uint16_t nlr_raw;
} VectorRow;

static inline void pipeline_init(Pipeline *pipeline)
{
    SensorFile sensor_file;
    sensor_file_defaults(&sensor_file);
    evaluator_init(&pipeline->ev[UNIT_REAR], &sensor_file.leg_line);
    evaluator_init(&pipeline->ev[UNIT_FRONT], &sensor_file.leg_line);
    station_init(&pipeline->st);
}

static inline VectorRow pipeline_process(Pipeline *pipeline, int sensor, const uint8_t frame[FRAME_SIZE])
{
    const EvalResult *res = evaluator_process(&pipeline->ev[sensor], frame);
    StationOutput out;
    bool paired = station_update(&pipeline->st, sensor, res->leg_angle, res->valid, &out);
    VectorRow row = {
        .sensor = sensor,
        .valid = res->valid,
        .leg_angle = res->leg_angle,
        .die_angle = res->die_angle,
        .line_angle = res->line_angle,
        .ref_done = res->done,
        .die_found = res->die_fits,
        .leg_points = res->leg_points,
        .filt_points = res->profile_last,
        .paired = paired,
    };
    if (paired) {
        row.both_valid = out.both;
        row.valid_rear = out.valid_rear;
        row.valid_front = out.valid_front;
        row.station_angle = out.angle;
        row.nlr_raw = nlr_scale(out.angle, VECTORS_PARAM20, VECTORS_PARAM28);
    }

    return row;
}

static inline void vector_row_print(const VectorRow *row)
{
    printf("%ld,%d,%.6f,%d,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d", row->n, row->sensor, row->time_s, row->valid,
           (double)row->leg_angle, (double)row->die_angle, (double)row->line_angle, row->ref_done, row->die_found,
           row->leg_points, row->filt_points, row->paired);
    if (row->paired) {
        printf(",%d,%d,%d,%.9g,%u\n", row->both_valid, row->valid_rear, row->valid_front, (double)row->station_angle,
               row->nlr_raw);
    } else {
        printf(",,,,,\n");
    }
}

static inline bool vector_row_parse(char *line, VectorRow *row)
{
    char *column[VECTORS_CSV_COLUMNS];
    int n = 0;
    while (n < VECTORS_CSV_COLUMNS && line) {
        column[n++] = strsep(&line, ",");
    }

    if (n < VECTORS_CSV_COLUMNS) {
        return false;
    }

    *row = (VectorRow){
        .n = atol(column[0]),
        .sensor = atoi(column[1]),
        .time_s = strtod(column[2], NULL),
        .valid = atoi(column[3]),
        .leg_angle = (float)strtod(column[4], NULL),
        .die_angle = (float)strtod(column[5], NULL),
        .line_angle = (float)strtod(column[6], NULL),
        .ref_done = atoi(column[7]),
        .die_found = atoi(column[8]),
        .leg_points = atoi(column[9]),
        .filt_points = atoi(column[10]),
        .paired = atoi(column[11]),
    };
    if (row->paired) {
        row->both_valid = atoi(column[12]);
        row->valid_rear = atoi(column[13]);
        row->valid_front = atoi(column[14]);
        row->station_angle = (float)strtod(column[15], NULL);
        row->nlr_raw = (uint16_t)atoi(column[16]);
    }

    return true;
}

#endif
