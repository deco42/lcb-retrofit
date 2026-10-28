#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "x87.h"

#define LINE_MAX_LEN 256

typedef void (*KeyHandler)(void *ctx, const char *key, const char *value, bool form);

// Calls h for every KEY=VALUE line with all white space removed
static void for_each_pair(const char *text, KeyHandler h, void *ctx, bool form)
{
    while (text && *text) {
        char line[LINE_MAX_LEN];
        size_t n = 0;
        for (; *text && *text != '\n'; text++) {
            if (!isspace((unsigned char)*text) && n < sizeof(line) - 1) {
                line[n++] = *text;
            }
        }

        if (*text == '\n') {
            text++;
        }

        line[n] = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) {
            continue;
        }

        *eq = 0;
        h(ctx, line, eq + 1, form);
    }
}

static void parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "TRUE") || !strcmp(v, "1")) {
        *out = true;
    } else if (!strcasecmp(v, "FALSE") || !strcmp(v, "0")) {
        *out = false;
    }
}

// Integer in [lo, hi] otherwise *out is set to the fallback (form) or kept
static void parse_int(const char *v, int *out, int lo, int hi, bool form, int fallback)
{
    char *end;
    long x = strtol(v, &end, 10);

    if (*v && !*end && x >= lo && x <= hi) {
        *out = (int)x;
    } else if (form) {
        *out = fallback;
    }
}

static float cover_fraction(int percent) { return (float)((ext)percent / 100); }

// LE_* keys, shared by setup.dat and sensor.dat returns true if the key was one of them
static bool legline_key(LegLineParams *le, const char *k, const char *v, bool form)
{
    if (!strcmp(k, "LE_POINTS_FIXL3")) {
        parse_int(v, &le->fix, 1, 99, form, 60);
    } else if (!strcmp(k, "LE_POINTS_MIN")) {
        parse_int(v, &le->pmin, 1, 99, form, 50);
    } else if (!strcmp(k, "LE_COVER")) {
        int percent = -1;
        parse_int(v, &percent, 0, 199, form, 99);
        if (percent >= 0) {
            le->cover = cover_fraction(percent);
        }
    } else if (!strcmp(k, "LE_TOLERANCE")) {
        char *end;
        double x = strtod(v, &end);
        if (*v && !*end && x > 0) {
            le->tol = (float)x;
        } else if (form) {
            le->tol = 0.4f;
        }
    } else if (!strcmp(k, "LE_LINE_MODE")) {
        le->extended = !strcmp(v, "LM_EXTENDED");
    } else {
        return false;
    }

    return true;
}

static void legline_defaults(LegLineParams *le)
{
    le->fix = 60;
    le->pmin = 50;
    le->cover = cover_fraction(99);
    le->tol = 0.4f;
    le->extended = true;
}

int legline_cover_percent(const LegLineParams *p) { return (int)(p->cover * 100.0 + 0.5); }

const char *legline_mode_name(const LegLineParams *p) { return p->extended ? "LM_EXTENDED" : "LM_STANDARD"; }

void setup_defaults(SetupFile *s)
{
    memset(s, 0, sizeof(*s));
    s->ctl.move_method = MV_REF;
    s->ctl.move_down_start = true;
    s->sb_timeout = 10;
    s->sb_meantilt = 10;
    legline_defaults(&s->le);
}

static void setup_key(void *ctx, const char *k, const char *v, bool form)
{
    SetupFile *s = ctx;
    if (!strcmp(k, "MV_METHOD")) {
        if (!strcmp(v, "MV_REF")) {
            s->ctl.move_method = MV_REF;
        } else if (!strcmp(v, "MV_POS")) {
            s->ctl.move_method = MV_POS;
        }
    } else if (!strcmp(k, "MOVE_DOWN_START")) {
        parse_bool(v, &s->ctl.move_down_start);
    } else if (!strcmp(k, "USE_SECURE_ANGLE")) {
        parse_bool(v, &s->ctl.secure_angle);
    } else if (!strcmp(k, "USE_TURN")) {
        parse_bool(v, &s->ctl.use_turn);
    } else if (!strcmp(k, "SB_TIMEOUT")) {
        parse_int(v, &s->sb_timeout, 1, 100000, form, 10);
    } else if (!strcmp(k, "SB_MEANTILT")) {
        parse_int(v, &s->sb_meantilt, 1, 100000, form, 7);
    } else {
        legline_key(&s->le, k, v, form);
    }
}

void setup_parse(SetupFile *s, const char *text, bool form) { for_each_pair(text, setup_key, s, form); }

