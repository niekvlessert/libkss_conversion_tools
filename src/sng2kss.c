#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "kss/kss.h"

typedef struct {
  uint8_t *data;
  uint32_t size;
  char *title;
} InputFile;

typedef struct {
  char **items;
  uint32_t count;
  uint32_t capacity;
} PathList;

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s [-o output.kss] input.sng [input2.sng ...]\n", prog);
  fprintf(stderr, "       %s [-o output.kss] sng-directory\n", prog);
}

static const char *path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *dup_range(const char *begin, size_t len) {
  char *out = (char *)malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, begin, len);
  out[len] = '\0';
  return out;
}

static char *dup_string(const char *text) { return dup_range(text, strlen(text)); }

static int path_list_add(PathList *list, const char *path) {
  char **new_items;
  if (list->count == list->capacity) {
    uint32_t new_capacity = list->capacity ? list->capacity * 2 : 64;
    new_items = (char **)realloc(list->items, new_capacity * sizeof(char *));
    if (!new_items)
      return 1;
    list->items = new_items;
    list->capacity = new_capacity;
  }

  list->items[list->count] = dup_string(path);
  if (!list->items[list->count])
    return 1;
  list->count++;
  return 0;
}

static void path_list_free(PathList *list) {
  uint32_t i;
  for (i = 0; i < list->count; i++)
    free(list->items[i]);
  free(list->items);
}

static int compare_paths(const void *a, const void *b) {
  const char *const *pa = (const char *const *)a;
  const char *const *pb = (const char *const *)b;
  return strcasecmp(*pa, *pb);
}

static int has_sng_extension(const char *path) {
  const char *dot = strrchr(path_basename(path), '.');
  return dot && strcasecmp(dot, ".sng") == 0;
}

static char *join_path(const char *dir, const char *name) {
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  int need_slash = dir_len > 0 && dir[dir_len - 1] != '/';
  char *out = (char *)malloc(dir_len + (need_slash ? 1 : 0) + name_len + 1);
  if (!out)
    return NULL;
  memcpy(out, dir, dir_len);
  if (need_slash)
    out[dir_len++] = '/';
  memcpy(out + dir_len, name, name_len + 1);
  return out;
}

static int add_input_path(PathList *paths, const char *path) {
  struct stat st;
  DIR *dir;
  struct dirent *entry;

  if (stat(path, &st) != 0)
    return path_list_add(paths, path);

  if (!S_ISDIR(st.st_mode))
    return path_list_add(paths, path);

  dir = opendir(path);
  if (!dir) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }

  while ((entry = readdir(dir)) != NULL) {
    char *joined;
    if (entry->d_name[0] == '.' || !has_sng_extension(entry->d_name))
      continue;
    joined = join_path(path, entry->d_name);
    if (!joined) {
      closedir(dir);
      return 1;
    }
    if (path_list_add(paths, joined)) {
      free(joined);
      closedir(dir);
      return 1;
    }
    free(joined);
  }

  closedir(dir);
  return 0;
}

static char *make_track_title(const char *path) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  size_t len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  return dup_range(base, len);
}

static char *make_output_name(const char *path) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  size_t dir_len = (size_t)(base - path);
  size_t stem_len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  char *out = (char *)malloc(dir_len + stem_len + 5);
  if (!out)
    return NULL;
  memcpy(out, path, dir_len);
  memcpy(out + dir_len, base, stem_len);
  memcpy(out + dir_len + stem_len, ".kss", 5);
  return out;
}

static char *make_part_output_name(const char *path, uint32_t part) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  size_t stem_len = dot && dot > base ? (size_t)(dot - path) : strlen(path);
  char suffix[32];
  size_t suffix_len;
  char *out;

  snprintf(suffix, sizeof(suffix), "_part%u.kss", part);
  suffix_len = strlen(suffix);
  out = (char *)malloc(stem_len + suffix_len + 1);
  if (!out)
    return NULL;

  memcpy(out, path, stem_len);
  memcpy(out + stem_len, suffix, suffix_len + 1);
  return out;
}

static int read_file(const char *path, uint8_t **data, uint32_t *size) {
  FILE *fp;
  long len;
  uint8_t *buf;

  fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }

  if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
    fprintf(stderr, "%s: failed to determine file size\n", path);
    fclose(fp);
    return 1;
  }

  if ((unsigned long)len > UINT32_MAX) {
    fprintf(stderr, "%s: file is too large\n", path);
    fclose(fp);
    return 1;
  }

  buf = (uint8_t *)malloc((size_t)len);
  if (!buf) {
    fclose(fp);
    return 1;
  }

  if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
    fprintf(stderr, "%s: read failed\n", path);
    free(buf);
    fclose(fp);
    return 1;
  }

  fclose(fp);
  *data = buf;
  *size = (uint32_t)len;
  return 0;
}

static int write_file(const char *path, const uint8_t *data, uint32_t size) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }

  if (size > 0 && fwrite(data, 1, size, fp) != size) {
    fprintf(stderr, "%s: write failed\n", path);
    fclose(fp);
    return 1;
  }

  fclose(fp);
  return 0;
}

