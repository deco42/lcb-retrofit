// combines the leg angles of the rear and front sensor into the bend angle
#ifndef LCBD_STATION_H
#define LCBD_STATION_H

#include <stdbool.h>

enum { SENSOR_REAR = 0, SENSOR_FRONT = 1 };

typedef struct {
    float angle; // result angle handed to the CNC link
    bool both;   // both legs were valid at the same time at least once
    bool valid_rear, valid_front;
} StationOutput;

typedef struct {
    float alpha[2];
    bool valid[2], delivered[2];
    bool both;
    int cnt[2];    // c_r, c_f
    float last[2]; // l_r, l_f
    float hist[3]; // smoothing history, oldest first
    int nhist;
    float held; // H
    long pairs;
} Station;

void station_init(Station *st);
void station_start_scan(Station *st);
bool station_update(Station *st, int sensor, float alpha, bool valid, StationOutput *out);

#endif
