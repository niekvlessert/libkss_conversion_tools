#include "ksp/engine.h"
#include "ksp/ksp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  uint32_t size;
} BLOB;

static int read_blob(const char *path, BLOB *blob) {
  FILE *file = fopen(path, "rb");
  long length;
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
    if (file) fclose(file);
    return 0;
  }
  blob->data = (uint8_t *)malloc(length ? (size_t)length : 1);
  if (!blob->data || (length && fread(blob->data, 1, (size_t)length, file) != (size_t)length)) {
    free(blob->data);
    blob->data = NULL;
    fclose(file);
    return 0;
  }
  fclose(file);
  blob->size = (uint32_t)length;
  return 1;
}

static void usage(const char *name) {
  fprintf(stderr,
      "usage: %s --kss-prefix FILE --engine FILE --song FILE\n"
      "          --engine-desc FILE --output FILE [metadata options]\n"
      "          [--engine-msx-bin]\n"
      "descriptor format: key=value (addresses accept 0x prefix)\n"
      "metadata: --title TEXT --author TEXT --game TEXT --comment TEXT\n",
      name);
}

int main(int argc, char **argv) {
  const char *prefix_path = NULL, *engine_path = NULL, *song_path = NULL;
  const char *descriptor_path = NULL, *output_path = NULL;
  const char *title = NULL, *author = NULL, *game = NULL, *comment = NULL;
  int engine_is_msx_bin = 0;
  BLOB prefix = {0}, engine = {0}, song = {0};
  KSP_ENGINE_DESCRIPTOR descriptor;
  uint8_t descriptor_data[KSP_ENGINE_DESCRIPTOR_SIZE];
  char metadata[4096];
  KSP_CHUNK chunks[4];
  uint32_t chunk_count = 3;
  char error[256];
  int i;

  for (i = 1; i < argc; i++) {
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(argv[i], "--kss-prefix") && value) prefix_path = value;
    else if (!strcmp(argv[i], "--engine") && value) engine_path = value;
    else if (!strcmp(argv[i], "--song") && value) song_path = value;
    else if (!strcmp(argv[i], "--engine-desc") && value) descriptor_path = value;
    else if (!strcmp(argv[i], "--output") && value) output_path = value;
    else if (!strcmp(argv[i], "--title") && value) title = value;
    else if (!strcmp(argv[i], "--author") && value) author = value;
    else if (!strcmp(argv[i], "--game") && value) game = value;
    else if (!strcmp(argv[i], "--comment") && value) comment = value;
    else if (!strcmp(argv[i], "--engine-msx-bin")) {
      engine_is_msx_bin = 1;
      i--;
    }
    else {
      usage(argv[0]);
      return 2;
    }
    i++;
  }
  if (!prefix_path || !engine_path || !song_path || !descriptor_path || !output_path) {
    usage(argv[0]);
    return 2;
  }
  if (!read_blob(prefix_path, &prefix) || !read_blob(engine_path, &engine) ||
      !read_blob(song_path, &song)) {
    fprintf(stderr, "could not read one of the input files\n");
    goto failure;
  }
  if (!ksp_read_engine_descriptor_text(descriptor_path, &descriptor, error, sizeof(error))) {
    fprintf(stderr, "invalid engine descriptor: %s\n", error);
    goto failure;
  }
  if (engine_is_msx_bin) {
    uint16_t load_address;
    uint16_t end_address;
    uint16_t exec_address;
    if (engine.size < 7 || engine.data[0] != 0xFE) {
      fprintf(stderr, "engine is not an MSX binary image\n");
      goto failure;
    }
    load_address = (uint16_t)engine.data[1] | ((uint16_t)engine.data[2] << 8);
    end_address = (uint16_t)engine.data[3] | ((uint16_t)engine.data[4] << 8);
    exec_address = (uint16_t)engine.data[5] | ((uint16_t)engine.data[6] << 8);
    if ((uint32_t)end_address < load_address ||
        end_address - load_address != engine.size - 7 ||
        load_address != descriptor.load_address ||
        exec_address != descriptor.init_address) {
      fprintf(stderr, "MSX binary header does not match engine descriptor\n");
      goto failure;
    }
    memmove(engine.data, engine.data + 7, engine.size - 7);
    engine.size -= 7;
  }
  ksp_encode_engine_descriptor(&descriptor, descriptor_data);
  if (!ksp_validate_engine_descriptor(descriptor_data, sizeof(descriptor_data),
                                      error, sizeof(error))) {
    fprintf(stderr, "invalid encoded engine descriptor: %s\n", error);
    goto failure;
  }
  memset(chunks, 0, sizeof(chunks));
  memcpy(chunks[0].type, "ENGN", 4);
  chunks[0].data = engine.data;
  chunks[0].size = engine.size;
  memcpy(chunks[1].type, "SONG", 4);
  chunks[1].data = song.data;
  chunks[1].size = song.size;
  memcpy(chunks[2].type, "EDES", 4);
  chunks[2].data = descriptor_data;
  chunks[2].size = sizeof(descriptor_data);
  if (title || author || game || comment) {
    int length = snprintf(metadata, sizeof(metadata),
                          "title=%s\nauthor=%s\ngame=%s\ncomment=%s\n",
                          title ? title : "", author ? author : "",
                          game ? game : "", comment ? comment : "");
    if (length < 0 || (size_t)length >= sizeof(metadata)) {
      fprintf(stderr, "metadata is too long\n");
      goto failure;
    }
    memcpy(chunks[3].type, "META", 4);
    chunks[3].data = (const uint8_t *)metadata;
    chunks[3].size = (uint32_t)length;
    chunk_count++;
  }
  if (!ksp_write_file(output_path, prefix.data, prefix.size, chunks,
                      chunk_count, error, sizeof(error))) {
    fprintf(stderr, "could not write KSP: %s\n", error);
    goto failure;
  }
  printf("wrote %s (%u chunks)\n", output_path, chunk_count);
  free(prefix.data);
  free(engine.data);
  free(song.data);
  return 0;

failure:
  free(prefix.data);
  free(engine.data);
  free(song.data);
  return 1;
}
