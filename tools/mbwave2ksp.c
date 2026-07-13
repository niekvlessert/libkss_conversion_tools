#include "ksp/engine.h"
#include "ksp/ksp.h"

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
#define MWM_SIGNATURE_SIZE 6u
#define MWM_FILE_HEADER_SIZE 0x116u
#define MWM_PATTERN_HEADER_SIZE 3u

typedef struct {
  uint8_t *data;
  uint32_t size;
} BLOB;

static void put16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
}

static uint16_t get16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int read_blob(const char *path, BLOB *blob) {
  FILE *file = fopen(path, "rb");
  long length;

  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
    if (file) fclose(file);
    return 0;
  }
  blob->data = (uint8_t *)malloc(length ? (size_t)length : 1);
  if (!blob->data ||
      (length && fread(blob->data, 1, (size_t)length, file) != (size_t)length)) {
    free(blob->data);
    blob->data = NULL;
    fclose(file);
    return 0;
  }
  fclose(file);
  blob->size = (uint32_t)length;
  return 1;
}

/* The on-disk MWM stores a three-byte header before each pattern block. The
 * pattern offsets point into the block data, while MBPLAY expects that block
 * header to have already been consumed by MWMLOAD. Keep the original blob for
 * the KSP SONG chunk, but embed the compact engine image in the KSS window. */
static int make_engine_song(const BLOB *input, BLOB *output) {
  uint32_t position_offset = MWM_SIGNATURE_SIZE + MWM_FILE_HEADER_SIZE;
  uint32_t position_count;
  uint32_t offset_table;
  uint32_t raw_cursor;
  uint32_t compact_cursor;
  uint32_t max_pattern = 0;
  uint32_t patterns = 0;
  uint32_t capacity;
  uint8_t song_length;

  if (input->size < position_offset)
    return 0;
  song_length = input->data[MWM_SIGNATURE_SIZE];
  position_count = (uint32_t)song_length + 1u;
  if (position_offset + position_count > input->size)
    return 0;
  for (uint32_t i = 0; i < position_count; i++) {
    if (input->data[position_offset + i] > max_pattern)
      max_pattern = input->data[position_offset + i];
  }
  offset_table = position_offset + position_count;
  if (offset_table + (max_pattern + 1u) * 2u > input->size)
    return 0;

  capacity = input->size;
  output->data = (uint8_t *)malloc(capacity);
  if (!output->data)
    return 0;
  memcpy(output->data, input->data, offset_table + (max_pattern + 1u) * 2u);
  compact_cursor = offset_table + (max_pattern + 1u) * 2u;
  raw_cursor = compact_cursor;

  while (patterns <= max_pattern) {
    uint32_t block_size;
    uint32_t block_patterns;
    if (raw_cursor + MWM_PATTERN_HEADER_SIZE > input->size) {
      free(output->data);
      output->data = NULL;
      return 0;
    }
    block_size = get16(input->data + raw_cursor);
    block_patterns = input->data[raw_cursor + 2u];
    raw_cursor += MWM_PATTERN_HEADER_SIZE;
    if (block_patterns == 0 || raw_cursor + block_size > input->size ||
        patterns + block_patterns > max_pattern + 1u) {
      free(output->data);
      output->data = NULL;
      return 0;
    }
    memcpy(output->data + compact_cursor, input->data + raw_cursor, block_size);
    compact_cursor += block_size;
    raw_cursor += block_size;
    patterns += block_patterns;
  }

  if (raw_cursor < input->size) {
    memcpy(output->data + compact_cursor, input->data + raw_cursor,
           input->size - raw_cursor);
    compact_cursor += input->size - raw_cursor;
  }
  output->size = compact_cursor;
  return 1;
}

static void usage(const char *name) {
  fprintf(stderr,
          "usage: %s --driver WAVEDRVR.BIN --song FILE.MWM --output FILE.ksp\n"
          "          [--mwk FILE.MWK] [--title TEXT] [--author TEXT] [--game TEXT]\n",
          name);
}

