#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "nlr.h"

static int run_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }

    NlrLink l;
    nlr_init(&l);
    char line[1024];
    int lineno = 0, lines = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') {
            continue;
        }

        lines++;
        char *tok = strtok(line, " ");
        bool ok = true;
        if (!strcmp(tok, "angle")) {
            l.angle = strtof(strtok(NULL, " "), NULL);
            l.both = atoi(strtok(NULL, " "));
            l.valid_rear = atoi(strtok(NULL, " "));
            l.valid_front = atoi(strtok(NULL, " "));
        } else if (!strcmp(tok, "flags")) {
            strtok(NULL, " ");
            l.referenced = atoi(strtok(NULL, " "));
            strtok(NULL, " ");
            l.encoder_used = atoi(strtok(NULL, " "));
            strtok(NULL, " ");
            l.leg_check = atoi(strtok(NULL, " "));
        } else if (!strcmp(tok, "rx")) {
            uint8_t b = (uint8_t)strtol(strtok(NULL, " "), NULL, 16);
            strtok(NULL, " "); // "ack"
            int ack = atoi(strtok(NULL, " "));
            strtok(NULL, " "); // "tx"

            uint8_t want[32];
            int nw = 0;
            for (char *t; (t = strtok(NULL, " ")) && nw < 32;) {
                want[nw++] = (uint8_t)strtol(t, NULL, 16);
            }

            NlrReply rep;
            nlr_receive(&l, b, &rep);
            ok = rep.ack == ack && rep.ntx == nw && !memcmp(rep.tx, want, (size_t)nw);
        } else if (!strcmp(tok, "dnc")) {
            uint8_t want[NLR_DNC_MAX], got[NLR_DNC_MAX];
            int nw = 0;
            for (char *t; (t = strtok(NULL, " ")) && nw < NLR_DNC_MAX;) {
                want[nw++] = (uint8_t)strtol(t, NULL, 16);
            }

            int ng = nlr_take_dnc(&l, got, sizeof(got));
            ok = ng == nw && !memcmp(got, want, (size_t)nw);
        } else {
            ok = false;
        }

        if (!ok) {
            fprintf(stderr, "%s:%d: vector failed\n", path, lineno);
            bad++;
        }
    }

    fclose(f);
    printf("CNC link vectors: %d lines, %d failed\n", lines, bad);

    return bad;
}

static void test_scaling(void)
{
    double k = 1.0 / 90.0;
    uint64_t bits;
    memcpy(&bits, &k, 8);
    CHECK(bits == 0x3F86C16C16C16C17ull);
    CHECK(nlr_scale(90.0f, 0x800, 0x400) == 0x3FF); // exact product just below 1024
    CHECK(nlr_scale(180.0f, 1000, 2000) == 1000);
    CHECK(nlr_scale(0.0f, 1000, 2000) == 3000);
    CHECK(nlr_scale(0.0f, 0x800, 0x400) == 0x000);
}

static void test_linesup(void)
{
    LineSup s = {0};
    bool changed, on;
    on = linesup_step(&s, true, false, 0, &changed);
    CHECK(on && !changed);
    on = linesup_step(&s, false, false, 10, &changed);
    CHECK(!on && changed);
    on = linesup_step(&s, true, true, 1109, &changed);
    CHECK(!on); // off for 1.1 s regardless of the lines
    on = linesup_step(&s, true, true, 1110, &changed);
    CHECK(on && !changed);
    on = linesup_step(&s, false, false, 2209, &changed);
    CHECK(on);
    on = linesup_step(&s, false, false, 2210, &changed); // still down: off again
    CHECK(!on && !changed);
    on = linesup_step(&s, true, false, 3310, &changed);
    on = linesup_step(&s, true, false, 4410, &changed);
    CHECK(on && changed && s.state == LS_OK);
}

static void test_dnc_overflow(void)
{
    NlrLink l;
    NlrReply rep;
    nlr_init(&l);
    const uint8_t head[] = {0x30, 0x01, 0x00, 0x36, 0x00, 0x00, 0x08}; // 2048 words = 4096 bytes
    for (size_t i = 0; i < sizeof(head); i++) {
        nlr_receive(&l, head[i], &rep);
    }

    for (int i = 0; i < 4096; i++) {
        nlr_receive(&l, 'A', &rep);
    }

    CHECK(rep.ntx == 13 && rep.tx[10] == '0' && rep.tx[11] == '9' && rep.tx[12] == '6');
    CHECK(l.dnc_ready && l.dnc_size == NLR_DNC_MAX && l.state == NLR_CMD);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : getenv("LCBD_VECTORS");
    if (!path) {
        path = "./testdata/cybelec_link.vectors.txt";
    }

    int bad = run_vectors(path);
    CHECK(bad == 0);
    test_scaling();
    test_linesup();
    test_dnc_overflow();
    return check_report("nlr");
}
