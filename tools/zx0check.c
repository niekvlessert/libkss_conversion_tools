#include "../src/zx_decompressor.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv) {
  FILE *f;
  long size;
  uint8_t *data, *out;
  if (argc != 5) return 2;
  f = fopen(argv[1], "rb");
  if (!f) return 3;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  rewind(f);
  data = malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) return 4;
  fclose(f);
  uint32_t offset = (uint32_t)strtoul(argv[2], NULL, 0);
  uint32_t length = (uint32_t)strtoul(argv[3], NULL, 0);
  uint32_t output_size = (uint32_t)strtoul(argv[4], NULL, 0);
  if (offset + length > (uint32_t)size) return 5;
  out = calloc(1, output_size);
  if (!out || !zx0_decompress_data(data + offset, length, out, output_size)) return 6;
  fwrite(out, 1, output_size, stdout);
  free(out);
  free(data);
  return 0;
}
