#ifndef LCBD_BYTES_H
#define LCBD_BYTES_H

#include <stdint.h>

static inline uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

static inline uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

#endif
