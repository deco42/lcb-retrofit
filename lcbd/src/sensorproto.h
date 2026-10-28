// Laser sensor protocol
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

#define SENSOR_CMD_MAX 32
#define FRAME_RESYNC_INVALID 10

typedef enum { SE_CONNECTED, SE_START_SCAN, SE_STOP_SCAN, SE_START_REFERENCE } SensorEvent;

size_t sensor_command(const SensorRegs *r, SensorEvent ev, uint8_t out[SENSOR_CMD_MAX]);

typedef enum { FRAME_NONE, FRAME_VALID, FRAME_INVALID, FRAME_RESYNC } FrameStatus;

typedef struct {
    uint8_t frame[FRAME_SIZE];
    size_t fill;
    int invalid_run;
} FrameAssembler;

void frame_assembler_reset(FrameAssembler *a);
FrameStatus frame_assemble(FrameAssembler *a, const uint8_t **data, size_t *len);

#endif
