#ifndef LCBD_STATION_H
#define LCBD_STATION_H

#include <stdbool.h>

#include "unit.h"

#define STATION_HISTORY_MAX 3 // the result is the mean of this many accepted angles

typedef struct {
    float angle; // result angle handed to the CNC link
    bool both;   // both legs were valid at the same time at least once
    bool valid_rear, valid_front;
} StationOutput;

// Angles in degrees
typedef struct {
    float leg_angle[UNITS];
    bool valid[UNITS], delivered[UNITS];
    bool both;
    int valid_score[UNITS]; // counts valid frames up and invalid frames down
    float last_angle[UNITS];
    float history[STATION_HISTORY_MAX]; // accepted angles, oldest first
    int n_history;
    float held_angle;
    long pair_count;
} Station;

void station_init(Station *st);
void station_start_scan(Station *st);
bool station_update(Station *st, int unit, float leg_angle, bool valid, StationOutput *output);

#endif
