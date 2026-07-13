#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSX_HEADER_SIZE 7u
#define WAVEDRVR_LOAD 0x9000u
#define WAVEDRVR_END 0xCFD7u
#define WAVEDRVR_EXEC 0x9000u
#define DRIVER_SOURCE 0x90B0u
#define DRIVER_LOAD 0x4000u
#define DRIVER_SIZE 0x3F27u

static void usage(const char *name) {
  fprintf(stderr, "usage: %s WAVEDRVR.BIN OUTPUT_ENGINE\n", name);
  fprintf(stderr, "extracts the fixed 90B0H..CFD6H player copied to 4000H\n");
}

static int read_file(const char *path, uint8_t **data, size_t *size) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *buffer;

  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    if (file) fclose(file);
    return 0;
  }
  buffer = (uint8_t *)malloc(length ? (size_t)length : 1);
  if (!buffer || (length && fread(buffer, 1, (size_t)length, file) != (size_t)length)) {
    free(buffer);
    fclose(file);
    return 0;
  }
  fclose(file);
  *data = buffer;
  *size = (size_t)length;
  return 1;
}

static uint16_t get16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

int main(int argc, char **argv) {
  uint8_t *input = NULL;
  uint8_t *engine = NULL;
  size_t input_size;
  FILE *output = NULL;
  size_t source_offset;

  if (argc != 3) {
    usage(argv[0]);
    return 2;
  }
  if (!read_file(argv[1], &input, &input_size)) {
    fprintf(stderr, "could not read %s\n", argv[1]);
    return 1;
  }
  if (input_size < MSX_HEADER_SIZE || input[0] != 0xFE ||
      get16(input + 1) != WAVEDRVR_LOAD ||
      get16(input + 3) != WAVEDRVR_END ||
      get16(input + 5) != WAVEDRVR_EXEC ||
      (size_t)(WAVEDRVR_END - WAVEDRVR_LOAD) != input_size - MSX_HEADER_SIZE) {
    fprintf(stderr, "%s is not the expected DMV1 WAVEDRVR.BIN image\n", argv[1]);
    free(input);
    return 1;
  }
  source_offset = MSX_HEADER_SIZE + (DRIVER_SOURCE - WAVEDRVR_LOAD);
  if (source_offset > input_size || DRIVER_SIZE > input_size - source_offset) {
    fprintf(stderr, "WAVEDRVR copied-driver range is outside the input\n");
    free(input);
    return 1;
  }
  if (input[source_offset] != 'A' || input[source_offset + 1] != 'B' ||
      input[source_offset + 2] != 0) {
    fprintf(stderr, "copied driver does not begin with AB\\0\n");
    free(input);
    return 1;
  }
  engine = (uint8_t *)malloc(DRIVER_SIZE);
  if (!engine) {
    fprintf(stderr, "out of memory\n");
    free(input);
    return 1;
  }
  memcpy(engine, input + source_offset, DRIVER_SIZE);
  output = fopen(argv[2], "wb");
  if (!output) {
    fprintf(stderr, "could not write %s\n", argv[2]);
    free(engine);
    free(input);
    return 1;
  }
  if (fwrite(engine, 1, DRIVER_SIZE, output) != DRIVER_SIZE ||
      fclose(output) != 0) {
    fprintf(stderr, "could not write %s\n", argv[2]);
    free(engine);
    free(input);
    return 1;
  }
  printf("extracted %s: %04XH..%04XH -> %04XH (%u bytes)\n",
         argv[1], DRIVER_SOURCE, DRIVER_SOURCE + DRIVER_SIZE - 1,
         DRIVER_LOAD, DRIVER_SIZE);
  free(engine);
  free(input);
  return 0;
}
