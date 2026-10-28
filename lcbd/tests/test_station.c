#include "check.h"
#include "station.h"

static bool pair(Station *st, float rear, bool vr, float front, bool vf, StationOutput *o)
{
    station_update(st, SENSOR_REAR, rear, vr, o);
    return station_update(st, SENSOR_FRONT, front, vf, o);
}

int main(void)
{
    Station st;
    StationOutput o;
    station_init(&st);

    // pairing: the second frame of the same sensor does not complete a pair
    CHECK(!station_update(&st, SENSOR_REAR, 0, false, &o));
    CHECK(!station_update(&st, SENSOR_REAR, 0, false, &o));
    CHECK(station_update(&st, SENSOR_FRONT, 0, false, &o));
    CHECK(o.angle == 0 && !o.both);

    station_start_scan(&st);
    CHECK(pair(&st, 0, false, 0, false, &o) && o.angle == 180.0f);

    // both legs valid: sum, smoothed over up to three accepted values
    CHECK(pair(&st, 45, true, 45, true, &o) && o.angle == 90.0f && o.both && o.valid_rear && o.valid_front);
    CHECK(pair(&st, 46, true, 46, true, &o) && o.angle == 91.0f);
    CHECK(pair(&st, 47, true, 47, true, &o) && o.angle == 92.0f);
    CHECK(pair(&st, 48, true, 48, true, &o) && o.angle == 94.0f); // mean of 92, 94, 96

    // out of range (> 180): the held angle is returned
    CHECK(pair(&st, 91, true, 91, true, &o) && o.angle == 96.0f);

    // only the front leg valid for a while: rule 2 * alpha_f once c_r has run down
    station_init(&st);
    for (int i = 0; i < 10; i++) {
        pair(&st, 0, false, 50, true, &o);
    }

    CHECK(o.angle == 100.0f && !o.both && !o.valid_rear && o.valid_front);

    return check_report("station");
}
