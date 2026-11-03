#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "nlr.h"

#define VECTOR_BYTES_MAX 32

static int run_vectors(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }

    NlrLink link;
    nlr_init(&link);
    char line[1024];
    int line_number = 0, n_vectors = 0, failed = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') {
            continue;
        }

        n_vectors++;
        char *token = strtok(line, " ");
        bool ok = true;
        if (strcmp(token, "angle") == 0) {
            link.angle = strtof(strtok(NULL, " "), NULL);
            link.both = atoi(strtok(NULL, " "));
            link.valid_rear = atoi(strtok(NULL, " "));
            link.valid_front = atoi(strtok(NULL, " "));
        } else if (strcmp(token, "flags") == 0) {
            strtok(NULL, " ");
            link.referenced = atoi(strtok(NULL, " "));
            strtok(NULL, " ");
            link.encoder_used = atoi(strtok(NULL, " "));
            strtok(NULL, " ");
            link.leg_check = atoi(strtok(NULL, " "));
        } else if (strcmp(token, "rx") == 0) {
            uint8_t byte = (uint8_t)strtol(strtok(NULL, " "), NULL, 16);
            strtok(NULL, " "); // "ack"
            int ack = atoi(strtok(NULL, " "));
            strtok(NULL, " "); // "tx"

            uint8_t expected[VECTOR_BYTES_MAX];
            int n_expected = 0;
            for (char *hex; (hex = strtok(NULL, " ")) && n_expected < VECTOR_BYTES_MAX;) {
                expected[n_expected++] = (uint8_t)strtol(hex, NULL, 16);
            }

            NlrReply reply;
            nlr_receive(&link, byte, &reply);
            ok = reply.ack == ack && reply.tx_len == n_expected && memcmp(reply.tx, expected, (size_t)n_expected) == 0;
        } else if (strcmp(token, "dnc") == 0) {
            uint8_t expected[NLR_DNC_MAX], received[NLR_DNC_MAX];
            int n_expected = 0;
            for (char *hex; (hex = strtok(NULL, " ")) && n_expected < NLR_DNC_MAX;) {
                expected[n_expected++] = (uint8_t)strtol(hex, NULL, 16);
            }

            int n_received = nlr_take_dnc(&link, received);
            ok = n_received == n_expected && memcmp(received, expected, (size_t)n_expected) == 0;
        } else {
            ok = false;
        }

        if (!ok) {
            fprintf(stderr, "%s:%d: vector failed\n", path, line_number);
            failed++;
        }
    }

    fclose(file);
    printf("CNC link vectors: %d lines, %d failed\n", n_vectors, failed);

    return failed;
}

static void test_angle_scaling(void)
{
    double per_degree = 1.0 / 90.0;
    uint64_t bits;
    memcpy(&bits, &per_degree, sizeof(bits));
    CHECK(bits == 0x3F86C16C16C16C17ull);
    CHECK(nlr_scale(90.0f, 0x800, 0x400) == 0x3FF); // exact product just below 1024
    CHECK(nlr_scale(180.0f, 1000, 2000) == 1000);
    CHECK(nlr_scale(0.0f, 1000, 2000) == 3000);
    CHECK(nlr_scale(0.0f, 0x800, 0x400) == 0x000);
}

static void test_line_supervision(void)
{
    LineSupervision sup = {0};
    bool changed, lines_on;
    lines_on = line_supervision_step(&sup, true, false, 0, &changed);
    CHECK(lines_on && !changed);
    lines_on = line_supervision_step(&sup, false, false, 10, &changed);
    CHECK(!lines_on && changed);
    lines_on = line_supervision_step(&sup, true, true, 1109, &changed);
    CHECK(!lines_on); // off for 1.1 s regardless of the lines
    lines_on = line_supervision_step(&sup, true, true, 1110, &changed);
    CHECK(lines_on && !changed);
    lines_on = line_supervision_step(&sup, false, false, 2209, &changed);
    CHECK(lines_on);
    lines_on = line_supervision_step(&sup, false, false, 2210, &changed); // still down: off again
    CHECK(!lines_on && !changed);
    lines_on = line_supervision_step(&sup, true, false, 3310, &changed);
    lines_on = line_supervision_step(&sup, true, false, 4410, &changed);
    CHECK(lines_on && changed && sup.state == LINE_OK);
}

static void test_dnc_block_filling_the_buffer(void)
{
    NlrLink link;
    NlrReply reply;
    nlr_init(&link);
    const uint8_t header[] = {0x30, 0x01, 0x00, 0x36, 0x00, 0x00, 0x08}; // 2048 words = 4096 bytes
    for (size_t i = 0; i < sizeof(header); i++) {
        nlr_receive(&link, header[i], &reply);
    }

    for (int i = 0; i < NLR_DNC_MAX; i++) {
        nlr_receive(&link, 'A', &reply);
    }

    // The byte count has three digits: 4096 is answered as 096
    CHECK(reply.tx_len == 13 && reply.tx[10] == '0' && reply.tx[11] == '9' && reply.tx[12] == '6');
    CHECK(link.dnc_ready && link.dnc_len == NLR_DNC_MAX && link.state == NLR_COMMAND);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : getenv("LCBD_VECTORS");
    if (!path) {
        path = "../testdata/cybelec_link.vectors.txt";
    }

    int failed = run_vectors(path);
    CHECK(failed == 0);
    test_angle_scaling();
    test_line_supervision();
    test_dnc_block_filling_the_buffer();
    return check_report("nlr");
}
