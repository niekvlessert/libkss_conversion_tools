#include "ksp/mbwave.h"

#include <stdlib.h>
#include <string.h>

#define MWM_SIGNATURE_SIZE 6u
#define MWM_FILE_HEADER_SIZE 0x116u
#define MWM_PATTERN_HEADER_SIZE 3u

static const uint8_t mbwave_bootstrap[] = {
    0xAF,                   /* XOR A */
    0x21, 0x00, 0xDA,       /* LD HL,DA00H */
    0x06, 0x24,             /* LD B,24H */
    0x77,                   /* clear: LD (HL),A */
    0x23,                   /* INC HL */
    0x10, 0xFC,             /* DJNZ clear */
    0x21, 0x06, 0x80,       /* LD HL,8006H (skip MBMS signature) */
    0x22, 0x04, 0xDA,       /* LD (DA04H),HL */
    0x3E, 0x03,             /* LD A,03H */
    0x32, 0x01, 0xDA,       /* LD (DA01H),A */
    0x32, 0x02, 0xDA,       /* LD (DA02H),A */
    0x32, 0x03, 0xDA,       /* LD (DA03H),A */
    0xCD, 0x42, 0x40,       /* CALL MBPLAY */
    0xC9                    /* RET */
};

int ksp_compact_mwm(const uint8_t *input, uint32_t input_size,
                    uint8_t **output, uint32_t *output_size) {
  uint32_t position_offset = MWM_SIGNATURE_SIZE + MWM_FILE_HEADER_SIZE;
  uint32_t position_count;
  uint32_t offset_table;
  uint32_t raw_cursor;
  uint32_t compact_cursor;
  uint32_t max_pattern = 0;
  uint32_t patterns = 0;
  uint32_t capacity;
  uint8_t song_length;
  uint8_t *compact;
  uint32_t i;

  if (!output || !output_size) return 0;
  *output = NULL;
  *output_size = 0;
  if (!input || input_size < position_offset ||
      memcmp(input, "MBMS", 4) != 0)
    return 0;

  song_length = input[MWM_SIGNATURE_SIZE];
  position_count = (uint32_t)song_length + 1u;
  if (position_offset + position_count > input_size)
    return 0;
  for (i = 0; i < position_count; i++) {
    if (input[position_offset + i] > max_pattern)
      max_pattern = input[position_offset + i];
  }
  offset_table = position_offset + position_count;
  if ((uint64_t)offset_table + (uint64_t)(max_pattern + 1u) * 2u > input_size)
    return 0;

  capacity = input_size;
  compact = (uint8_t *)malloc(capacity ? capacity : 1u);
  if (!compact) return 0;
  memcpy(compact, input, offset_table + (max_pattern + 1u) * 2u);
  compact_cursor = offset_table + (max_pattern + 1u) * 2u;
  raw_cursor = compact_cursor;

  while (patterns <= max_pattern) {
    uint32_t block_size;
    uint32_t block_patterns;
    if (raw_cursor + MWM_PATTERN_HEADER_SIZE > input_size) {
      free(compact);
      return 0;
    }
    block_size = (uint32_t)input[raw_cursor] |
                 ((uint32_t)input[raw_cursor + 1u] << 8);
    block_patterns = input[raw_cursor + 2u];
    raw_cursor += MWM_PATTERN_HEADER_SIZE;
    if (block_patterns == 0 || block_size > input_size - raw_cursor ||
        patterns + block_patterns > max_pattern + 1u) {
      free(compact);
      return 0;
    }
    memcpy(compact + compact_cursor, input + raw_cursor, block_size);
    compact_cursor += block_size;
    raw_cursor += block_size;
    patterns += block_patterns;
  }

  if (raw_cursor < input_size) {
    memcpy(compact + compact_cursor, input + raw_cursor,
           input_size - raw_cursor);
    compact_cursor += input_size - raw_cursor;
  }
  *output = compact;
  *output_size = compact_cursor;
  return 1;
}

uint32_t ksp_mbwave_bootstrap_size(void) {
  return (uint32_t)sizeof(mbwave_bootstrap);
}

int ksp_copy_mbwave_bootstrap(uint8_t *output, uint32_t output_size) {
  if (!output || output_size < sizeof(mbwave_bootstrap)) return 0;
  memcpy(output, mbwave_bootstrap, sizeof(mbwave_bootstrap));
  return 1;
}
