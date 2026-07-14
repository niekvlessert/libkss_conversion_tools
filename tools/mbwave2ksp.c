#include "ksp/engine.h"
#include "ksp/ksp.h"
#include "ksp/mbwave.h"

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
#define SONG_LOAD 0x8000u
#define ENGINE_BOOTSTRAP 0x7F30u
#define ENGINE_INIT ENGINE_BOOTSTRAP
#define ENGINE_PLAY 0x45E5u
#define ENGINE_STOP 0x4048u
#define MAX_TRACKS 256u

typedef struct { uint8_t *data; uint32_t size; } BLOB;

static void put16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)value; data[1] = (uint8_t)(value >> 8);
}
static uint16_t get16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}
static int read_blob(const char *path, BLOB *blob) {
  FILE *file = fopen(path, "rb"); long length;
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
    if (file) fclose(file); return 0;
  }
  blob->data = (uint8_t *)malloc(length ? (size_t)length : 1);
  if (!blob->data || (length && fread(blob->data, 1, (size_t)length, file) != (size_t)length)) {
    free(blob->data); blob->data = NULL; fclose(file); return 0;
  }
  fclose(file); blob->size = (uint32_t)length; return 1;
}
static void usage(const char *name) {
  fprintf(stderr,
    "usage: %s --driver WAVEDRVR.BIN --song FILE.MWM [--song FILE.MWM ...]\n"
    "          --output FILE.ksp [--mwk FILE.MWK ...]\n"
    "          [--title TEXT] [--author TEXT] [--game TEXT] [--zx0]\n"
    "MWK arguments correspond to SONG arguments by order; use --mwk - for songs without a kit.\n", name);
}
static int parse_args(int argc, char **argv, const char **driver,
                      const char **songs, uint32_t *song_count,
                      const char **output, const char **mwks,
                      uint32_t *mwk_count, const char **title,
                      const char **author, const char **game, int *use_zx0) {
  int i;
  for (i = 1; i < argc; i++) {
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(argv[i], "--driver") && value) *driver = value;
    else if (!strcmp(argv[i], "--song") && value && *song_count < MAX_TRACKS) songs[(*song_count)++] = value;
    else if (!strcmp(argv[i], "--mwk") && value && *mwk_count < MAX_TRACKS)
      mwks[(*mwk_count)++] = (!strcmp(value, "-") || !strcmp(value, "NONE")) ? NULL : value;
    else if (!strcmp(argv[i], "--output") && value) *output = value;
    else if (!strcmp(argv[i], "--title") && value) *title = value;
    else if (!strcmp(argv[i], "--author") && value) *author = value;
    else if (!strcmp(argv[i], "--game") && value) *game = value;
    else if (!strcmp(argv[i], "--zx0")) { *use_zx0 = 1; continue; }
    else return 0;
    i++;
  }
  return *driver && *song_count && *output &&
         (*mwk_count == 0 || *mwk_count == *song_count);
}
static int extract_driver(const BLOB *input, uint8_t **engine) {
  size_t source_offset;
  if (input->size < MSX_HEADER_SIZE || input->data[0] != 0xFE ||
      get16(input->data + 1) != WAVEDRVR_LOAD || get16(input->data + 3) != WAVEDRVR_END ||
      get16(input->data + 5) != WAVEDRVR_EXEC ||
      (uint32_t)(WAVEDRVR_END - WAVEDRVR_LOAD) != input->size - MSX_HEADER_SIZE) return 0;
  source_offset = MSX_HEADER_SIZE + (DRIVER_SOURCE - WAVEDRVR_LOAD);
  if (source_offset > input->size || DRIVER_SIZE > input->size - source_offset ||
      input->data[source_offset] != 'A' || input->data[source_offset + 1] != 'B' ||
      input->data[source_offset + 2] != 0) return 0;
  *engine = (uint8_t *)malloc(DRIVER_SIZE);
  if (!*engine) return 0;
  memcpy(*engine, input->data + source_offset, DRIVER_SIZE); return 1;
}
static void make_kss_header(uint8_t prefix[KSP_KSS_HEADER_SIZE], uint32_t load_size,
                            uint32_t song_count) {
  memset(prefix, 0, KSP_KSS_HEADER_SIZE); memcpy(prefix, "KSSX", 4);
  put16(prefix + 0x04, DRIVER_LOAD); put16(prefix + 0x06, (uint16_t)load_size);
  put16(prefix + 0x08, ENGINE_INIT); put16(prefix + 0x0A, ENGINE_PLAY);
  prefix[0x0E] = 0x10; /* OPL4 is described by EDES. */
  put16(prefix + 0x18, 0);
  put16(prefix + 0x1A, (uint16_t)(song_count - 1));
}

