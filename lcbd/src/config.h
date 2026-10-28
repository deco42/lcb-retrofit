#ifndef LCBD_CONFIG_H
#define LCBD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { MV_REF, MV_POS } MoveMethod;

// Controller setup from setup.dat
typedef struct {
    MoveMethod move_method;
    bool move_down_start;
    bool secure_angle;
    bool use_turn;
} CtlSetup;

#define DIE_MAX 200
#define DIE_NAME_MAX 11

// One line of die.dat
typedef struct {
    char name[DIE_NAME_MAX + 1];
    int32_t rear, front;
} DieEntry;

// Leg line parameters of the evaluator from sensor.dat
typedef struct {
    int fix;       // LE_POINTS_FIXL3
    int pmin;      // LE_POINTS_MIN
    float cover;   // LE_COVER / 100
    float tol;     // LE_TOLERANCE
    bool extended; // LE_LINE_MODE == LM_EXTENDED
} LegLineParams;

// Sensor registers from sensor.dat
typedef struct {
    bool light_filter;  // USE_EXTERNAL_LIGHT_FILTER
    bool internal_ctrl; // USE_INTERNAL_SENSOR_CTRL
    int shutter, gain;  // fixed values for scan mode (SENSOR_n_SHUTTERTIME / _VIDEOGAIN)
} SensorRegs;

// The files
typedef struct {
    CtlSetup ctl; // MV_METHOD, MOVE_DOWN_START, USE_SECURE_ANGLE, USE_TURN
    int sb_timeout, sb_meantilt;
    LegLineParams le; // stored only
} SetupFile;

typedef struct {
    SensorRegs regs[2]; // sensor 0 rear (SENSOR_1_*), 1 front (SENSOR_2_*)
    LegLineParams le;
} SensorFile;

void setup_defaults(SetupFile *s);
void setup_parse(SetupFile *s, const char *text, bool form);
size_t setup_format(const SetupFile *s, char *buf, size_t len);

void sensor_defaults(SensorFile *s);
void sensor_parse(SensorFile *s, const char *text);

int legline_cover_percent(const LegLineParams *p);
const char *legline_mode_name(const LegLineParams *p);

int dies_parse(const char *text, DieEntry *out, int max);
size_t dies_format(const DieEntry *d, int n, char *buf, size_t len);

size_t config_describe(const SetupFile *s, const SensorFile *f, char *buf, size_t len);

#endif
