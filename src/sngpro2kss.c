#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "pro_tracker_converter.h"
#include "sng2kss_converter.h"
#include "sngpro2kss_converter.h"

typedef struct {
  char **items;
  uint32_t count;
  uint32_t capacity;
} PATH_LIST;

typedef struct {
  uint8_t *data;
  uint32_t size;
  char *title;
  char *path;
  SNGPRO_TRACK_TYPE type;
  int valid;
} INPUT_FILE;

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [-o output.kss] input.sng input.pro ...\n", program);
  fprintf(stderr, "       %s [-o output.kss] sng-directory pro-directory\n", program);
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

static char *duplicate_string(const char *value) {
  return duplicate_range(value, strlen(value));
}

static char *make_title(const char *path) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  size_t length = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  return duplicate_range(base, length);
}

static int path_list_add(PATH_LIST *list, const char *path) {
  char **new_items;
  if (list->count == list->capacity) {
    uint32_t new_capacity = list->capacity ? list->capacity * 2 : 64;
    new_items = (char **)realloc(list->items, new_capacity * sizeof(*new_items));
    if (!new_items)
      return 1;
    list->items = new_items;
    list->capacity = new_capacity;
  }
  list->items[list->count] = duplicate_string(path);
  if (!list->items[list->count])
    return 1;
  list->count++;
  return 0;
}

static void path_list_free(PATH_LIST *list) {
  uint32_t i;
  for (i = 0; i < list->count; ++i)
    free(list->items[i]);
  free(list->items);
}

static int compare_paths(const void *left, const void *right) {
  const char *const *a = (const char *const *)left;
  const char *const *b = (const char *const *)right;
  return strcasecmp(*a, *b);
}

static int compare_inputs(const void *left, const void *right) {
  const INPUT_FILE *a = (const INPUT_FILE *)left;
  const INPUT_FILE *b = (const INPUT_FILE *)right;
  if (a->type != b->type)
    return a->type == SNGPRO_TRACK_SNG ? -1 : 1;
  return strcasecmp(a->path, b->path);
}

static int is_extension(const char *path, const char *extension) {
  const char *dot = strrchr(path_basename(path), '.');
  return dot && strcasecmp(dot, extension) == 0;
}