static int parse_args(int argc, char **argv, const char **driver,
                      const char **song, const char **output,
                      const char **mwk,
                      const char **title, const char **author,
                      const char **game) {
  int i;
  for (i = 1; i < argc; i++) {
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(argv[i], "--driver") && value) *driver = value;
    else if (!strcmp(argv[i], "--song") && value) *song = value;
    else if (!strcmp(argv[i], "--output") && value) *output = value;
    else if (!strcmp(argv[i], "--mwk") && value) *mwk = value;
    else if (!strcmp(argv[i], "--title") && value) *title = value;
    else if (!strcmp(argv[i], "--author") && value) *author = value;
    else if (!strcmp(argv[i], "--game") && value) *game = value;
    else return 0;
    i++;
  }
  return *driver && *song && *output;
}

static int extract_driver(const BLOB *input, uint8_t **engine) {
  size_t source_offset;
  if (input->size < MSX_HEADER_SIZE || input->data[0] != 0xFE ||
      get16(input->data + 1) != WAVEDRVR_LOAD ||
      get16(input->data + 3) != WAVEDRVR_END ||
      get16(input->data + 5) != WAVEDRVR_EXEC ||
      (uint32_t)(WAVEDRVR_END - WAVEDRVR_LOAD) != input->size - MSX_HEADER_SIZE) {
    return 0;
  }
  source_offset = MSX_HEADER_SIZE + (DRIVER_SOURCE - WAVEDRVR_LOAD);
  if (source_offset > input->size || DRIVER_SIZE > input->size - source_offset ||
      input->data[source_offset] != 'A' || input->data[source_offset + 1] != 'B' ||
      input->data[source_offset + 2] != 0) {
    return 0;
  }
  *engine = (uint8_t *)malloc(DRIVER_SIZE);
  if (!*engine) return 0;
  memcpy(*engine, input->data + source_offset, DRIVER_SIZE);
  return 1;
}

static void make_kss_prefix(uint8_t *prefix, uint32_t prefix_size,
                            const uint8_t *engine, const BLOB *song_image) {
  uint32_t load_size = prefix_size - 0x20;
  memset(prefix, 0, prefix_size);
  memcpy(prefix, "KSSX", 4);
  put16(prefix + 0x04, DRIVER_LOAD);
  put16(prefix + 0x06, (uint16_t)load_size);
  put16(prefix + 0x08, ENGINE_INIT);
  put16(prefix + 0x0A, ENGINE_PLAY);
  prefix[0x0C] = 0;
  prefix[0x0D] = 0;
  prefix[0x0E] = 0x10;
  prefix[0x0F] = 0x00; /* OPL4 is described by EDES, not legacy KSS flags. */
  memcpy(prefix + 0x20, engine, DRIVER_SIZE);
  /* MBPLAY consumes the song address from DA04H. KSS init normally only
   * supplies the song number in A, so use the unused gap after the extracted
   * driver as a tiny ABI adapter. */
  {
    uint32_t offset = ENGINE_BOOTSTRAP - DRIVER_LOAD;
    const uint8_t bootstrap[] = {
        0xAF,                   /* XOR A */
        0x21, 0x00, 0xDA,       /* LD HL,DA00H */
        0x06, 0x24,             /* LD B,24H */
        0x77,                   /* clear: LD (HL),A */
        0x23,                   /* INC HL */
        0x10, 0xFC,             /* DJNZ clear */
        0x21, 0x06, 0x80,       /* LD HL,8006H (skip MBMS signature) */
        0x22, 0x04, 0xDA,       /* LD (DA04H),HL */
        0x3E, 0x03,             /* LD A,03H */
        0x32, 0x01, 0xDA,       /* LD (DA01H),A */
        0x32, 0x02, 0xDA,       /* LD (DA02H),A */
        0x32, 0x03, 0xDA,       /* LD (DA03H),A */
        0xCD, 0x42, 0x40,       /* CALL MBPLAY */
        0xC9                    /* RET */
    };
    memcpy(prefix + 0x20 + offset, bootstrap, sizeof(bootstrap));
  }
  memcpy(prefix + 0x20 + (SONG_LOAD - DRIVER_LOAD), song_image->data,
         song_image->size);
}

