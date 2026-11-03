#include <string.h>

#include "check.h"
#include "config.h"

static void test_setup_file_parse_and_format(void)
{
    SetupFile setup;
    char buf[1024];
    setup_file_defaults(&setup);
    const CtlSetup *ctl = &setup.ctl;
    const LegLineParams *par = &setup.leg_line;
    CHECK(ctl->move_method == MOVE_METHOD_REF && ctl->move_down_start && !ctl->secure_angle && !ctl->use_turn);
    CHECK(setup.sb_timeout_s == 10 && setup.sb_meantilt == 10 && par->last_section_points == 60 &&
          leg_line_cover_percent(par) == 99);

    setup_file_parse(&setup,
                     "MV_METHOD = MV_POS\r\nMOVE_DOWN_START=FALSE\nUSE_SECURE_ANGLE=TRUE\nUSE_TURN=TRUE\n"
                     "UNKNOWN=5\nSB_TIMEOUT=x\nLE_TOLERANCE=0.25\nLE_LINE_MODE=LM_STANDARD\n",
                     false);
    CHECK(ctl->move_method == MOVE_METHOD_POS && !ctl->move_down_start && ctl->secure_angle && ctl->use_turn);
    CHECK(setup.sb_timeout_s == 10 && par->tolerance == 0.25f && !par->extended);

    // Invalid numbers from the service page get the form defaults
    setup_file_parse(
        &setup, "SB_TIMEOUT=abc\nSB_MEANTILT=\nLE_POINTS_FIXL3=500\nLE_POINTS_MIN=x\nLE_COVER=-3\nLE_TOLERANCE=0\n",
        true);
    CHECK(setup.sb_timeout_s == 10 && setup.sb_meantilt == 7 && par->last_section_points == 60 &&
          par->min_points == 50);
    CHECK(leg_line_cover_percent(par) == 99 && par->tolerance == 0.4f);

    const char *expected =
        "MV_METHOD=MV_POS\nMOVE_DOWN_START=FALSE\nUSE_SECURE_ANGLE=TRUE\nUSE_TURN=TRUE\nSB_TIMEOUT=10\n"
        "SB_MEANTILT=7\nLE_POINTS_FIXL3=60\nLE_POINTS_MIN=50\nLE_COVER=99\nLE_TOLERANCE=0.4\n"
        "LE_LINE_MODE=LM_STANDARD\n";
    setup_file_format(&setup, buf, sizeof(buf));
    CHECK(strcmp(buf, expected) == 0);
}

static void test_sensor_file_parse(void)
{
    SensorFile sensor_file;
    const SensorRegs *rear = &sensor_file.regs[UNIT_REAR], *front = &sensor_file.regs[UNIT_FRONT];
    const LegLineParams *par = &sensor_file.leg_line;
    sensor_file_defaults(&sensor_file);
    CHECK(rear->light_filter && !rear->internal_control && rear->shutter == 310 && front->gain == 330);

    sensor_file_parse(&sensor_file,
                      "SENSOR_1_SHUTTERTIME=500\nSENSOR_2_VIDEOGAIN=200\nSENSOR_3_SHUTTERTIME=9\nLE_POINTS_FIXL3=40\n"
                      "LE_LINE_MODE=LM_STANDARD\nUSE_EXTERNAL_LIGHT_FILTER=FALSE\n");
    CHECK(rear->shutter == 500 && front->gain == 200 && front->shutter == 310);
    CHECK(!rear->light_filter && !front->light_filter);
    CHECK(par->last_section_points == 40 && par->min_points == 50 && par->cover == 0.99f && par->tolerance == 0.4f &&
          !par->extended);

    // Invalid values in the file keep the defaults
    sensor_file_defaults(&sensor_file);
    sensor_file_parse(&sensor_file, "LE_POINTS_FIXL3=0\nLE_POINTS_MIN=120\nLE_COVER=250\nLE_TOLERANCE=-1\n");
    CHECK(par->last_section_points == 60 && par->min_points == 50 && par->cover == 0.99f && par->tolerance == 0.4f &&
          par->extended);
}

static void test_die_file_parse_and_format(void)
{
    DieEntry dies[DIE_MAX];
    char buf[256];
    int n_dies = dies_parse("V16,1500,1700\nTOOLONGNAME12,1,2\nbad\nW8,100,200,5,6\n,1,2\nX,1a,2\n", dies, DIE_MAX);
    CHECK(n_dies == 2);
    CHECK(strcmp(dies[0].name, "V16") == 0 && dies[0].rear == 1500 && dies[0].front == 1700);
    CHECK(strcmp(dies[1].name, "W8") == 0 && dies[1].rear == 100 && dies[1].front == 200);

    dies_format(dies, n_dies, buf, sizeof(buf));
    CHECK(strcmp(buf, "V16,1500,1700,0,0\nW8,100,200,0,0\n") == 0);
    CHECK(dies_parse("A,1,1\nB,2,2\nC,3,3\n", dies, 2) == 2);
}

int main(void)
{
    test_setup_file_parse_and_format();
    test_sensor_file_parse();
    test_die_file_parse_and_format();
    return check_report("config");
}
