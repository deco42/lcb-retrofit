#include "ecat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"
#include "soem/soem.h"

#define BECKHOFF_VENDOR_ID 0x00000002u
#define IO_MAP_SIZE 512 // bytes, more than the process data of all terminals

// EL1502 in the mode "2Ch. Counter": per channel a 16 bit status or control word and a 32 bit counter value
#define EL1502_CHANNELS 2
#define EL1502_CHANNEL_BYTES 6
#define EL1502_COUNTER_OFFSET 2
#define EL1502_BYTES (EL1502_CHANNELS * EL1502_CHANNEL_BYTES)

// Terminals in bus order, SOEM slave 0 is the whole bus
enum { TERMINAL_EK1100 = 1, TERMINAL_EL1819, TERMINAL_EL2809, TERMINAL_EL1502, TERMINALS = TERMINAL_EL1502 };

static const struct {
    uint32_t product_code;
    const char *name;
    int input_bytes, output_bytes;
} EXPECTED[TERMINALS + 1] = {
    [TERMINAL_EK1100] = {0x044c2c52, "EK1100", 0, 0},
    [TERMINAL_EL1819] = {0x071b3052, "EL1819", 2, 0},
    [TERMINAL_EL2809] = {0x0af93052, "EL2809", 0, 2},
    [TERMINAL_EL1502] = {0x05de3052, "EL1502", EL1502_BYTES, EL1502_BYTES},
};

struct Ecat {
    ecx_contextt context;
    uint8 io_map[IO_MAP_SIZE];
    int expected_working_counter;
};

// CoE objects of the EL1502, subindex 0 of a PDO assignment is the number of assigned PDOs
#define SM2_PDO_ASSIGN 0x1C12  // output PDOs
#define SM3_PDO_ASSIGN 0x1C13  // input PDOs
#define EL1502_SETTINGS 0x8000 // "CNT Settings" of channel 1, the next channel at +EL1502_SETTINGS_STEP
#define EL1502_SETTINGS_STEP 0x10
#define EL1502_COUNT_DOWN 4 // subindex of the settings, 0 = count up

// PDO mappings of the channels in the mode "2Ch. Counter"
static const struct {
    uint16 assign_index;
    uint16 pdo[EL1502_CHANNELS];
} PDO_ASSIGNMENT[] = {{SM2_PDO_ASSIGN, {0x1600, 0x1601}}, {SM3_PDO_ASSIGN, {0x1A00, 0x1A01}}};

// EL1502 in PRE-OP: PDO assignment "2Ch. Counter" instead of the default "1Ch. +/- Counter", both counters counting
// up. The software applies the direction
static int el1502_setup(ecx_contextt *context, uint16 slave)
{
    uint8 zero = 0, n_pdos = EL1502_CHANNELS;
    bool ok = true;
    for (size_t k = 0; k < sizeof(PDO_ASSIGNMENT) / sizeof(PDO_ASSIGNMENT[0]); k++) {
        uint16 index = PDO_ASSIGNMENT[k].assign_index;
        ok &= ecx_SDOwrite(context, slave, index, 0, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) > 0;
        for (int i = 0; i < EL1502_CHANNELS; i++) {
            const uint16 *pdo = &PDO_ASSIGNMENT[k].pdo[i];
            ok &= ecx_SDOwrite(context, slave, index, (uint8)(i + 1), FALSE, sizeof(*pdo), pdo, EC_TIMEOUTRXM) > 0;
        }

        ok &= ecx_SDOwrite(context, slave, index, 0, FALSE, sizeof(n_pdos), &n_pdos, EC_TIMEOUTRXM) > 0;
    }

    for (int i = 0; i < EL1502_CHANNELS; i++) {
        uint16 index = (uint16)(EL1502_SETTINGS + i * EL1502_SETTINGS_STEP);
        ok &= ecx_SDOwrite(context, slave, index, EL1502_COUNT_DOWN, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) > 0;
    }

    if (!ok) {
        fprintf(stderr, "EL1502: CoE configuration failed\n");
    }

    return ok;
}

static Ecat *fail(Ecat *ecat, char *err, size_t err_size, const char *reason)
{
    char found[1024];
    ecat_describe(ecat, found, sizeof(found));
    snprintf(err, err_size, "%s\nfound:\n%s", reason, found);
    ecx_close(&ecat->context);
    free(ecat);
    return NULL;
}

#define SAFE_OP_TIMEOUT_US (EC_TIMEOUTSTATE * 4)
#define OPERATIONAL_ATTEMPTS 40
#define OPERATIONAL_CHECK_US (EC_TIMEOUTSTATE / 10)

static bool reach_operational(Ecat *ecat)
{
    ecx_contextt *context = &ecat->context;
    ecx_statecheck(context, 0, EC_STATE_SAFE_OP, SAFE_OP_TIMEOUT_US);
    ecx_send_processdata(context);
    ecx_receive_processdata(context, EC_TIMEOUTRET);
    context->slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(context, 0);
    for (int i = 0; i < OPERATIONAL_ATTEMPTS; i++) {
        ecx_send_processdata(context);
        ecx_receive_processdata(context, EC_TIMEOUTRET);
        ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, OPERATIONAL_CHECK_US);
        if (context->slavelist[0].state == EC_STATE_OPERATIONAL) {
            return true;
        }
    }

    return false;
}