int main(int argc, char **argv) {
  const char *driver_path = NULL, *output_path = NULL, *title = NULL;
  const char *author = NULL, *game = NULL;
  const char *song_paths[MAX_TRACKS] = {0}, *mwk_paths[MAX_TRACKS] = {0};
  uint32_t song_count = 0, mwk_count = 0, i, chunk_count, max_song_size = 0;
  uint32_t mwk_ids[MAX_TRACKS];
  int use_zx0 = 0, metadata_length;
  BLOB driver = {0}, songs[MAX_TRACKS] = {{0}}, mwks[MAX_TRACKS] = {{0}};
  uint8_t *engine = NULL, *prefix = NULL, descriptor_data[KSP_ENGINE_DESCRIPTOR_SIZE];
  KSP_CHUNK chunks[3 + MAX_TRACKS * 2 + 1];
  KSP_ENGINE_DESCRIPTOR descriptor = {1, DRIVER_LOAD, ENGINE_INIT, ENGINE_PLAY, ENGINE_STOP, 0,
                                      SONG_LOAD, 0xDA00, 0x0100, 60, 1, 0, 0};
  char metadata[2048], error[256];

  if (!parse_args(argc, argv, &driver_path, song_paths, &song_count, &output_path,
                  mwk_paths, &mwk_count, &title, &author, &game, &use_zx0)) {
    usage(argv[0]); return 2;
  }
  if (!read_blob(driver_path, &driver) || !extract_driver(&driver, &engine)) {
    fprintf(stderr, "could not read or recognize the DMV1 driver\n"); goto failure;
  }
  for (i = 0; i < song_count; i++) {
    uint8_t *compact = NULL; uint32_t compact_size = 0;
    if (!read_blob(song_paths[i], &songs[i]) || songs[i].size < 4 ||
        memcmp(songs[i].data, "MBMS", 4) != 0 ||
        !ksp_compact_mwm(songs[i].data, songs[i].size, &compact, &compact_size)) {
      fprintf(stderr, "invalid MBMS/MWM song: %s\n", song_paths[i]); goto failure;
    }
    free(compact);
    if (compact_size > max_song_size) max_song_size = compact_size;
    if (mwk_count && mwk_paths[i] && !read_blob(mwk_paths[i], &mwks[i])) {
      fprintf(stderr, "could not read MWK: %s\n", mwk_paths[i]); goto failure;
    }
  }
  for (i = 0; i < song_count; i++) mwk_ids[i] = UINT32_MAX;
  {
    uint32_t unique_mwk_count = 0, j;
    for (i = 0; i < song_count; i++) {
      if (!mwk_count || !mwk_paths[i]) continue;
      for (j = 0; j < i; j++) {
        if (mwk_paths[j] && mwks[j].size == mwks[i].size &&
            !memcmp(mwks[j].data, mwks[i].data, mwks[i].size)) {
          mwk_ids[i] = mwk_ids[j];
          break;
        }
      }
      if (mwk_ids[i] == UINT32_MAX) mwk_ids[i] = unique_mwk_count++;
    }
  }
  if (max_song_size > 0x8000u || 0x4000u + max_song_size > 0xFFFFu) {
    fprintf(stderr, "a song is too large for the fixed-window bootstrap\n"); goto failure;
  }
  prefix = (uint8_t *)malloc(KSP_KSS_HEADER_SIZE);
  if (!prefix) { fprintf(stderr, "out of memory\n"); goto failure; }
  make_kss_header(prefix, 0x4000u + max_song_size, song_count);
  ksp_encode_engine_descriptor(&descriptor, descriptor_data);
  memset(chunks, 0, sizeof(chunks));
  memcpy(chunks[0].type, "ENGN", 4); chunks[0].data = engine; chunks[0].size = DRIVER_SIZE;
  chunks[0].compression = use_zx0 ? KSP_COMPRESSION_ZX0 : KSP_COMPRESSION_NONE;
  memcpy(chunks[1].type, "EDES", 4); chunks[1].data = descriptor_data;
  chunks[1].size = sizeof(descriptor_data);
  chunk_count = 2;
  for (i = 0; i < song_count; i++) {
    uint32_t n = chunk_count++;
    memcpy(chunks[n].type, "SONG", 4); chunks[n].id = i; chunks[n].data = songs[i].data;
    chunks[n].size = songs[i].size;
    chunks[n].aux = mwk_ids[i];
    chunks[n].compression = use_zx0 ? KSP_COMPRESSION_ZX0 : KSP_COMPRESSION_NONE;
    if (mwk_count && mwk_paths[i]) {
      uint32_t j, already_emitted = 0;
      for (j = 0; j < i; j++)
        if (mwk_paths[j] && mwk_ids[j] == mwk_ids[i]) already_emitted = 1;
      if (!already_emitted) {
        n = chunk_count++; memcpy(chunks[n].type, "MWK ", 4); chunks[n].id = mwk_ids[i];
        chunks[n].data = mwks[i].data; chunks[n].size = mwks[i].size;
      }
    }
  }
  if (title || author || game) {
    metadata_length = snprintf(metadata, sizeof(metadata), "title=%s\nauthor=%s\ngame=%s\n",
                                title ? title : "", author ? author : "", game ? game : "");
    if (metadata_length < 0 || (size_t)metadata_length >= sizeof(metadata)) {
      fprintf(stderr, "metadata is too long\n"); goto failure;
    }
    memcpy(chunks[chunk_count].type, "META", 4); chunks[chunk_count].data = (const uint8_t *)metadata;
    chunks[chunk_count++].size = (uint32_t)metadata_length;
  }
  if (!ksp_write_file(output_path, prefix, KSP_KSS_HEADER_SIZE, chunks, chunk_count,
                      error, sizeof(error))) {
    fprintf(stderr, "could not write KSP: %s\n", error); goto failure;
  }
  printf("wrote %s: %u song(s), %u chunks%s\n", output_path, song_count, chunk_count,
         use_zx0 ? " (ZX0 ENGN/SONG)" : "");
  free(prefix); free(engine); free(driver.data);
  for (i = 0; i < song_count; i++) { free(songs[i].data); free(mwks[i].data); }
  return 0;
failure:
  free(prefix); free(engine); free(driver.data);
  for (i = 0; i < song_count; i++) { free(songs[i].data); free(mwks[i].data); }
  return 1;
}
