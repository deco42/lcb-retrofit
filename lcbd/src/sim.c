#include "sim.h"

#include <string.h>

#define AT_REFERENCE_POS 0.5 // the reference switch opens above this position (pulses)

#define PULSES_PER_MS 1       // drive speed: 1000 pulses/s
#define END_POS 6000          // mechanical end of the drives (pulses)
#define COAST_AFTER_RUN_MS 50 // a drive switched off after a longer run coasts for coast_ms
#define CYLINDER_TRAVEL_MS 800

void machine_init(Machine *m, double start_pos)
{
    memset(m, 0, sizeof(*m));
    m->pos[UNIT_REAR] = m->pos[UNIT_FRONT] = start_pos;
    m->cylinder_target = CYLINDER_OFF;
}

uint16_t machine_inputs(const Machine *m)
{
    uint16_t inputs = m->extra_inputs;
    if (m->cylinder_down && !m->cylinder_stuck) {
        inputs |= BIT(IN_CYLINDER_DOWN);
    }

    if (m->pos[UNIT_REAR] > AT_REFERENCE_POS) {
        inputs |= BIT(IN_REF_REAR);
    }

    if (m->pos[UNIT_FRONT] > AT_REFERENCE_POS) {
        inputs |= BIT(IN_REF_FRONT);
    }

    return inputs;
}

void machine_step(Machine *m, uint16_t outputs, uint32_t now_ms)
{
    for (int unit = 0; unit < UNITS; unit++) {
        bool forward = outputs & BIT(IO_FORWARD_OUTPUT[unit]), back = outputs & BIT(IO_BACK_OUTPUT[unit]);
        int direction = 0;
        if (forward != back) { // both on = standing
            direction = forward ? 1 : -1;
            m->run_ms[unit]++;
            m->coast_left_ms[unit] = 0;
        } else {
            if (m->run_ms[unit] > COAST_AFTER_RUN_MS) {
                m->coast_left_ms[unit] = m->coast_ms;
            }

            m->run_ms[unit] = 0;
            if (m->coast_left_ms[unit] > 0) {
                m->coast_left_ms[unit]--;
                direction = m->last_direction[unit];
            }
        }

        if (direction != 0) {
            double pos = m->pos[unit] + direction * PULSES_PER_MS;
            m->last_direction[unit] = direction;
            if (pos < 0) {
                pos = 0;
            }

            if (pos > END_POS) {
                pos = END_POS;
            }

            if (pos != m->pos[unit]) {
                m->raw[unit] += PULSES_PER_MS; // the EL1502 counts every pulse up
            }

            m->pos[unit] = pos;
        }
    }

    bool down_on = outputs & BIT(OUT_CYLINDER_DOWN), up_on = outputs & BIT(OUT_CYLINDER_UP);
    CylinderOutput valve = down_on == up_on ? CYLINDER_OFF : down_on ? CYLINDER_DOWN : CYLINDER_UP;
    if (valve != CYLINDER_OFF && valve != m->cylinder_target) {
        m->cylinder_target = valve;
        m->cylinder_ms = now_ms;
    }

    if (m->cylinder_target != CYLINDER_OFF && now_ms - m->cylinder_ms >= CYLINDER_TRAVEL_MS) {
        m->cylinder_down = m->cylinder_target == CYLINDER_DOWN;
    }
}
