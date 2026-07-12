#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "zx_compressor.h"
#include "zx0/zx0.h"

/* The full optimal search is quadratic in the input size. The official quick mode uses this
 * window and is effectively indistinguishable in size for these Pro Tracker images, while
 * keeping conversion practical on a desktop build. */
#define ZX_MAX_SEARCH_OFFSET 2176

int zx0_compress_data(const uint8_t *input, uint32_t input_size, uint8_t **output,
                      uint32_t *output_size, uint32_t *delta) {
  ZX0_BLOCK *optimal;
  int compressed_size;
  int compressed_delta;

  if (!input || !output || !output_size || !delta || input_size < 3 || input_size > INT_MAX)
    return 0;

  optimal = zx0_optimize((unsigned char *)input, (int)input_size, 0, ZX_MAX_SEARCH_OFFSET);
  *output = zx0_compress_blocks(optimal, (unsigned char *)input, (int)input_size, 0, 0, 1,
                                &compressed_size, &compressed_delta);
  *output_size = (uint32_t)compressed_size;
  *delta = (uint32_t)compressed_delta;
  return 1;
}
