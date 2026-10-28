#include "station.h"

#include <math.h>
#include <string.h>

#include "x87.h"

void station_init(Station *st) { memset(st, 0, sizeof *st); }

void station_start_scan(Station *st) { st->held = 180.0f; }

static void count(int *c, bool valid)
{
    if (valid) {
        *c = *c + 1 > 7 ? 7 : *c + 1;
    } else {
        *c = *c - 1 < 0 ? 0 : *c - 1;
    }
}

static bool track(float alpha_a, float *last_a, float *last_b, float *tr, float *out)
{
    float delta = (float)((ext)alpha_a - *last_a);
    *tr = (float)((ext)*last_b + delta);
    if (delta < 8 && fabsl((ext)alpha_a - *tr) < 8) {
        *out = (float)((ext)alpha_a + *tr);
        *last_b = *tr;
        return true;
    }

    return false;
}

static float combine(Station *st)
{
    const int R = SENSOR_REAR, F = SENSOR_FRONT;
    float *a = st->alpha, *l = st->last, out = 0, tr = 0;
    bool *v = st->valid, have = false;
    int *c = st->cnt;

    count(&c[F], v[F]);
    count(&c[R], v[R]);
    if (c[F] > 6 && c[R] == 0 && v[F]) {
        out = (float)(2 * (ext)a[F]);
        have = true;
    }

    if (c[R] > 6 && c[F] == 0 && v[R]) {
        out = (float)(2 * (ext)a[R]);
        have = true;
    }

    if (c[F] > 5 && c[R] > 0 && v[F] && !v[R] && l[F] != 0 && l[R] != 0) {
        have |= track(a[F], &l[F], &l[R], &tr, &out);
    }

    if (c[R] > 5 && c[F] > 0 && v[R] && !v[F] && l[F] != 0 && l[R] != 0) {
        have |= track(a[R], &l[R], &l[F], &tr, &out);
    }

    if (!v[R] && !v[F] && c[F] > 2 && c[R] > 2) {
        out = (float)((ext)l[R] + l[F]);
        have = true;
    } else {
        l[F] = v[F] ? a[F] : tr;
        l[R] = v[R] ? a[R] : tr;
        if (v[R] && v[F]) {
            out = (float)((ext)a[R] + a[F]);
            have = true;
        }
    }

    if (!have || !(out >= 20 && out <= 180)) {
        return st->held;
    }

    st->held = out;

    if (st->nhist == 3) {
        st->hist[0] = st->hist[1];
        st->hist[1] = st->hist[2];
        st->nhist = 2;
    }

    st->hist[st->nhist++] = out;

    // The running sum is stored as float and accumulated from the newest value back
    float sum = 0;
    for (int i = st->nhist - 1; i >= 0; i--) {
        sum = (float)((ext)sum + st->hist[i]);
    }

    return (float)((ext)sum / st->nhist);
}

bool station_update(Station *st, int sensor, float alpha, bool valid, StationOutput *out)
{
    st->alpha[sensor] = alpha;
    st->valid[sensor] = valid;
    st->delivered[sensor] = true;

    if (st->valid[0] && st->valid[1]) {
        st->both = true;
    }

    if (!st->delivered[0] || !st->delivered[1]) {
        return false;
    }

    st->delivered[0] = st->delivered[1] = false;
    st->pairs++;
    out->angle = combine(st);
    out->both = st->both;
    out->valid_rear = st->valid[SENSOR_REAR];
    out->valid_front = st->valid[SENSOR_FRONT];

    return true;
}
