#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "x87.h"

#define FILE_LINE_MAX 256

#define SB_TIMEOUT_DEFAULT 10
#define SB_MEANTILT_DEFAULT 10
#define SB_MEANTILT_FORM_DEFAULT 7
#define SB_VALUE_MIN 1 // range of SB_TIMEOUT and SB_MEANTILT
#define SB_VALUE_MAX 100000

#define SHUTTER_DEFAULT 310
#define GAIN_DEFAULT 330
#define EXPOSURE_MAX 1023 // shutter and gain are 10-bit sensor values

#define LINE_MODE_EXTENDED "LM_EXTENDED"
#define LINE_MODE_STANDARD "LM_STANDARD"

typedef void (*KeyHandler)(void *target, const char *key, const char *value, bool from_form);

static void for_each_pair(const char *text, KeyHandler handler, void *target, bool from_form)
{
    while (*text) {
        char line[FILE_LINE_MAX];
        size_t len = 0;
        for (; *text && *text != '\n'; text++) {
            if (!isspace((unsigned char)*text) && len < sizeof(line) - 1) {
                line[len++] = *text;
            }
        }

        if (*text == '\n') {
            text++;
        }

        line[len] = '\0';
        char *equals = strchr(line, '=');
        if (!equals || equals == line) {
            continue;
        }

        *equals = '\0';
        handler(target, line, equals + 1, from_form);
    }
}

static void parse_bool(const char *value, bool *out)
{
    if (strcasecmp(value, "TRUE") == 0 || strcmp(value, "1") == 0) {
        *out = true;
    } else if (strcasecmp(value, "FALSE") == 0 || strcmp(value, "0") == 0) {
        *out = false;
    }
}

static void parse_int(const char *value, int *out, int min, int max, bool from_form, int form_default)
{
    char *end;
    long number = strtol(value, &end, 10);

    if (*value && !*end && number >= min && number <= max) {
        *out = (int)number;
    } else if (from_form) {
        *out = form_default;
    }
}

static float cover_fraction(int percent) { return (float)((ext)percent / 100); }

#define LAST_SECTION_POINTS_DEFAULT 60
#define MIN_POINTS_DEFAULT 50
#define COVER_PERCENT_DEFAULT 99
#define TOLERANCE_DEFAULT 0.4f
#define POINT_COUNT_MIN 1 // range of LE_POINTS_FIXL3 and LE_POINTS_MIN
#define POINT_COUNT_MAX 99
#define COVER_PERCENT_MIN 0
#define COVER_PERCENT_MAX 199

static void leg_line_key(LegLineParams *par, const char *key, const char *value, bool from_form)
{
    if (strcmp(key, "LE_POINTS_FIXL3") == 0) {
        parse_int(value, &par->last_section_points, POINT_COUNT_MIN, POINT_COUNT_MAX, from_form,
                  LAST_SECTION_POINTS_DEFAULT);
    } else if (strcmp(key, "LE_POINTS_MIN") == 0) {
        parse_int(value, &par->min_points, POINT_COUNT_MIN, POINT_COUNT_MAX, from_form, MIN_POINTS_DEFAULT);
    } else if (strcmp(key, "LE_COVER") == 0) {
        int percent = -1; // stays negative when an invalid value is kept
        parse_int(value, &percent, COVER_PERCENT_MIN, COVER_PERCENT_MAX, from_form, COVER_PERCENT_DEFAULT);
        if (percent >= 0) {
            par->cover = cover_fraction(percent);
        }
    } else if (strcmp(key, "LE_TOLERANCE") == 0) {
        char *end;
        double tolerance = strtod(value, &end);
        if (*value && !*end && tolerance > 0) {
            par->tolerance = (float)tolerance;
        } else if (from_form) {
            par->tolerance = TOLERANCE_DEFAULT;
        }
    } else if (strcmp(key, "LE_LINE_MODE") == 0) {
        par->extended = strcmp(value, LINE_MODE_EXTENDED) == 0;
    }
}

static void leg_line_defaults(LegLineParams *par)
{
    par->last_section_points = LAST_SECTION_POINTS_DEFAULT;
    par->min_points = MIN_POINTS_DEFAULT;
    par->cover = cover_fraction(COVER_PERCENT_DEFAULT);
    par->tolerance = TOLERANCE_DEFAULT;
    par->extended = true;
}

