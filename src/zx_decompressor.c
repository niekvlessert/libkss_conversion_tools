#include "zx_decompressor.h"

#include <limits.h>
#include <stddef.h>

typedef struct {
  const uint8_t *input;
  uint32_t input_size;
  uint32_t input_index;
  uint8_t bit_mask;
  uint8_t bit_value;
  uint8_t last_byte;
  int backtrack;
} ZX0_READER;

static int read_byte(ZX0_READER *reader, uint8_t *value) {
  if (reader->input_index >= reader->input_size)
    return 0;
  *value = reader->input[reader->input_index++];
  reader->last_byte = *value;
  return 1;
}

static int read_bit(ZX0_READER *reader, unsigned *value) {
  if (reader->backtrack) {
    reader->backtrack = 0;
    *value = reader->last_byte & 1u;
    return 1;
  }
  if (!reader->bit_mask) {
    if (!read_byte(reader, &reader->bit_value))
      return 0;
    reader->bit_mask = 0x80u;
  }
  *value = (reader->bit_value & reader->bit_mask) ? 1u : 0u;
  reader->bit_mask >>= 1;
  return 1;
}

static int read_interlaced_elias_gamma(ZX0_READER *reader, int inverted,
                                       uint32_t *value) {
  unsigned bit;
  uint32_t result = 1;

  for (;;) {
    if (!read_bit(reader, &bit))
      return 0;
    if (bit)
      break;
    if (result > UINT32_MAX / 2u)
      return 0;
    if (!read_bit(reader, &bit))
      return 0;
    result = (result << 1) | (bit ^ (unsigned)inverted);
  }
  *value = result;
  return 1;
}

static int copy_from_output(uint8_t *output, uint32_t output_size,
                            uint32_t *output_index, uint32_t offset,
                            uint32_t length) {
  uint32_t i;

  if (offset == 0 || offset > *output_index ||
      length > output_size - *output_index)
    return 0;
  for (i = 0; i < length; i++) {
    output[*output_index] = output[*output_index - offset];
    (*output_index)++;
  }
  return 1;
}

static int copy_literals(ZX0_READER *reader, uint8_t *output,
                         uint32_t output_size, uint32_t *output_index,
                         uint32_t length) {
  uint32_t i;

  if (length > output_size - *output_index)
    return 0;
  for (i = 0; i < length; i++) {
    if (!read_byte(reader, output + *output_index))
      return 0;
    (*output_index)++;
  }
  return 1;
}

int zx0_decompress_data(const uint8_t *input, uint32_t input_size,
                        uint8_t *output, uint32_t output_size) {
  ZX0_READER reader;
  uint32_t output_index = 0;
  uint32_t length;
  uint32_t offset_msb;
  uint32_t offset;
  uint8_t offset_lsb;
  unsigned bit;

  if (!input || !input_size || (!output && output_size))
    return 0;

  reader.input = input;
  reader.input_size = input_size;
  reader.input_index = 0;
  reader.bit_mask = 0;
  reader.bit_value = 0;
  reader.last_byte = 0;
  reader.backtrack = 0;
  offset = 1;

  /* The first block is always a literal block. */
  if (!read_interlaced_elias_gamma(&reader, 0, &length) ||
      !copy_literals(&reader, output, output_size, &output_index, length))
    return 0;

  for (;;) {
    if (!read_bit(&reader, &bit))
      return 0;
    if (!bit) {
      /* Copy from the previous offset, then choose literals or a new offset. */
      if (!read_interlaced_elias_gamma(&reader, 0, &length) ||
          !copy_from_output(output, output_size, &output_index, offset ? offset : 1u,
                            length))
        return 0;
      if (!read_bit(&reader, &bit))
        return 0;
      if (!bit) {
        if (!read_interlaced_elias_gamma(&reader, 0, &length) ||
            !copy_literals(&reader, output, output_size, &output_index, length))
          return 0;
        continue;
      }
      /* bit=1 falls through to the new-offset block. */
    }

  new_offset:
    if (!read_interlaced_elias_gamma(&reader, 1, &offset_msb))
      return 0;
    if (offset_msb == 256u)
      return output_index == output_size;
    if (offset_msb == 0 || offset_msb > UINT32_MAX / 128u)
      return 0;
    if (!read_byte(&reader, &offset_lsb))
      return 0;
    offset = offset_msb * 128u - ((uint32_t)offset_lsb >> 1);
    if (offset == 0)
      return 0;
    reader.backtrack = 1;
    if (!read_interlaced_elias_gamma(&reader, 0, &length) || length == UINT32_MAX ||
        !copy_from_output(output, output_size, &output_index, offset, length + 1u))
      return 0;
    if (!read_bit(&reader, &bit))
      return 0;
    if (!bit) {
      if (!read_interlaced_elias_gamma(&reader, 0, &length) ||
          !copy_literals(&reader, output, output_size, &output_index, length))
        return 0;
    } else {
      /* A new-offset block can be followed directly by another one. */
      goto new_offset;
    }
  }
}
