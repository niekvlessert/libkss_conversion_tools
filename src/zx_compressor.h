#ifndef ZX_COMPRESSOR_H
#define ZX_COMPRESSOR_H

#include <stdint.h>

int zx0_compress_data(const uint8_t *input, uint32_t input_size, uint8_t **output,
                      uint32_t *output_size, uint32_t *delta);

#endif
