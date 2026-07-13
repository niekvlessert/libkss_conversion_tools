#ifndef ZX_DECOMPRESSOR_H
#define ZX_DECOMPRESSOR_H

#include <stdint.h>

/* Decode the current (non-classic) forward ZX0 stream into a caller-owned
 * output buffer of exactly output_size bytes. */
int zx0_decompress_data(const uint8_t *input, uint32_t input_size,
                        uint8_t *output, uint32_t output_size);

#endif