int main(int argc, char **argv) {
  const char *driver_path = NULL, *song_path = NULL, *output_path = NULL;
  const char *mwk_path = NULL;
  const char *title = NULL, *author = NULL, *game = NULL;
  BLOB driver = {0}, song = {0}, song_image = {0}, mwk = {0};
  uint8_t *engine = NULL;
  uint8_t *prefix = NULL;
  uint8_t descriptor_data[KSP_ENGINE_DESCRIPTOR_SIZE];
  KSP_ENGINE_DESCRIPTOR descriptor = {
      1, DRIVER_LOAD, ENGINE_INIT, ENGINE_PLAY, ENGINE_STOP, 0,
      SONG_LOAD, 0xDA00, 0x0100, 60, 1, 0, 0};
  KSP_CHUNK chunks[5];
  uint32_t chunk_count = 3;
  uint32_t load_size;
  uint32_t prefix_size;
  char metadata[2048];
  char error[256];
  int metadata_length;

  if (!parse_args(argc, argv, &driver_path, &song_path, &output_path,
                  &mwk_path,
                  &title, &author, &game)) {
    usage(argv[0]);
    return 2;
  }
  if (!read_blob(driver_path, &driver) || !read_blob(song_path, &song)) {
    fprintf(stderr, "could not read the driver or song\n");
    goto failure;
  }
  if (mwk_path && !read_blob(mwk_path, &mwk)) {
    fprintf(stderr, "could not read MWK: %s\n", mwk_path);
    goto failure;
  }
  if (song.size < 4 || memcmp(song.data, "MBMS", 4) != 0) {
    fprintf(stderr, "%s is not an MBMS/MWM song\n", song_path);
    goto failure;
  }
  if (!extract_driver(&driver, &engine)) {
    fprintf(stderr, "%s is not the expected DMV1 WAVEDRVR.BIN\n", driver_path);
    goto failure;
  }
  if (!make_engine_song(&song, &song_image)) {
    fprintf(stderr, "%s is too short to contain an MWM header\n", song_path);
    goto failure;
  }
  if (song_image.size > 0x8000u ||
      (load_size = 0x4000u + song_image.size) > 0xFFFFu) {
    fprintf(stderr, "song is too large for the first fixed-window bootstrap\n");
    goto failure;
  }
  prefix_size = 0x20u + load_size;
  prefix = (uint8_t *)malloc(prefix_size);
  if (!prefix) {
    fprintf(stderr, "out of memory\n");
    goto failure;
  }
  make_kss_prefix(prefix, prefix_size, engine, &song_image);
  ksp_encode_engine_descriptor(&descriptor, descriptor_data);
  memset(chunks, 0, sizeof(chunks));
  memcpy(chunks[0].type, "ENGN", 4);
  chunks[0].data = engine;
  chunks[0].size = DRIVER_SIZE;
  memcpy(chunks[1].type, "SONG", 4);
  chunks[1].data = song.data;
  chunks[1].size = song.size;
  memcpy(chunks[2].type, "EDES", 4);
  chunks[2].data = descriptor_data;
  chunks[2].size = sizeof(descriptor_data);
  if (mwk_path) {
    memcpy(chunks[chunk_count].type, "MWK ", 4);
    chunks[chunk_count].data = mwk.data;
    chunks[chunk_count].size = mwk.size;
    chunk_count++;
  }
  if (title || author || game) {
    metadata_length = snprintf(metadata, sizeof(metadata), "title=%s\nauthor=%s\ngame=%s\n",
                                title ? title : "", author ? author : "",
                                game ? game : "");
    if (metadata_length < 0 || (size_t)metadata_length >= sizeof(metadata)) {
      fprintf(stderr, "metadata is too long\n");
      goto failure;
    }
    memcpy(chunks[chunk_count].type, "META", 4);
    chunks[chunk_count].data = (const uint8_t *)metadata;
    chunks[chunk_count].size = (uint32_t)metadata_length;
    chunk_count++;
  }
  if (!ksp_write_file(output_path, prefix, prefix_size, chunks, chunk_count,
                      error, sizeof(error))) {
    fprintf(stderr, "could not write KSP: %s\n", error);
    goto failure;
  }
  printf("wrote %s: KSS load=4000H size=%04XH init=%04XH play=45E5H, %u chunks\n",
         output_path, load_size, ENGINE_INIT, chunk_count);
  free(prefix);
  free(engine);
  free(driver.data);
  free(song.data);
  free(song_image.data);
  free(mwk.data);
  return 0;

failure:
  free(prefix);
  free(engine);
  free(driver.data);
  free(song.data);
  free(song_image.data);
  free(mwk.data);
  return 1;
}