static void setup_key(void *target, const char *key, const char *value, bool from_form)
{
    SetupFile *setup = target;
    if (strcmp(key, "MV_METHOD") == 0) {
        if (strcmp(value, move_method_name(MOVE_METHOD_REF)) == 0) {
            setup->ctl.move_method = MOVE_METHOD_REF;
        } else if (strcmp(value, move_method_name(MOVE_METHOD_POS)) == 0) {
            setup->ctl.move_method = MOVE_METHOD_POS;
        }
    } else if (strcmp(key, "MOVE_DOWN_START") == 0) {
        parse_bool(value, &setup->ctl.move_down_start);
    } else if (strcmp(key, "USE_SECURE_ANGLE") == 0) {
        parse_bool(value, &setup->ctl.secure_angle);
    } else if (strcmp(key, "USE_TURN") == 0) {
        parse_bool(value, &setup->ctl.use_turn);
    } else if (strcmp(key, "SB_TIMEOUT") == 0) {
        parse_int(value, &setup->sb_timeout_s, SB_VALUE_MIN, SB_VALUE_MAX, from_form, SB_TIMEOUT_DEFAULT);
    } else if (strcmp(key, "SB_MEANTILT") == 0) {
        parse_int(value, &setup->sb_meantilt, SB_VALUE_MIN, SB_VALUE_MAX, from_form, SB_MEANTILT_FORM_DEFAULT);
    } else {
        leg_line_key(&setup->leg_line, key, value, from_form);
    }
}

static const char *const SHUTTER_KEYS[UNITS] = {"SENSOR_1_SHUTTERTIME", "SENSOR_2_SHUTTERTIME"};
static const char *const GAIN_KEYS[UNITS] = {"SENSOR_1_VIDEOGAIN", "SENSOR_2_VIDEOGAIN"};

static void sensor_key(void *target, const char *key, const char *value, bool from_form)
{
    SensorFile *sensor_file = target;
    SensorRegs *regs = sensor_file->regs;
    (void)from_form;

    for (int unit = 0; unit < UNITS; unit++) {
        if (strcmp(key, SHUTTER_KEYS[unit]) == 0) {
            parse_int(value, &regs[unit].shutter, 0, EXPOSURE_MAX, false, 0);
            return;
        }

        if (strcmp(key, GAIN_KEYS[unit]) == 0) {
            parse_int(value, &regs[unit].gain, 0, EXPOSURE_MAX, false, 0);
            return;
        }
    }

    if (strcmp(key, "USE_EXTERNAL_LIGHT_FILTER") == 0) {
        parse_bool(value, &regs[UNIT_REAR].light_filter);
    } else if (strcmp(key, "USE_INTERNAL_SENSOR_CTRL") == 0) {
        parse_bool(value, &regs[UNIT_REAR].internal_control);
    } else {
        leg_line_key(&sensor_file->leg_line, key, value, false);
    }

    // The USE_* switches hold for both sensors
    regs[UNIT_FRONT].light_filter = regs[UNIT_REAR].light_filter;
    regs[UNIT_FRONT].internal_control = regs[UNIT_REAR].internal_control;
}

void setup_file_defaults(SetupFile *setup)
{
    memset(setup, 0, sizeof(*setup));
    setup->ctl.move_method = MOVE_METHOD_REF;
    setup->ctl.move_down_start = true;
    setup->sb_timeout_s = SB_TIMEOUT_DEFAULT;
    setup->sb_meantilt = SB_MEANTILT_DEFAULT;
    leg_line_defaults(&setup->leg_line);
}

void setup_file_parse(SetupFile *setup, const char *text, bool from_form)
{
    for_each_pair(text, setup_key, setup, from_form);
}

size_t setup_file_format(const SetupFile *setup, char *buf, size_t size)
{
    const CtlSetup *ctl = &setup->ctl;
    const LegLineParams *par = &setup->leg_line;
    int len = snprintf(buf, size,
                       "MV_METHOD=%s\nMOVE_DOWN_START=%s\nUSE_SECURE_ANGLE=%s\nUSE_TURN=%s\nSB_TIMEOUT=%d\n"
                       "SB_MEANTILT=%d\nLE_POINTS_FIXL3=%d\nLE_POINTS_MIN=%d\nLE_COVER=%d\nLE_TOLERANCE=%g\n"
                       "LE_LINE_MODE=%s\n",
                       move_method_name(ctl->move_method), ctl->move_down_start ? "TRUE" : "FALSE",
                       ctl->secure_angle ? "TRUE" : "FALSE", ctl->use_turn ? "TRUE" : "FALSE", setup->sb_timeout_s,
                       setup->sb_meantilt, par->last_section_points, par->min_points, leg_line_cover_percent(par),
                       (double)par->tolerance, leg_line_mode_name(par));

    return len < 0 ? 0 : (size_t)len;
}

