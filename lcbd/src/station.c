#include "station.h"

#include <math.h>
#include <string.h>

#include "x87.h"

#define START_ANGLE 180.0f // held angle at the start of a scan

void station_init(Station *st) { memset(st, 0, sizeof(*st)); }

void station_start_scan(Station *st) { st->held_angle = START_ANGLE; }

#define SCORE_MAX 7

static void update_score(int *score, bool valid)
{
    if (valid) {
        *score = *score + 1 > SCORE_MAX ? SCORE_MAX : *score + 1;
    } else {
        *score = *score - 1 < 0 ? 0 : *score - 1;
    }
}

#define TRACK_STEP_MAX 8 // the tracked leg follows only changes below this

#define ALONE_SCORE 6 // a leg above this while the other is at 0 measures alone
#define TRACK_SCORE 5 // a leg above this while the other is above 0 has the other one tracked

static bool one_leg_angle(Station *st, int unit, int other, float *tracked, float *out)
{
    const int *score = st->valid_score;
    float *last = st->last_angle;

    if (score[unit] > ALONE_SCORE && score[other] == 0 && st->valid[unit]) {
        *out = (float)(2 * (ext)st->leg_angle[unit]);
        return true;
    }

    if (score[unit] > TRACK_SCORE && score[other] > 0 && st->valid[unit] && !st->valid[other] && last[unit] != 0 &&
        last[other] != 0) {
        float leg = st->leg_angle[unit];
        float change = (float)((ext)leg - last[unit]);
        *tracked = (float)((ext)last[other] + change);
        if (change < TRACK_STEP_MAX && fabsl((ext)leg - *tracked) < TRACK_STEP_MAX) {
            *out = (float)((ext)leg + *tracked);
            last[other] = *tracked;
            return true;
        }
    }

    return false;
}

#define LOST_SCORE 2 // both legs invalid but above this: the sum of their last angles

static bool pair_angle(Station *st, float *out)
{
    const float *leg = st->leg_angle;
    float *last = st->last_angle;
    const bool *valid = st->valid;
    int *score = st->valid_score;
    float tracked = 0;

    update_score(&score[UNIT_FRONT], valid[UNIT_FRONT]);
    update_score(&score[UNIT_REAR], valid[UNIT_REAR]);

    // At most one of the two finds an angle
    bool have = one_leg_angle(st, UNIT_FRONT, UNIT_REAR, &tracked, out);
    have |= one_leg_angle(st, UNIT_REAR, UNIT_FRONT, &tracked, out);

    if (!valid[UNIT_REAR] && !valid[UNIT_FRONT] && score[UNIT_FRONT] > LOST_SCORE && score[UNIT_REAR] > LOST_SCORE) {
        *out = (float)((ext)last[UNIT_REAR] + last[UNIT_FRONT]);
        return true;
    }

    last[UNIT_FRONT] = valid[UNIT_FRONT] ? leg[UNIT_FRONT] : tracked;
    last[UNIT_REAR] = valid[UNIT_REAR] ? leg[UNIT_REAR] : tracked;
    if (valid[UNIT_REAR] && valid[UNIT_FRONT]) {
        *out = (float)((ext)leg[UNIT_REAR] + leg[UNIT_FRONT]);
        return true;
    }

    return have;
}

static float smooth(Station *st, float angle)
{
    if (st->n_history == STATION_HISTORY_MAX) {
        memmove(st->history, st->history + 1, sizeof(st->history) - sizeof(*st->history));
        st->n_history = STATION_HISTORY_MAX - 1;
    }

    st->history[st->n_history++] = angle;

    float sum = 0;
    for (int i = st->n_history - 1; i >= 0; i--) {
        sum = (float)((ext)sum + st->history[i]);
    }

    return (float)((ext)sum / st->n_history);
}

#define RESULT_MIN 20 // accepted angles, others return the held angle
#define RESULT_MAX 180

static float combine(Station *st)
{
    float out = 0;
    if (!pair_angle(st, &out) || !(out >= RESULT_MIN && out <= RESULT_MAX)) {
        return st->held_angle;
    }

    st->held_angle = out;

    return smooth(st, out);
}

bool station_update(Station *st, int unit, float leg_angle, bool valid, StationOutput *output)
{
    st->leg_angle[unit] = leg_angle;
    st->valid[unit] = valid;
    st->delivered[unit] = true;

    st->both |= st->valid[UNIT_REAR] && st->valid[UNIT_FRONT];

    if (!st->delivered[UNIT_REAR] || !st->delivered[UNIT_FRONT]) {
        return false;
    }

    st->delivered[UNIT_REAR] = st->delivered[UNIT_FRONT] = false;
    st->pair_count++;
    output->angle = combine(st);
    output->both = st->both;
    output->valid_rear = st->valid[UNIT_REAR];
    output->valid_front = st->valid[UNIT_FRONT];

    return true;
}
