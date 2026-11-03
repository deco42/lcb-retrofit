#include "check.h"
#include "station.h"

static bool update_pair(Station *st, float rear, bool rear_valid, float front, bool front_valid, StationOutput *output)
{
    station_update(st, UNIT_REAR, rear, rear_valid, output);
    return station_update(st, UNIT_FRONT, front, front_valid, output);
}

static void test_pairing_smoothing_and_range(void)
{
    Station st;
    StationOutput output;
    station_init(&st);

    // The second frame of the same sensor does not complete a pair
    CHECK(!station_update(&st, UNIT_REAR, 0, false, &output));
    CHECK(!station_update(&st, UNIT_REAR, 0, false, &output));
    CHECK(station_update(&st, UNIT_FRONT, 0, false, &output));
    CHECK(output.angle == 0 && !output.both);

    station_start_scan(&st);
    CHECK(update_pair(&st, 0, false, 0, false, &output) && output.angle == 180.0f);

    // Both legs valid: sum, smoothed over up to three accepted values
    CHECK(update_pair(&st, 45, true, 45, true, &output) && output.angle == 90.0f && output.both && output.valid_rear &&
          output.valid_front);
    CHECK(update_pair(&st, 46, true, 46, true, &output) && output.angle == 91.0f);
    CHECK(update_pair(&st, 47, true, 47, true, &output) && output.angle == 92.0f);
    CHECK(update_pair(&st, 48, true, 48, true, &output) && output.angle == 94.0f); // mean of 92, 94, 96

    // Out of range (> 180): the held angle is returned
    CHECK(update_pair(&st, 91, true, 91, true, &output) && output.angle == 96.0f);
}

static void test_front_leg_alone(void)
{
    Station st;
    StationOutput output;
    station_init(&st);

    // Twice the front angle once the rear score has run down
    for (int i = 0; i < 10; i++) {
        update_pair(&st, 0, false, 50, true, &output);
    }

    CHECK(output.angle == 100.0f && !output.both && !output.valid_rear && output.valid_front);
}

int main(void)
{
    test_pairing_smoothing_and_range();
    test_front_leg_alone();
    return check_report("station");
}
