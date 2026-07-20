#include "ksp/ksp.h"
#include "ksp/engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  uint8_t prefix[0x20] = {0};
  uint8_t engine[256];
  uint8_t song[512];
  uint8_t descriptor[KSP_ENGINE_DESCRIPTOR_SIZE];
  KSP_ENGINE_DESCRIPTOR d = {1, 0x8000, 0x8120, 0x8150, 0,
                             0xF000, 0x4000, 0xC000, 0x2000,
                             50, 1, 131072, 0};
  KSP_CHUNK chunks[3];
  KSP_INDEX index;
  uint32_t i;
  uint8_t *loaded_engine = NULL;
  uint8_t *loaded_song = NULL;
  FILE *file;
  char error[256];
  const char *path = "/tmp/ksp-selftest.ksp";
  memcpy(prefix, "KSSX", 4);
  for (i = 0; i < sizeof(engine); i++)
    engine[i] = (uint8_t)(i & 0x0F);
  for (i = 0; i < sizeof(song); i++)
    song[i] = (uint8_t)((i / 16) & 0x0F);
  ksp_encode_engine_descriptor(&d, descriptor);
  memset(chunks, 0, sizeof(chunks));
  memcpy(chunks[0].type, "ENGN", 4); chunks[0].data = engine; chunks[0].size = sizeof(engine);
  memcpy(chunks[1].type, "SONG", 4); chunks[1].data = song; chunks[1].size = sizeof(song);
  memcpy(chunks[2].type, "EDES", 4); chunks[2].data = descriptor; chunks[2].size = sizeof(descriptor);
  if (!ksp_write_file(path, prefix, sizeof(prefix), chunks, 3, error, sizeof(error))) {
    fprintf(stderr, "write: %s\n", error);
    return 1;
  }
  if (!ksp_validate_file(path, 1, &index, error, sizeof(error))) {
    fprintf(stderr, "validate: %s\n", error);
    return 1;
  }
  if (index.entry_count != 3 ||
      !ksp_index_is_compact(&index) ||
      index.entries[0].offset != KSP_KSS_HEADER_SIZE ||
      index.entries[0].packed_size != sizeof(engine) ||
      index.entries[0].compression != KSP_COMPRESSION_NONE ||
      index.entries[1].compression != KSP_COMPRESSION_NONE) {
    fprintf(stderr, "unexpected index\n");
    ksp_free_index(&index);
    return 1;
  }
  if (!ksp_read_chunk(path, &index.entries[1], &loaded_song, error,
                      sizeof(error)) || memcmp(loaded_song, song, sizeof(song))) {
    fprintf(stderr, "chunk read failed: %s\n", error);
    free(loaded_song);
    ksp_free_index(&index);
    return 1;
  }
  free(loaded_song);
  loaded_song = NULL;
  ksp_free_index(&index);

  chunks[0].compression = KSP_COMPRESSION_ZX0;
  chunks[1].compression = KSP_COMPRESSION_ZX0;
  if (!ksp_write_file(path, prefix, sizeof(prefix), chunks, 3, error,
                      sizeof(error))) {
    fprintf(stderr, "compressed write: %s\n", error);
    return 1;
  }
  if (!ksp_validate_file(path, 1, &index, error, sizeof(error))) {
    fprintf(stderr, "compressed validate: %s\n", error);
    return 1;
  }
  if (!ksp_index_is_compact(&index) ||
      index.entries[0].offset != KSP_KSS_HEADER_SIZE ||
      index.entries[0].compression != KSP_COMPRESSION_ZX0 ||
      index.entries[1].compression != KSP_COMPRESSION_ZX0 ||
      index.entries[0].packed_size >= index.entries[0].unpacked_size ||
      index.entries[1].packed_size >= index.entries[1].unpacked_size) {
    fprintf(stderr, "ZX0 compression was not applied\n");
    ksp_free_index(&index);
    return 1;
  }
  if (!ksp_read_chunk(path, &index.entries[0], &loaded_engine, error,
                      sizeof(error)) ||
      memcmp(loaded_engine, engine, sizeof(engine))) {
    fprintf(stderr, "compressed engine read failed: %s\n", error);
    free(loaded_engine);
    ksp_free_index(&index);
    return 1;
  }
  if (!ksp_read_chunk(path, &index.entries[1], &loaded_song, error,
                      sizeof(error)) || memcmp(loaded_song, song, sizeof(song))) {
    fprintf(stderr, "compressed song read failed: %s\n", error);
    free(loaded_engine);
    free(loaded_song);
    ksp_free_index(&index);
    return 1;
  }
  free(loaded_engine);
  free(loaded_song);
  file = fopen(path, "r+b");
  if (!file || fseek(file, (long)index.entries[0].offset, SEEK_SET) != 0 ||
      fputc(0xFF, file) == EOF) {
    fprintf(stderr, "could not prepare corruption test\n");
    if (file) fclose(file);
    ksp_free_index(&index);
    return 1;
  }
  fclose(file);
  ksp_free_index(&index);
  if (ksp_validate_file(path, 1, &index, error, sizeof(error))) {
    fprintf(stderr, "corruption was not detected\n");
    ksp_free_index(&index);
    return 1;
  }
  remove(path);
  puts("ksp self-test passed");
  return 0;
}
