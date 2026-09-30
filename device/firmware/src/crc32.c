#include "crc32.h"

/* Bitwise, table-less: used for keystore checks and ~1 line per uplink. */
uint32_t crc32_ieee(const void *data, uint32_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return c ^ 0xFFFFFFFFu;
}
