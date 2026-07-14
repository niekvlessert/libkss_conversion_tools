#include "zx_compressor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  FILE *file = NULL;
  uint8_t *input = NULL;
  uint8_t *output = NULL;
  uint32_t output_size = 0;
  uint32_t delta = 0;
  long length;
  int result = 1;

  if (argc != 3) {
    fprintf(stderr, "usage: %s INPUT OUTPUT\n", argv[0]);
    return 2;
  }
  file = fopen(argv[1], "rb");
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0)
    goto cleanup;
  input = (uint8_t *)malloc((size_t)length);
  if (!input || fread(input, 1, (size_t)length, file) != (size_t)length)
    goto cleanup;
  fclose(file);
  file = NULL;
  if (!zx0_compress_data(input, (uint32_t)length, &output, &output_size, &delta))
    goto cleanup;
  file = fopen(argv[2], "wb");
  if (!file || fwrite(output, 1, output_size, file) != output_size)
    goto cleanup;
  printf("wrote %s: %ld -> %u bytes\n", argv[2], length, output_size);
  result = 0;

cleanup:
  if (result) fprintf(stderr, "could not ZX0-compress %s\n", argv[1]);
  if (file) fclose(file);
  free(input);
  free(output);
  return result;
}
