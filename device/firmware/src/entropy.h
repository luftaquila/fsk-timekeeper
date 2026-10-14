#ifndef ENTROPY_H
#define ENTROPY_H

#include <stdint.h>

/* Hardware RNG; leaves out unchanged on timeout. Never substitutes a PRNG. */
int entropy_boot_id(uint64_t *out);

#endif
