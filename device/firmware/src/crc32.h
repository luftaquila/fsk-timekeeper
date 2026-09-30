/* CRC-32/IEEE (reflected poly 0xEDB88320, init and xorout 0xFFFFFFFF). */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

uint32_t crc32_ieee(const void *data, uint32_t len);

#endif /* CRC32_H */
