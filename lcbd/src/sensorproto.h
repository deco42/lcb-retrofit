#ifndef LCBD_SENSORPROTO_H
#define LCBD_SENSORPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Sensor frame: 2048 bytes, the measuring points start at 0x42, 290 records of 5 bytes
#define FRAME_SIZE 2048
#define FRAME_RECORDS 290
#define FRAME_POINTS_OFFSET 0x42
#define FRAME_RECORD_SIZE 5
#define FRAME_X_SCALE 1.5     // the profile x is the record x divided by this
#define FRAME_ZERO_FIRST 0x34 // header bytes that are 0 in every valid frame, a stream out of step has other data here
#define FRAME_ZERO_LAST 0x3B
#define FRAME_RESYNC_INVALID 10 // this many invalid frames in a row: the stream is out of step

// Sensor commands: a register write is the register (bit 7 = 0) and the value (bit 7 = 1, 7 bits)
#define SENSOR_REG_SHUTTER 0x00         // low 7 bits, bits 7 to 9 follow in the next register
#define SENSOR_REG_GAIN 0x06            // as SENSOR_REG_SHUTTER
#define SENSOR_REG_LIGHT_FILTER 0x0B    // 1 = on
#define SENSOR_REG_FIXED 0x15           // 1 = fixed shutter and gain, 0 = the sensor controls its exposure
#define SENSOR_CMD_START_STREAMING 0x1C // single byte
#define SENSOR_CMD_MAX 32

#define SENSOR_DEFAULT_REAR "192.168.123.248"
#define SENSOR_DEFAULT_FRONT "192.168.123.249"
#define SENSOR_DEFAULT_PORT 3000

typedef enum { SENSOR_CONNECTED, SENSOR_START_SCAN, SENSOR_STOP_SCAN, SENSOR_START_REFERENCE } SensorEvent;

size_t sensor_command(const SensorRegs *regs, SensorEvent event, uint8_t cmd[SENSOR_CMD_MAX]);

typedef enum { FRAME_NONE, FRAME_VALID, FRAME_INVALID, FRAME_RESYNC } FrameStatus;

typedef struct {
    uint8_t frame[FRAME_SIZE];
    size_t frame_len;
    int invalid_run; // invalid frames in a row
} FrameAssembler;

void frame_assembler_reset(FrameAssembler *fa);
FrameStatus frame_assemble(FrameAssembler *fa, const uint8_t **data, size_t *len);

// A record is x (2 bytes), y (2 bytes) and a quality byte, each coordinate as low + 128 * high
static inline const uint8_t *frame_record(const uint8_t *frame, int i)
{
    return frame + FRAME_POINTS_OFFSET + FRAME_RECORD_SIZE * i;
}

static inline int record_x(const uint8_t *record) { return record[0] + 128 * record[1]; }

static inline int record_y(const uint8_t *record) { return record[2] + 128 * record[3]; }

static inline uint8_t record_quality(const uint8_t *record) { return record[4]; }

#endif