static void free_inputs(InputFile *inputs, uint32_t count) {
  uint32_t i;
  if (!inputs)
    return;
  for (i = 0; i < count; i++) {
    free(inputs[i].data);
    free(inputs[i].title);
  }
  free(inputs);
}

static KSS *convert_range(InputFile *inputs, uint32_t start, uint32_t count) {
  uint8_t **data;
  uint32_t *sizes;
  const char **titles;
  uint32_t i;
  KSS *kss;

  data = (uint8_t **)calloc(count, sizeof(uint8_t *));
  sizes = (uint32_t *)calloc(count, sizeof(uint32_t));
  titles = (const char **)calloc(count, sizeof(char *));
  if (!data || !sizes || !titles) {
    free(data);
    free(sizes);
    free(titles);
    return NULL;
  }

  for (i = 0; i < count; i++) {
    data[i] = inputs[start + i].data;
    sizes[i] = inputs[start + i].size;
    titles[i] = inputs[start + i].title;
  }

  kss = KSS_sngs2kss(data, sizes, titles, count);
  free(data);
  free(sizes);
  free(titles);
  return kss;
}

static int write_range(InputFile *inputs, uint32_t start, uint32_t count, const char *output) {
  KSS *kss = convert_range(inputs, start, count);
  if (!kss)
    return 1;

  if (write_file(output, kss->data, kss->size)) {
    KSS_delete(kss);
    return 1;
  }

  printf("wrote %s (%u track%s)\n", output, count, count == 1 ? "" : "s");
  KSS_delete(kss);
  return 0;
}

static int write_split_outputs(InputFile *inputs, uint32_t count, const char *output) {
  uint32_t start = 0;
  uint32_t part = 1;

  while (start < count) {
    uint32_t lo = start + 1;
    uint32_t hi = count;
    uint32_t best = 0;
    char *part_output;

    while (lo <= hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      KSS *probe = convert_range(inputs, start, mid - start);
      if (probe) {
        best = mid;
        KSS_delete(probe);
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }

    if (best == start) {
      fprintf(stderr, "%s: too large to fit in a single KSS\n", inputs[start].title);
      return 1;
    }

    part_output = make_part_output_name(output, part);
    if (!part_output)
      return 1;

    if (write_range(inputs, start, best - start, part_output)) {
      free(part_output);
      return 1;
    }

    free(part_output);
    start = best;
    part++;
  }

  return 0;
}

int main(int argc, char **argv) {
  const char *output = NULL;
  int first_input = 1;
  uint32_t input_count, valid_count = 0, i;
  PathList paths = {0};
  InputFile *inputs = NULL;
  KSS *kss;
  char *default_output = NULL;
  int result = 1;

  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (argc >= 4 && strcmp(argv[1], "-o") == 0) {
    output = argv[2];
    first_input = 3;
  } else if (argc >= 3 && strncmp(argv[1], "-o", 2) == 0) {
    output = argv[1] + 2;
    first_input = 2;
  }

  input_count = (uint32_t)(argc - first_input);
  if (input_count == 0) {
    usage(argv[0]);
    return 1;
  }

  for (i = 0; i < input_count; i++) {
    if (add_input_path(&paths, argv[first_input + (int)i]))
      goto cleanup;
  }

  if (paths.count == 0) {
    fprintf(stderr, "no input files found\n");
    goto cleanup;
  }

  qsort(paths.items, paths.count, sizeof(char *), compare_paths);

  inputs = (InputFile *)calloc(paths.count, sizeof(InputFile));
  if (!inputs) {
    goto cleanup;
  }

  for (i = 0; i < paths.count; i++) {
    const char *path = paths.items[i];
    uint8_t *file_data = NULL;
    uint32_t file_size = 0;
    char *title;

    if (read_file(path, &file_data, &file_size))
      goto cleanup;

    if (!KSS_isSNGdata(file_data, file_size)) {
      fprintf(stderr, "%s: not recognized as Musixx SNG, skipping\n", path);
      free(file_data);
      continue;
    }

    title = make_track_title(path);
    if (!title) {
      free(file_data);
      goto cleanup;
    }

    inputs[valid_count].data = file_data;
    inputs[valid_count].size = file_size;
    inputs[valid_count].title = title;
    valid_count++;
  }

  if (valid_count == 0) {
    fprintf(stderr, "no valid Musixx SNG inputs found\n");
    goto cleanup;
  }

  if (!output) {
    default_output = make_output_name(argv[first_input]);
    if (!default_output)
      goto cleanup;
    output = default_output;
  }

  kss = convert_range(inputs, 0, valid_count);
  if (!kss) {
    if (valid_count == 1) {
      fprintf(stderr, "failed to convert SNG input to KSS\n");
      goto cleanup;
    }
    fprintf(stderr, "single KSS is too large, splitting output\n");
    result = write_split_outputs(inputs, valid_count, output);
    goto cleanup;
  }

  if (!write_file(output, kss->data, kss->size)) {
    printf("wrote %s (%u track%s)\n", output, valid_count, valid_count == 1 ? "" : "s");
    result = 0;
  }
  KSS_delete(kss);

cleanup:
  free(default_output);
  path_list_free(&paths);
  free_inputs(inputs, paths.count);
  return result;
}
