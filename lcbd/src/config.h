#ifndef LCBD_CONFIG_H
#define LCBD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unit.h"

#define SETUP_FILE_NAME "setup.dat"
#define SENSOR_FILE_NAME "sensor.dat"
#define DIE_FILE_NAME "die.dat"

typedef enum { MOVE_METHOD_REF, MOVE_METHOD_POS } MoveMethod;

typedef struct {
    MoveMethod move_method; // MV_METHOD
    bool move_down_start;
    bool secure_angle; // USE_SECURE_ANGLE
    bool use_turn;
} CtlSetup;

#define DIE_MAX 200
#define DIE_NAME_MAX 11

// One line of die.dat, positions in counter pulses
typedef struct {
    char name[DIE_NAME_MAX + 1];
    int32_t rear, front;
} DieEntry;

typedef struct {
    int last_section_points; // LE_POINTS_FIXL3
    int min_points;          // LE_POINTS_MIN
    float cover;             // LE_COVER / 100
    float tolerance;         // LE_TOLERANCE
    bool extended;           // LE_LINE_MODE == LM_EXTENDED
} LegLineParams;

typedef struct {
    bool light_filter;     // USE_EXTERNAL_LIGHT_FILTER
    bool internal_control; // USE_INTERNAL_SENSOR_CTRL
    int shutter, gain;     // SENSOR_n_SHUTTERTIME, SENSOR_n_VIDEOGAIN (0 to 1023): fixed values for the scan mode
} SensorRegs;

typedef struct {
    CtlSetup ctl;
    int sb_timeout_s, sb_meantilt; // SB_TIMEOUT (maximum measuring time in s), SB_MEANTILT (tilt angles averaged)
    LegLineParams leg_line;        // stored only, the evaluator uses the one of sensor.dat
} SetupFile;

typedef struct {
    SensorRegs regs[UNITS]; // SENSOR_1_* is the rear, SENSOR_2_* the front sensor
    LegLineParams leg_line;
} SensorFile;

void setup_file_defaults(SetupFile *setup);
void setup_file_parse(SetupFile *setup, const char *text, bool from_form);
size_t setup_file_format(const SetupFile *setup, char *buf, size_t size);

void sensor_file_defaults(SensorFile *sensor_file);
void sensor_file_parse(SensorFile *sensor_file, const char *text);

int leg_line_cover_percent(const LegLineParams *par);
const char *leg_line_mode_name(const LegLineParams *par);
const char *move_method_name(MoveMethod method);

int dies_parse(const char *text, DieEntry *dies, int max);
size_t dies_format(const DieEntry *dies, int n_dies, char *buf, size_t size);

void config_describe(const SetupFile *setup, const SensorFile *sensor_file, char *buf, size_t size);

#endif
