#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "config.h"
#include "evaluator.h"

// Machine model of the synthetic frames: rows of points along the die flank from the die edge R and along the leg
#define REF_X 1500
#define REF_Y 2000
#define DIE_ANGLE (-60)
#define POINT_STEP 12.0
#define DIE_POINTS 120
#define LEG_POINTS 70
#define LEG_OFFSET_Y 100
#define POINT_QUALITY 100
#define NO_LEG (-1)
#define LEG_ANGLE 30
#define ANGLE_TOLERANCE 0.5f

#define LEG_FRAMES 3
#define RANDOM_FRAMES 2000

static void put_record(uint8_t *record, double x, double y)
{
    int raw_x = (int)nearbyint(x * FRAME_X_SCALE), raw_y = (int)nearbyint(y);
    record[0] = (uint8_t)(raw_x & 0x7F);
    record[1] = (uint8_t)(raw_x >> 7);
    record[2] = (uint8_t)(raw_y & 0x7F);
    record[3] = (uint8_t)(raw_y >> 7);
    record[4] = POINT_QUALITY;
}

static void sim_frame(uint8_t *frame, double leg_angle)
{
    double die_cos = cos(DIE_ANGLE * M_PI / 180), die_sin = sin(DIE_ANGLE * M_PI / 180);
    double x[EVAL_POINTS_MAX], y[EVAL_POINTS_MAX];
    int n = 0;
    if (leg_angle == NO_LEG) {
        x[n] = REF_X - POINT_STEP * die_cos;
        y[n++] = REF_Y - POINT_STEP * die_sin;
    } else {
        double leg = (DIE_ANGLE + leg_angle) * M_PI / 180;
        for (int i = LEG_POINTS; i > 0; i--) {
            x[n] = REF_X - i * POINT_STEP * cos(leg);
            y[n++] = REF_Y + LEG_OFFSET_Y - i * POINT_STEP * sin(leg);
        }
    }

    x[n] = REF_X;
    y[n++] = REF_Y;
    for (int i = 1; i <= DIE_POINTS; i++) {
        x[n] = REF_X + i * POINT_STEP * die_cos;
        y[n++] = REF_Y + i * POINT_STEP * die_sin;
    }

    memset(frame, 0, FRAME_SIZE);
    for (int i = 0; i < n; i++) {
        put_record(frame + FRAME_POINTS_OFFSET + FRAME_RECORD_SIZE * i, x[i], y[i]);
    }
}

int main(void)
{
    static uint8_t ref[FRAME_SIZE], leg[FRAME_SIZE], junk[FRAME_SIZE];
    SensorFile sensor_file;
    sensor_file_defaults(&sensor_file);
    Evaluator ev;
    evaluator_init(&ev, &sensor_file.leg_line);
    sim_frame(ref, NO_LEG);

    const EvalResult *res = evaluator_process(&ev, ref);
    CHECK(res->ref_frame && !res->valid && evaluator_die_found(&ev) && res->die_fits == 1);
    CHECK(res->ref.x == REF_X && res->ref.y == REF_Y);
    CHECK(fabsf(res->die_angle - DIE_ANGLE) < ANGLE_TOLERANCE);
    for (int i = 1; i < EVAL_REF_FRAMES; i++) {
        res = evaluator_process(&ev, ref);
    }

    CHECK(evaluator_reference_done(&ev) && !res->done); // the done flag follows with the next frame
    res = evaluator_process(&ev, ref);
    CHECK(res->done && !res->ref_frame);

    sim_frame(leg, LEG_ANGLE);
    for (int i = 0; i < LEG_FRAMES; i++) {
        res = evaluator_process(&ev, leg);
    }

    CHECK(res->valid && fabsf(res->leg_angle - LEG_ANGLE) < ANGLE_TOLERANCE);

    evaluator_restart_reference(&ev);
    CHECK(!evaluator_die_found(&ev) && !evaluator_reference_done(&ev));
    res = evaluator_process(&ev, ref);
    CHECK(res->ref_frame && evaluator_die_found(&ev));

    // Random frames must neither crash nor produce a valid angle in reference mode
    srand(1);
    for (int n = 0; n < RANDOM_FRAMES; n++) {
        for (int i = 0; i < FRAME_SIZE; i++) {
            junk[i] = (uint8_t)rand();
        }

        res = evaluator_process(&ev, junk);
    }

    CHECK(!res->valid && res->candidates >= -1 && res->candidates < FRAME_RECORDS);

    // The reference frame holds the point before R, R itself and the die points
    int n_used = 0;
    for (int i = 0; i < FRAME_RECORDS; i++) {
        const uint8_t *record = frame_record(ref, i);
        if (record_x(record) || record_y(record) || record_quality(record)) {
            n_used++;
        }
    }

    const uint8_t *ref_record = frame_record(ref, 1);
    CHECK(n_used == 2 + DIE_POINTS);
    CHECK(record_x(ref_record) == (int)(REF_X * FRAME_X_SCALE) && record_y(ref_record) == REF_Y &&
          record_quality(ref_record) == POINT_QUALITY);

    return check_report("evaluator");
}
