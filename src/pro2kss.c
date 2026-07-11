#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pro_tracker_converter.h"

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [-o output.kss] input.pro\n", program);
}

static const char *path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *duplicate_range(const char *start, size_t length) {
  char *result = (char *)malloc(length + 1);
  if (!result)
    return NULL;
  memcpy(result, start, length);
  result[length] = '\0';
  return result;
}

static char *make_title(const char *path) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  size_t length = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  return duplicate_range(base, length);
}

static char *make_output_name(const char *path) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  size_t directory_length = (size_t)(base - path);
  size_t stem_length = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  char *result = (char *)malloc(directory_length + stem_length + 5);

  if (!result)
    return NULL;
  memcpy(result, path, directory_length);
  memcpy(result + directory_length, base, stem_length);
  memcpy(result + directory_length + stem_length, ".kss", 5);
  return result;
}

static int read_file(const char *path, uint8_t **data, uint32_t *size) {
  FILE *file;
  long length;
  uint8_t *buffer;

  file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "%s: failed to determine file size\n", path);
    fclose(file);
    return 1;
  }
  if ((unsigned long)length > UINT32_MAX) {
    fprintf(stderr, "%s: file is too large\n", path);
    fclose(file);
    return 1;
  }

  buffer = (uint8_t *)malloc((size_t)length);
  if (!buffer) {
    fclose(file);
    return 1;
  }
  if (length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
    fprintf(stderr, "%s: read failed\n", path);
    free(buffer);
    fclose(file);
    return 1;
  }
  fclose(file);
  *data = buffer;
  *size = (uint32_t)length;
  return 0;
}

static int write_file(const char *path, const uint8_t *data, uint32_t size) {
  FILE *file = fopen(path, "wb");
  if (!file) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  if (fwrite(data, 1, size, file) != size) {
    fprintf(stderr, "%s: write failed\n", path);
    fclose(file);
    return 1;
  }
  fclose(file);
  return 0;
}

int main(int argc, char **argv) {
  const char *input;
  const char *output = NULL;
  int input_index = 1;
  uint8_t *pro_data = NULL;
  uint32_t pro_size = 0;
  char *title = NULL;
  char *default_output = NULL;
  PRO_KSS *kss = NULL;
  int result = 1;

  if (argc >= 4 && strcmp(argv[1], "-o") == 0) {
    output = argv[2];
    input_index = 3;
  } else if (argc >= 3 && strncmp(argv[1], "-o", 2) == 0) {
    output = argv[1] + 2;
    input_index = 2;
  }
  if (argc - input_index != 1) {
    usage(argv[0]);
    return 1;
  }
  input = argv[input_index];

  if (read_file(input, &pro_data, &pro_size))
    goto cleanup;
  if (!pro_tracker_is_valid(pro_data, pro_size)) {
    fprintf(stderr, "%s: not recognized as a Pro Tracker PRO file\n", input);
    goto cleanup;
  }
  title = make_title(input);
  if (!title)
    goto cleanup;
  if (!output) {
    default_output = make_output_name(input);
    if (!default_output)
      goto cleanup;
    output = default_output;
  }

  kss = pro_tracker_to_kss(pro_data, pro_size, title);
  if (!kss) {
    fprintf(stderr, "%s: failed to build KSS image\n", input);
    goto cleanup;
  }
  if (write_file(output, kss->data, kss->size))
    goto cleanup;

  printf("wrote %s (1 track)\n", output);
  result = 0;

cleanup:
  pro_kss_delete(kss);
  free(default_output);
  free(title);
  free(pro_data);
  return result;
}
