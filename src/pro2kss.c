#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pro_tracker_converter.h"

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [-o output.kss] input.pro [input2.pro ...]\n", program);
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

static char *duplicate_string(const char *value) {
  return duplicate_range(value, strlen(value));
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
  const char *output = NULL;
  const char **input_paths = NULL;
  uint8_t **file_data = NULL;
  uint32_t *file_sizes = NULL;
  char **titles = NULL;
  PRO_TRACKER_INPUT *tracks = NULL;
  uint32_t input_count = 0;
  uint32_t track_count = 0;
  uint32_t i;
  char *default_output = NULL;
  PRO_KSS *kss = NULL;
  int result = 1;

  input_paths = (const char **)calloc((size_t)argc, sizeof(*input_paths));
  file_data = (uint8_t **)calloc((size_t)argc, sizeof(*file_data));
  file_sizes = (uint32_t *)calloc((size_t)argc, sizeof(*file_sizes));
  titles = (char **)calloc((size_t)argc, sizeof(*titles));
  tracks = (PRO_TRACKER_INPUT *)calloc((size_t)argc, sizeof(*tracks));
  if (!input_paths || !file_data || !file_sizes || !titles || !tracks)
    goto cleanup;

  for (i = 1; i < (uint32_t)argc; ++i) {
    if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 >= (uint32_t)argc) {
        usage(argv[0]);
        goto cleanup;
      }
      output = argv[++i];
    } else if (strncmp(argv[i], "-o", 2) == 0) {
      output = argv[i] + 2;
      if (!*output) {
        usage(argv[0]);
        goto cleanup;
      }
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      goto cleanup;
    } else {
      input_paths[input_count++] = argv[i];
    }
  }
  if (input_count == 0) {
    usage(argv[0]);
    goto cleanup;
  }

  for (i = 0; i < input_count; ++i) {
    const char *path = input_paths[i];

    if (read_file(path, &file_data[i], &file_sizes[i]))
      goto cleanup;
    if (!pro_tracker_is_valid(file_data[i], file_sizes[i])) {
      fprintf(stderr, "skipping %s: not recognized as a Pro Tracker PRO file\n", path);
      free(file_data[i]);
      file_data[i] = NULL;
      continue;
    }
    titles[i] = make_title(path);
    if (!titles[i])
      goto cleanup;
    tracks[track_count].data = file_data[i];
    tracks[track_count].size = file_sizes[i];
    tracks[track_count].title = titles[i];
    fprintf(stderr, "adding track %u: %s\n", (unsigned)(track_count + 1), path);
    fflush(stderr);
    ++track_count;
  }
  if (track_count == 0) {
    fprintf(stderr, "no valid Pro Tracker PRO files were found\n");
    goto cleanup;
  }

  if (!output) {
    if (track_count == 1) {
      /* Find the path corresponding to the one valid track. */
      for (i = 0; i < input_count; ++i) {
        if (file_data[i]) {
          default_output = make_output_name(input_paths[i]);
          break;
        }
      }
    } else {
      default_output = duplicate_string("pro.kss");
    }
    if (!default_output)
      goto cleanup;
    output = default_output;
  }

  fprintf(stderr, "compressing %u track%s with ZX0...\n", (unsigned)track_count,
          track_count == 1 ? "" : "s");
  kss = pro_trackers_to_kss(tracks, track_count);
  if (!kss) {
    fprintf(stderr, "failed to build KSS image\n");
    goto cleanup;
  }
  if (write_file(output, kss->data, kss->size))
    goto cleanup;

  printf("wrote %s (%u track%s)\n", output, (unsigned)track_count,
         track_count == 1 ? "" : "s");
  result = 0;

cleanup:
  pro_kss_delete(kss);
  if (titles) {
    for (i = 0; i < input_count; ++i)
      free(titles[i]);
  }
  if (file_data) {
    for (i = 0; i < input_count; ++i)
      free(file_data[i]);
  }
  free(tracks);
  free(titles);
  free(file_sizes);
  free(file_data);
  free(input_paths);
  free(default_output);
  return result;
}