#define TERMINAL_ORDER "(expected: EK1100, EL1819, EL2809, EL1502)"

Ecat *ecat_open(const char *interface_name, char *err, size_t err_size)
{
    Ecat *ecat = calloc(1, sizeof(*ecat));
    if (!ecat) {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }

    ecx_contextt *context = &ecat->context;
    if (!ecx_init(context, interface_name)) {
        snprintf(err, err_size, "cannot open network port %s (name? root or CAP_NET_RAW?)", interface_name);
        free(ecat);
        return NULL;
    }

    if (ecx_config_init(context) <= 0) {
        return fail(ecat, err, err_size, "no EtherCAT terminals found (cable at X1 of the EK1100?)");
    }

    ec_slavet *slaves = context->slavelist;
    if (context->slavecount != TERMINALS) {
        return fail(ecat, err, err_size, "wrong number of terminals " TERMINAL_ORDER);
    }

    for (int i = 1; i <= TERMINALS; i++) {
        if (slaves[i].eep_man != BECKHOFF_VENDOR_ID || slaves[i].eep_id != EXPECTED[i].product_code) {
            return fail(ecat, err, err_size, "wrong terminal or order " TERMINAL_ORDER);
        }
    }

    slaves[TERMINAL_EL1502].PO2SOconfig = el1502_setup;
    ecx_config_map_group(context, ecat->io_map, 0);
    for (int i = 1; i <= TERMINALS; i++) {
        if ((int)slaves[i].Ibytes != EXPECTED[i].input_bytes || (int)slaves[i].Obytes != EXPECTED[i].output_bytes) {
            return fail(ecat, err, err_size, "process data size not as expected (EL1502 not in 2 channel mode?)");
        }
    }

    // Logical read and write: every terminal with outputs adds 2 to the working counter, every one with inputs 1
    ec_groupt *group = &context->grouplist[0];
    ecat->expected_working_counter = group->outputsWKC * 2 + group->inputsWKC;

    if (reach_operational(ecat)) {
        return ecat;
    }

    ecx_readstate(context);
    for (int i = 1; i <= TERMINALS; i++) {
        if (slaves[i].state != EC_STATE_OPERATIONAL) {
            char reason[200];
            snprintf(reason, sizeof(reason), "%s does not reach OPERATIONAL: state 0x%04x, %s", EXPECTED[i].name,
                     slaves[i].state, ec_ALstatuscode2string(slaves[i].ALstatuscode));
            return fail(ecat, err, err_size, reason);
        }
    }

    return fail(ecat, err, err_size, "bus does not reach OPERATIONAL");
}

bool ecat_exchange(Ecat *ecat, uint16_t out, uint16_t *in, uint32_t raw[UNITS])
{
    ec_slavet *slaves = ecat->context.slavelist;
    uint8 *outputs = slaves[TERMINAL_EL2809].outputs;
    const uint8 *inputs = slaves[TERMINAL_EL1819].inputs;
    const uint8 *counters = slaves[TERMINAL_EL1502].inputs;
    outputs[0] = (uint8)out;
    outputs[1] = (uint8)(out >> 8);
    memset(slaves[TERMINAL_EL1502].outputs, 0, EL1502_BYTES); // no set, latch or output of the counters
    ecx_send_processdata(&ecat->context);
    int working_counter = ecx_receive_processdata(&ecat->context, EC_TIMEOUTRET);
    if (working_counter < ecat->expected_working_counter) {
        return false;
    }

    *in = le16(inputs);
    raw[UNIT_REAR] = le32(counters + EL1502_COUNTER_OFFSET);                         // channel 1
    raw[UNIT_FRONT] = le32(counters + EL1502_CHANNEL_BYTES + EL1502_COUNTER_OFFSET); // channel 2

    return true;
}

void ecat_close(Ecat *ecat)
{
    uint16_t in;
    uint32_t raw[UNITS];
    if (!ecat) {
        return;
    }

    ecat_exchange(ecat, 0, &in, raw);
    ecat_exchange(ecat, 0, &in, raw);
    ecat->context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&ecat->context, 0);
    ecx_close(&ecat->context);
    free(ecat);
}

void ecat_describe(const Ecat *ecat, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 1; i <= ecat->context.slavecount && len < size; i++) {
        const ec_slavet *slave = &ecat->context.slavelist[i];
        int n = snprintf(buf + len, size - len, "  %d: %-10s vendor 0x%08x product 0x%08x  in %u bytes  out %u bytes\n",
                         i, slave->name, (unsigned)slave->eep_man, (unsigned)slave->eep_id, (unsigned)slave->Ibytes,
                         (unsigned)slave->Obytes);
        if (n < 0) {
            break;
        }

        len += (size_t)n;
    }
}
