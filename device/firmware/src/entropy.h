#ifndef ENTROPY_H
#define ENTROPY_H

#include <stdint.h>

/* Hardware RNG; leaves out unchanged on timeout. Never substitutes a PRNG. */
int entropy_boot_id(uint64_t *out);
/* Poll one random byte without waiting; -1 timeout, 0 pending, 1 ready. */
int entropy_poll(uint8_t *out);

#endif