size_t setup_format(const SetupFile *s, char *buf, size_t len)
{
    int n = snprintf(buf, len,
                     "MV_METHOD=%s\nMOVE_DOWN_START=%s\nUSE_SECURE_ANGLE=%s\nSB_TIMEOUT=%d\nSB_MEANTILT=%d\n"
                     "LE_POINTS_FIXL3=%d\nLE_POINTS_MIN=%d\nLE_COVER=%d\nLE_TOLERANCE=%g\nLE_LINE_MODE=%s\n",
                     s->ctl.move_method == MV_POS ? "MV_POS" : "MV_REF", s->ctl.move_down_start ? "TRUE" : "FALSE",
                     s->ctl.secure_angle ? "TRUE" : "FALSE", s->sb_timeout, s->sb_meantilt, s->le.fix, s->le.pmin,
                     legline_cover_percent(&s->le), (double)s->le.tol, legline_mode_name(&s->le));

    return n < 0 ? 0 : (size_t)n;
}

void sensor_defaults(SensorFile *s)
{
    memset(s, 0, sizeof(*s));

    for (int i = 0; i < 2; i++) {
        s->regs[i] = (SensorRegs){.light_filter = true, .internal_ctrl = false, .shutter = 310, .gain = 330};
    }

    legline_defaults(&s->le);
}

static void sensor_key(void *ctx, const char *k, const char *v, bool form)
{
    SensorFile *s = ctx;
    SensorRegs *r = s->regs;
    (void)form;

    // the two USE_* switches hold for both sensors
    if (!strcmp(k, "USE_EXTERNAL_LIGHT_FILTER")) {
        parse_bool(v, &r[0].light_filter);
        r[1].light_filter = r[0].light_filter;
    } else if (!strcmp(k, "USE_INTERNAL_SENSOR_CTRL")) {
        parse_bool(v, &r[0].internal_ctrl);
        r[1].internal_ctrl = r[0].internal_ctrl;
    } else if (!strcmp(k, "SENSOR_1_SHUTTERTIME")) {
        parse_int(v, &r[0].shutter, 0, 1023, false, 0);
    } else if (!strcmp(k, "SENSOR_1_VIDEOGAIN")) {
        parse_int(v, &r[0].gain, 0, 1023, false, 0);
    } else if (!strcmp(k, "SENSOR_2_SHUTTERTIME")) {
        parse_int(v, &r[1].shutter, 0, 1023, false, 0);
    } else if (!strcmp(k, "SENSOR_2_VIDEOGAIN")) {
        parse_int(v, &r[1].gain, 0, 1023, false, 0);
    } else {
        legline_key(&s->le, k, v, false);
    }
}

void sensor_parse(SensorFile *s, const char *text) { for_each_pair(text, sensor_key, s, false); }

int dies_parse(const char *text, DieEntry *out, int max)
{
    int n = 0;
    while (text && *text && n < max) {
        char line[LINE_MAX_LEN];
        size_t len = strcspn(text, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)(len < sizeof(line) ? len : sizeof(line) - 1), text);
        text += len + (text[len] == '\n');
        line[strcspn(line, "\r")] = 0;

        char *f[5] = {0};
        int nf = 0;
        for (char *p = line; nf < 5 && p; nf++) {
            f[nf] = p;
            p = strchr(p, ',');

            if (p) {
                *p++ = 0;
            }
        }

        size_t nl = f[0] ? strlen(f[0]) : 0;
        if (nf < 3 || nl < 1 || nl > DIE_NAME_MAX) {
            continue;
        }

        char *e1, *e2;
        long rear = strtol(f[1], &e1, 10), front = strtol(f[2], &e2, 10);
        if (!*f[1] || *e1 || !*f[2] || *e2) {
            continue;
        }

        snprintf(out[n].name, sizeof(out[n].name), "%s", f[0]);
        out[n].rear = (int32_t)rear;
        out[n].front = (int32_t)front;
        n++;
    }

    return n;
}

size_t dies_format(const DieEntry *d, int n, char *buf, size_t len)
{
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < n && used < len; i++) {
        int w = snprintf(buf + used, len - used, "%s,%d,%d,0,0\n", d[i].name, (int)d[i].rear, (int)d[i].front);
        if (w < 0) {
            break;
        }

        used += (size_t)w;
    }

    return used < len ? used : len - 1;
}

size_t config_describe(const SetupFile *s, const SensorFile *f, char *buf, size_t len)
{
    int n =
        snprintf(buf, len,
                 "setup: %s, move down at start %d, secure angle %d, use turn %d, SB %d/%d; sensors: light filter %d, "
                 "internal control %d, rear %d/%d, front %d/%d, line %s fix %d min %d cover %d tol %g",
                 s->ctl.move_method == MV_POS ? "MV_POS" : "MV_REF", s->ctl.move_down_start, s->ctl.secure_angle,
                 s->ctl.use_turn, s->sb_timeout, s->sb_meantilt, f->regs[0].light_filter, f->regs[0].internal_ctrl,
                 f->regs[0].shutter, f->regs[0].gain, f->regs[1].shutter, f->regs[1].gain, legline_mode_name(&f->le),
                 f->le.fix, f->le.pmin, legline_cover_percent(&f->le), (double)f->le.tol);

    return n < 0 ? 0 : (size_t)n;
}
