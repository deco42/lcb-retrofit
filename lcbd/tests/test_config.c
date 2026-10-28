// setup.dat, sensor.dat and die.dat parsing and formatting
#include <string.h>

#include "check.h"
#include "config.h"

static void test_setup(void)
{
    SetupFile s;
    char buf[1024];
    setup_defaults(&s);
    CHECK(s.ctl.move_method == MV_REF && s.ctl.move_down_start && !s.ctl.secure_angle && !s.ctl.use_turn);
    CHECK(s.sb_timeout == 10 && s.sb_meantilt == 10 && s.le.fix == 60 && legline_cover_percent(&s.le) == 99);

    setup_parse(&s,
                "MV_METHOD = MV_POS\r\nMOVE_DOWN_START=FALSE\nUSE_SECURE_ANGLE=TRUE\nUSE_TURN=TRUE\n"
                "UNKNOWN=5\nSB_TIMEOUT=x\nLE_TOLERANCE=0.25\nLE_LINE_MODE=LM_STANDARD\n",
                false);
    CHECK(s.ctl.move_method == MV_POS && !s.ctl.move_down_start && s.ctl.secure_angle && s.ctl.use_turn);
    CHECK(s.sb_timeout == 10 && s.le.tol == 0.25f && !s.le.extended);

    setup_parse(&s, "SB_TIMEOUT=abc\nSB_MEANTILT=\nLE_POINTS_FIXL3=500\nLE_POINTS_MIN=x\nLE_COVER=-3\nLE_TOLERANCE=0\n",
                true);
    CHECK(s.sb_timeout == 10 && s.sb_meantilt == 7 && s.le.fix == 60 && s.le.pmin == 50);
    CHECK(legline_cover_percent(&s.le) == 99 && s.le.tol == 0.4f);

    setup_format(&s, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "MV_METHOD=MV_POS\nMOVE_DOWN_START=FALSE\nUSE_SECURE_ANGLE=TRUE\nSB_TIMEOUT=10\nSB_MEANTILT=7\n"
                  "LE_POINTS_FIXL3=60\nLE_POINTS_MIN=50\nLE_COVER=99\nLE_TOLERANCE=0.4\nLE_LINE_MODE=LM_STANDARD\n"));
}

static void test_sensor(void)
{
    SensorFile f;
    sensor_defaults(&f);
    CHECK(f.regs[0].light_filter && !f.regs[0].internal_ctrl && f.regs[0].shutter == 310 && f.regs[1].gain == 330);
    sensor_parse(&f, "SENSOR_1_SHUTTERTIME=500\nSENSOR_2_VIDEOGAIN=200\nSENSOR_3_SHUTTERTIME=9\nLE_POINTS_FIXL3=40\n"
                     "LE_LINE_MODE=LM_STANDARD\nUSE_EXTERNAL_LIGHT_FILTER=FALSE\n");
    CHECK(f.regs[0].shutter == 500 && f.regs[1].gain == 200 && f.regs[1].shutter == 310);
    CHECK(!f.regs[0].light_filter && !f.regs[1].light_filter);
    CHECK(f.le.fix == 40 && f.le.pmin == 50 && f.le.cover == 0.99f && f.le.tol == 0.4f && !f.le.extended);

    sensor_defaults(&f); // invalid values in the file keep the defaults
    sensor_parse(&f, "LE_POINTS_FIXL3=0\nLE_POINTS_MIN=120\nLE_COVER=250\nLE_TOLERANCE=-1\n");
    CHECK(f.le.fix == 60 && f.le.pmin == 50 && f.le.cover == 0.99f && f.le.tol == 0.4f && f.le.extended);
}

static void test_dies(void)
{
    DieEntry d[DIE_MAX];
    char buf[256];
    int n = dies_parse("V16,1500,1700\nTOOLONGNAME12,1,2\nbad\nW8,100,200,5,6\n,1,2\nX,1a,2\n", d, DIE_MAX);
    CHECK(n == 2);
    CHECK(!strcmp(d[0].name, "V16") && d[0].rear == 1500 && d[0].front == 1700);
    CHECK(!strcmp(d[1].name, "W8") && d[1].rear == 100 && d[1].front == 200);
    dies_format(d, n, buf, sizeof buf);
    CHECK(!strcmp(buf, "V16,1500,1700,0,0\nW8,100,200,0,0\n"));
    CHECK(dies_parse("A,1,1\nB,2,2\nC,3,3\n", d, 2) == 2);
}

int main(void)
{
    test_setup();
    test_sensor();
    test_dies();
    return check_report("config");
}