static int add_input_path(PATH_LIST *paths, const char *path) {
  struct stat st;
  DIR *directory;
  struct dirent *entry;

  if (stat(path, &st) != 0)
    return path_list_add(paths, path);
  if (!S_ISDIR(st.st_mode))
    return path_list_add(paths, path);

  directory = opendir(path);
  if (!directory) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  while ((entry = readdir(directory)) != NULL) {
    size_t directory_length;
    size_t name_length;
    char *joined;

    if (entry->d_name[0] == '.' ||
        (!is_extension(entry->d_name, ".sng") && !is_extension(entry->d_name, ".pro")))
      continue;
    directory_length = strlen(path);
    name_length = strlen(entry->d_name);
    joined = (char *)malloc(directory_length + 1 + name_length + 1);
    if (!joined) {
      closedir(directory);
      return 1;
    }
    memcpy(joined, path, directory_length);
    joined[directory_length] = '/';
    memcpy(joined + directory_length + 1, entry->d_name, name_length + 1);
    if (path_list_add(paths, joined)) {
      free(joined);
      closedir(directory);
      return 1;
    }
    free(joined);
  }
  closedir(directory);
  return 0;
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

static void free_inputs(INPUT_FILE *inputs, uint32_t count) {
  uint32_t i;
  if (!inputs)
    return;
  for (i = 0; i < count; ++i) {
    free(inputs[i].data);
    free(inputs[i].title);
    free(inputs[i].path);
  }
  free(inputs);
}

static int is_valid_pro(const uint8_t *data, uint32_t size) {
  return pro_tracker_is_valid(data, size);
}

int main(int argc, char **argv) {
  const char *output = NULL;
  PATH_LIST paths = {0};
  INPUT_FILE *inputs = NULL;
  SNGPRO_INPUT *tracks = NULL;
  SNGPRO_KSS *kss = NULL;
  uint32_t first_input = 1;
  uint32_t valid_count = 0;
  uint32_t track_count = 0;
  uint32_t i;
  uint32_t skip_count;
  int result = 1;

  if (argc >= 4 && strcmp(argv[1], "-o") == 0) {
    output = argv[2];
    first_input = 3;
  } else if (argc >= 3 && strncmp(argv[1], "-o", 2) == 0) {
    output = argv[1] + 2;
    first_input = 2;
  }
  if (first_input >= (uint32_t)argc) {
    usage(argv[0]);
    return 1;
  }

  for (i = first_input; i < (uint32_t)argc; ++i) {
    if (add_input_path(&paths, argv[i]))
      goto cleanup;
  }
  if (paths.count == 0) {
    fprintf(stderr, "no input files found\n");
    goto cleanup;
  }
  qsort(paths.items, paths.count, sizeof(*paths.items), compare_paths);

  inputs = (INPUT_FILE *)calloc(paths.count, sizeof(*inputs));
  if (!inputs)
    goto cleanup;
  for (i = 0; i < paths.count; ++i) {
    uint8_t *data = NULL;
    uint32_t size = 0;

    if (read_file(paths.items[i], &data, &size))
      goto cleanup;
    if (is_extension(paths.items[i], ".sng")) {
      if (!sng_is_valid(data, size)) {
        fprintf(stderr, "skipping %s: not recognized as Musixx SNG\n", paths.items[i]);
        free(data);
        continue;
      }
      inputs[valid_count].type = SNGPRO_TRACK_SNG;
    } else if (is_valid_pro(data, size)) {
      inputs[valid_count].type = SNGPRO_TRACK_PRO;
    } else {
      fprintf(stderr, "skipping %s: not recognized as SNG or Pro Tracker\n", paths.items[i]);
      free(data);
      continue;
    }
    inputs[valid_count].data = data;
    inputs[valid_count].size = size;
    inputs[valid_count].title = make_title(paths.items[i]);
    inputs[valid_count].path = duplicate_string(paths.items[i]);
    inputs[valid_count].valid = 1;
    if (!inputs[valid_count].title || !inputs[valid_count].path)
      goto cleanup;
    valid_count++;
  }
  if (valid_count == 0) {
    fprintf(stderr, "no valid SNG or Pro Tracker inputs found\n");
    goto cleanup;
  }
  qsort(inputs, valid_count, sizeof(*inputs), compare_inputs);

  skip_count = valid_count > 256 ? valid_count - 256 : 0;
  for (i = 0; i < skip_count; ++i) {
    uint32_t j;
    uint32_t smallest = UINT32_MAX;
    for (j = 0; j < valid_count; ++j) {
      if (inputs[j].valid && inputs[j].type == SNGPRO_TRACK_PRO &&
          (smallest == UINT32_MAX || inputs[j].size < inputs[smallest].size ||
           (inputs[j].size == inputs[smallest].size && strcasecmp(inputs[j].path, inputs[smallest].path) < 0)))
        smallest = j;
    }
    if (smallest == UINT32_MAX) {
      fprintf(stderr, "more than 256 tracks remain, but there are no Pro files left to skip\n");
      goto cleanup;
    }
    fprintf(stderr, "skipping smallest PRO for KSS compatibility: %s (%u bytes)\n",
            inputs[smallest].path, (unsigned)inputs[smallest].size);
    inputs[smallest].valid = 0;
  }

  tracks = (SNGPRO_INPUT *)calloc(valid_count - skip_count, sizeof(*tracks));
  if (!tracks)
    goto cleanup;
  for (i = 0; i < valid_count; ++i) {
    if (!inputs[i].valid)
      continue;
    tracks[track_count].data = inputs[i].data;
    tracks[track_count].size = inputs[i].size;
    tracks[track_count].title = inputs[i].title;
    tracks[track_count].type = inputs[i].type;
    fprintf(stderr, "adding track %u: %s [%s]\n", (unsigned)(track_count + 1), inputs[i].path,
            inputs[i].type == SNGPRO_TRACK_SNG ? "SNG" : "PRO");
    fflush(stderr);
    track_count++;
  }
  if (track_count == 0)
    goto cleanup;

  if (!output)
    output = "sngpro.kss";
  fprintf(stderr, "compressing %u tracks with ZX0 and building both engines...\n",
          (unsigned)track_count);
  kss = sngpro_tracks_to_kss(tracks, track_count);
  if (!kss) {
    fprintf(stderr, "failed to build merged SNG/Pro KSS\n");
    goto cleanup;
  }
  if (write_file(output, kss->data, kss->size))
    goto cleanup;
  printf("wrote %s (%u tracks)\n", output, (unsigned)track_count);
  result = 0;

cleanup:
  sngpro_kss_delete(kss);
  free(tracks);
  free_inputs(inputs, valid_count);
  path_list_free(&paths);
  return result;
}
