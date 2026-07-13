#include "ksp/ksp.h"

#include <stdio.h>
#include <stdlib.h>

static void usage(const char *name) {
  fprintf(stderr, "usage: %s INPUT.KSP OUTPUT.KSS\n", name);
}

static int write_file(const char *path, const uint8_t *data, uint32_t size) {
  FILE *file = fopen(path, "wb");
  int ok;
  if (!file) return 0;
  ok = fwrite(data, 1, size, file) == size;
  if (fclose(file) != 0) ok = 0;
  return ok;
}

int main(int argc, char **argv) {
  KSP_INDEX index;
  uint8_t *image = NULL;
  uint32_t image_size = 0;
  char error[256];

  if (argc != 3) {
    usage(argv[0]);
    return 2;
  }
  if (!ksp_validate_file(argv[1], 1, &index, error, sizeof(error)) ||
      !ksp_build_kss_image(argv[1], &index, &image, &image_size, error,
                           sizeof(error))) {
    fprintf(stderr, "could not materialize KSP: %s\n", error);
    ksp_free_index(&index);
    return 1;
  }
  if (!write_file(argv[2], image, image_size)) {
    fprintf(stderr, "could not write %s\n", argv[2]);
    free(image);
    ksp_free_index(&index);
    return 1;
  }
  printf("wrote %s (%u bytes)\n", argv[2], image_size);
  free(image);
  ksp_free_index(&index);
  return 0;
}