void sensor_file_defaults(SensorFile *sensor_file)
{
    memset(sensor_file, 0, sizeof(*sensor_file));

    for (int unit = 0; unit < UNITS; unit++) {
        sensor_file->regs[unit] = (SensorRegs){
            .light_filter = true, .internal_control = false, .shutter = SHUTTER_DEFAULT, .gain = GAIN_DEFAULT};
    }

    leg_line_defaults(&sensor_file->leg_line);
}

void sensor_file_parse(SensorFile *sensor_file, const char *text)
{
    for_each_pair(text, sensor_key, sensor_file, false);
}

int leg_line_cover_percent(const LegLineParams *par) { return (int)(par->cover * 100.0 + 0.5); }

const char *leg_line_mode_name(const LegLineParams *par)
{
    return par->extended ? LINE_MODE_EXTENDED : LINE_MODE_STANDARD;
}

const char *move_method_name(MoveMethod method) { return method == MOVE_METHOD_POS ? "MV_POS" : "MV_REF"; }

// The columns of die.dat: name, rear and front position, then x and y which are not used
typedef enum { DIE_COLUMN_NAME, DIE_COLUMN_REAR, DIE_COLUMN_FRONT, DIE_COLUMN_X, DIE_COLUMN_Y, DIE_COLUMNS } DieColumn;

int dies_parse(const char *text, DieEntry *dies, int max)
{
    int n_dies = 0;
    while (*text && n_dies < max) {
        char line[FILE_LINE_MAX];
        size_t line_len = strcspn(text, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)(line_len < sizeof(line) ? line_len : sizeof(line) - 1), text);
        text += line_len + (text[line_len] == '\n');
        line[strcspn(line, "\r")] = '\0';

        char *columns[DIE_COLUMNS] = {0};
        int n_columns = 0;
        for (char *rest = line; n_columns < DIE_COLUMNS && rest; n_columns++) {
            columns[n_columns] = rest;
            rest = strchr(rest, ',');

            if (rest) {
                *rest++ = '\0';
            }
        }

        const char *name = columns[DIE_COLUMN_NAME];
        size_t name_len = strlen(name);
        if (n_columns <= DIE_COLUMN_FRONT || name_len < 1 || name_len > DIE_NAME_MAX) {
            continue;
        }

        const char *rear_text = columns[DIE_COLUMN_REAR], *front_text = columns[DIE_COLUMN_FRONT];
        char *rear_end, *front_end;
        long rear = strtol(rear_text, &rear_end, 10), front = strtol(front_text, &front_end, 10);
        if (!*rear_text || *rear_end || !*front_text || *front_end) {
            continue;
        }

        snprintf(dies[n_dies].name, sizeof(dies[n_dies].name), "%s", name);
        dies[n_dies].rear = (int32_t)rear;
        dies[n_dies].front = (int32_t)front;
        n_dies++;
    }

    return n_dies;
}

size_t dies_format(const DieEntry *dies, int n_dies, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < n_dies && len < size; i++) {
        int written =
            snprintf(buf + len, size - len, "%s,%d,%d,0,0\n", dies[i].name, (int)dies[i].rear, (int)dies[i].front);
        if (written < 0) {
            break;
        }

        len += (size_t)written;
    }

    return len < size ? len : size - 1;
}

void config_describe(const SetupFile *setup, const SensorFile *sensor_file, char *buf, size_t size)
{
    const CtlSetup *ctl = &setup->ctl;
    const SensorRegs *rear = &sensor_file->regs[UNIT_REAR], *front = &sensor_file->regs[UNIT_FRONT];
    const LegLineParams *par = &sensor_file->leg_line;
    snprintf(buf, size,
             "setup: %s, move down at start %d, secure angle %d, use turn %d, SB %d/%d, sensors: light "
             "filter %d, internal control %d, rear %d/%d, front %d/%d, line %s fix %d min %d cover %d tol %g",
             move_method_name(ctl->move_method), ctl->move_down_start, ctl->secure_angle, ctl->use_turn,
             setup->sb_timeout_s, setup->sb_meantilt, rear->light_filter, rear->internal_control, rear->shutter,
             rear->gain, front->shutter, front->gain, leg_line_mode_name(par), par->last_section_points,
             par->min_points, leg_line_cover_percent(par), (double)par->tolerance);
}
