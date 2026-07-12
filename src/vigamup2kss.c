/*
 * Merge the Konami MSX KSS files from the VGM Up archive into one KSSX.
 *
 * The original files are already complete KSS machine images.  This tool
 * keeps their engines and banked data intact, but loads each initial image
 * through a shared ZX0 decoder selected by a small dispatcher.  The two
 * source files using the 8K mapper are normalized to the common 16K mapper;
 * the other source engines keep their original mapper instructions, with
 * their bank numbers redirected to the merged bank area.
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "zx_compressor.h"
#include "zx_decompressors.h"

#define KSS_HEADER_SIZE 0x20
#define KSS_TITLE_MAX 256
#define KSS_MAX_LOAD_SIZE 0xFE00
#define KSS_BANK_SIZE 0x4000
#define KSS_BANK_OFFSET 6
#define KSS_MAX_BANKS 127
#define KSS_MAX_TRACKS 256
#define INITIAL_CHUNK_SIZE 0x2000

#define MERGED_LOAD_ADDRESS 0x0200
#define MERGED_INIT_ADDRESS 0xF000
#define MERGED_DECODER_ADDRESS 0xF200
#define MERGED_STUB_ADDRESS 0xE400
#define MERGED_VARIABLE_ADDRESS 0xF5C0
#define MERGED_GAME_DESCRIPTOR_ADDRESS 0xF600
#define MERGED_CHUNK_DESCRIPTOR_ADDRESS 0xF700
#define MERGED_TRACK_TABLE_ADDRESS 0xF940

#define CURRENT_GAME_ADDRESS (MERGED_VARIABLE_ADDRESS + 0)
#define CURRENT_SONG_ADDRESS (MERGED_VARIABLE_ADDRESS + 1)
#define CURRENT_DESC_ADDRESS (MERGED_VARIABLE_ADDRESS + 2)
#define CURRENT_CHUNK_PTR_ADDRESS (MERGED_VARIABLE_ADDRESS + 4)
#define CURRENT_CHUNK_COUNT_ADDRESS (MERGED_VARIABLE_ADDRESS + 6)
#define CURRENT_PLAY_ADDRESS (MERGED_VARIABLE_ADDRESS + 8)
#define CURRENT_DEST_ADDRESS (MERGED_VARIABLE_ADDRESS + 10)
#define CURRENT_COPY_OFFSET_ADDRESS (MERGED_VARIABLE_ADDRESS + 12)
#define CURRENT_COPY_LENGTH_ADDRESS (MERGED_VARIABLE_ADDRESS + 14)
#define CURRENT_MAIN_BANK_ADDRESS (MERGED_VARIABLE_ADDRESS + 16)
#define CURRENT_8K_PAGE4_ADDRESS (MERGED_VARIABLE_ADDRESS + 18)
#define CURRENT_8K_PAGE5_ADDRESS (MERGED_VARIABLE_ADDRESS + 19)

#define GAME_DESCRIPTOR_SIZE 8
#define CHUNK_DESCRIPTOR_SIZE 9
#define MAX_GAME_NAME 64
#define MAX_METADATA 512
#define MAX_TRACK_TITLE 256

typedef struct {
  int id;
  int seconds;
  int fade_seconds;
  int loop;
  char title[MAX_TRACK_TITLE];
  int valid;
} TRACK_META;

typedef struct {
  char title[MAX_METADATA];
  char full_title[MAX_METADATA];
  char japanese_title[MAX_METADATA];
  char vendor[MAX_METADATA];
  char year[MAX_METADATA];
  char composers[MAX_METADATA];
  char chips[MAX_METADATA];
  int tracks_to_play[KSS_MAX_TRACKS];
  uint32_t track_count;
} GAME_META;

typedef struct {
  uint8_t bank;
  uint16_t source;
  uint16_t destination;
  uint16_t copy_offset;
  uint16_t copy_length;
} CHUNK_REFERENCE;

typedef struct {
  uint32_t image_offset;
  uint32_t branch_offset;
  uint8_t next[3];
  uint8_t next_length;
  uint8_t opcode_length;
  uint8_t mapper_port;
} BANK_PATCH;

typedef struct {
  char name[MAX_GAME_NAME];
  char path[1024];
  uint8_t *file_data;
  uint32_t file_size;
  uint16_t load_address;
  uint16_t load_size;
  uint16_t init_address;
  uint16_t play_address;
  uint8_t bank_offset;
  uint8_t bank_count;
  uint8_t bank_mode;
  uint8_t *initial;
  uint32_t initial_size;
  uint32_t global_bank_base;
  uint8_t main_bank;
  uint8_t combo_options;
  uint32_t chunk_count;
  CHUNK_REFERENCE *chunks;
  BANK_PATCH *patches;
  uint32_t patch_count;
  uint16_t descriptor_address;
  GAME_META meta;
  TRACK_META track_meta[KSS_MAX_TRACKS];
} GAME;

typedef struct {
  GAME *game;
  int source_song;
  int seconds;
  int fade_seconds;
  int loop;
  char title[KSS_TITLE_MAX];
} OUTPUT_TRACK;

typedef struct {
  uint8_t *data;
  uint32_t count;
  uint32_t raw_count;
  uint32_t stream_offset;
} BANK_PACK;

typedef struct {
  uint8_t *data;
  uint32_t size;
} OUTPUT_KSS;

static uint16_t get_word(const uint8_t *data, uint32_t offset) {
  return (uint16_t)(data[offset] | ((uint16_t)data[offset + 1] << 8));
}

static void set_word(uint8_t *data, uint32_t offset, uint16_t value) {
  data[offset] = (uint8_t)(value & 0xFF);
  data[offset + 1] = (uint8_t)(value >> 8);
}

static void set_dword(uint8_t *data, uint32_t offset, uint32_t value) {
  data[offset] = (uint8_t)(value & 0xFF);
  data[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
  data[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
  data[offset + 3] = (uint8_t)(value >> 24);
}

static void emit_byte(uint8_t *image, uint32_t *address, uint8_t value) {
  image[*address - MERGED_LOAD_ADDRESS] = value;
  (*address)++;
}

static void emit_word(uint8_t *image, uint32_t *address, uint16_t value) {
  emit_byte(image, address, (uint8_t)(value & 0xFF));
  emit_byte(image, address, (uint8_t)(value >> 8));
}

static void patch_word(uint8_t *image, uint32_t address, uint16_t value) {
  set_word(image, address - MERGED_LOAD_ADDRESS, value);
}

static void patch_relative(uint8_t *image, uint32_t address, uint32_t target) {
  int32_t delta = (int32_t)target - (int32_t)(address + 2);
  image[address - MERGED_LOAD_ADDRESS + 1] = (uint8_t)delta;
}

static char *trim(char *value) {
  char *end;
  while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')
    value++;
  end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    *--end = '\0';
  return value;
}

static void copy_text(char *destination, size_t destination_size, const char *source) {
  if (destination_size == 0)
    return;
  strncpy(destination, source ? source : "", destination_size - 1);
  destination[destination_size - 1] = '\0';
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
    fclose(file);
    fprintf(stderr, "%s: failed to determine file size\n", path);
    return 1;
  }
  if ((unsigned long)length > UINT32_MAX) {
    fclose(file);
    fprintf(stderr, "%s: file is too large\n", path);
    return 1;
  }
  buffer = (uint8_t *)malloc((size_t)length);
  if (!buffer) {
    fclose(file);
    return 1;
  }
  if (length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
    fclose(file);
    free(buffer);
    fprintf(stderr, "%s: read failed\n", path);
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
  if (fwrite(data, 1, size, file) != size || fclose(file) != 0) {
    fprintf(stderr, "%s: write failed\n", path);
    return 1;
  }
  return 0;
}

static int is_extension(const char *path, const char *extension) {
  const char *base = strrchr(path, '/');
  const char *dot;
  base = base ? base + 1 : path;
  dot = strrchr(base, '.');
  return dot && strcasecmp(dot, extension) == 0;
}

static void make_path(char *destination, size_t size, const char *directory,
                      const char *name, const char *extension) {
  snprintf(destination, size, "%s/%s%s", directory, name, extension);
}

static void parse_track_ids(GAME_META *meta, char *value) {
  char *token;
  meta->track_count = 0;
  token = strtok(value, ",");
  while (token && meta->track_count < KSS_MAX_TRACKS) {
    char *end;
    long id = strtol(trim(token), &end, 10);
    if (end != token && id >= 0 && id < KSS_MAX_TRACKS)
      meta->tracks_to_play[meta->track_count++] = (int)id;
    token = strtok(NULL, ",");
  }
}

static int read_gameinfo(const char *path, GAME_META *meta) {
  FILE *file;
  char line[2048];

  memset(meta, 0, sizeof(*meta));
  file = fopen(path, "r");
  if (!file) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  while (fgets(line, sizeof(line), file)) {
    char *separator;
    char *key;
    char *value;
    line[strcspn(line, "\r\n")] = '\0';
    separator = strchr(line, ':');
    if (!separator)
      continue;
    *separator = '\0';
    key = trim(line);
    value = trim(separator + 1);
    if (strcmp(key, "tracks_to_play") == 0)
      parse_track_ids(meta, value);
    else if (strcmp(key, "title") == 0)
      copy_text(meta->title, sizeof(meta->title), value);
    else if (strcmp(key, "full_title") == 0)
      copy_text(meta->full_title, sizeof(meta->full_title), value);
    else if (strcmp(key, "japanese_title") == 0)
      copy_text(meta->japanese_title, sizeof(meta->japanese_title), value);
    else if (strcmp(key, "vendor") == 0)
      copy_text(meta->vendor, sizeof(meta->vendor), value);
    else if (strcmp(key, "year") == 0)
      copy_text(meta->year, sizeof(meta->year), value);
    else if (strcmp(key, "composers") == 0)
      copy_text(meta->composers, sizeof(meta->composers), value);
    else if (strcmp(key, "chips") == 0)
      copy_text(meta->chips, sizeof(meta->chips), value);
  }
  fclose(file);
  return meta->track_count == 0;
}

static int parse_track_line(char *line, TRACK_META *track) {
  char *first;
  char *second;
  char *third;
  char *fourth;
  char *end;
  long id;

  line[strcspn(line, "\r\n")] = '\0';
  first = strchr(line, ',');
  if (!first)
    return 0;
  second = strchr(first + 1, ',');
  if (!second)
    return 0;
  third = strchr(second + 1, ',');
  if (!third)
    return 0;
  fourth = strchr(third + 1, ',');

  *first = '\0';
  *second = '\0';
  *third = '\0';
  if (fourth)
    *fourth = '\0';
  id = strtol(trim(line), &end, 10);
  if (end == line || id < 0 || id >= KSS_MAX_TRACKS)
    return 0;

  memset(track, 0, sizeof(*track));
  track->id = (int)id;
  copy_text(track->title, sizeof(track->title), trim(first + 1));
  track->seconds = (int)strtol(trim(second + 1), NULL, 10);
  if (fourth && trim(third + 1)[0])
    track->fade_seconds = (int)strtol(trim(third + 1), NULL, 10);
  else
    track->fade_seconds = 0;
  track->loop = fourth && (trim(fourth + 1)[0] == 'y' || trim(fourth + 1)[0] == 'Y');
  track->valid = 1;
  return 1;
}

static int read_trackinfo(const char *path, TRACK_META *tracks) {
  FILE *file;
  char line[2048];

  memset(tracks, 0, sizeof(TRACK_META) * KSS_MAX_TRACKS);
  file = fopen(path, "r");
  if (!file) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  while (fgets(line, sizeof(line), file)) {
    TRACK_META track;
    if (!parse_track_line(line, &track))
      continue;
    tracks[track.id] = track;
  }
  fclose(file);
  return 0;
}

static int compare_games(const void *left, const void *right) {
  const GAME *a = (const GAME *)left;
  const GAME *b = (const GAME *)right;
  return strcasecmp(a->name, b->name);
}

static int load_source_kss(GAME *game) {
  uint32_t bank_size;
  uint32_t bank_bytes;
  uint32_t rounded_size;

  if (read_file(game->path, &game->file_data, &game->file_size))
    return 1;
  if (game->file_size < 0x10 || memcmp(game->file_data, "KSCC", 4) != 0) {
    fprintf(stderr, "%s: expected a KSCC KSS file\n", game->path);
    return 1;
  }
  game->load_address = get_word(game->file_data, 4);
  game->load_size = get_word(game->file_data, 6);
  game->init_address = get_word(game->file_data, 8);
  game->play_address = get_word(game->file_data, 0x0A);
  game->bank_offset = game->file_data[0x0C];
  game->bank_count = game->file_data[0x0D] & 0x7F;
  game->bank_mode = (game->file_data[0x0D] & 0x80) ? 8 : 16;
  bank_size = game->bank_mode == 8 ? 0x2000 : 0x4000;
  bank_bytes = (uint32_t)game->bank_count * bank_size;
  if ((uint32_t)game->load_address + game->load_size > 0x10000 ||
      0x10u + game->load_size + bank_bytes > game->file_size) {
    fprintf(stderr, "%s: invalid KSS image bounds\n", game->path);
    return 1;
  }
  rounded_size = (game->load_size + INITIAL_CHUNK_SIZE - 1) & ~(INITIAL_CHUNK_SIZE - 1);
  if ((uint32_t)game->load_address + rounded_size > MERGED_INIT_ADDRESS) {
    fprintf(stderr, "%s: initial image reaches the merged dispatcher\n", game->path);
    return 1;
  }
  if ((uint32_t)game->load_address + rounded_size > MERGED_STUB_ADDRESS) {
    fprintf(stderr, "%s: initial image reaches the bank-switch stubs\n", game->path);
    return 1;
  }
  game->initial = (uint8_t *)malloc(rounded_size);
  if (!game->initial)
    return 1;
  /* VM_init_memory zero-fills the part of the main RAM image outside the KSS load. */
  memset(game->initial, 0, rounded_size);
  memcpy(game->initial, game->file_data + 0x10, game->load_size);
  game->initial_size = rounded_size;
  return 0;
}

static int bank_pack_reserve(BANK_PACK *pack, uint32_t count) {
  uint8_t *new_data;
  if (count > KSS_MAX_BANKS)
    return 1;
  new_data = (uint8_t *)realloc(pack->data, count * KSS_BANK_SIZE);
  if (!new_data)
    return 1;
  pack->data = new_data;
  memset(pack->data + pack->count * KSS_BANK_SIZE, 0xC9,
         (count - pack->count) * KSS_BANK_SIZE);
  pack->count = count;
  return 0;
}

static uint32_t bank_pack_add_raw(BANK_PACK *pack, const uint8_t *data) {
  uint32_t bank;
  if (pack->stream_offset != 0 || bank_pack_reserve(pack, pack->count + 1))
    return 0;
  bank = KSS_BANK_OFFSET + pack->count - 1;
  memcpy(pack->data + (pack->count - 1) * KSS_BANK_SIZE, data, KSS_BANK_SIZE);
  pack->raw_count = pack->count;
  return bank;
}

static uint32_t bank_pack_add_stream(BANK_PACK *pack, const uint8_t *data,
                                     uint32_t size, uint16_t *source) {
  uint32_t bank;
  uint32_t offset;

  if (!data || size == 0 || size > KSS_BANK_SIZE)
    return 0;
  if (pack->count == pack->raw_count || pack->stream_offset + size > KSS_BANK_SIZE) {
    if (bank_pack_reserve(pack, pack->count + 1))
      return 0;
    pack->stream_offset = 0;
  }
  bank = KSS_BANK_OFFSET + pack->count - 1;
  offset = pack->stream_offset;
  memcpy(pack->data + (pack->count - 1) * KSS_BANK_SIZE + offset, data, size);
  pack->stream_offset += size;
  *source = (uint16_t)(0x8000 + offset);
  return bank;
}

static int prepare_source_banks(GAME *game, BANK_PACK *pack) {
  uint32_t i;

  game->global_bank_base = 0;
  if (game->bank_count != 0) {
    game->global_bank_base = pack->count + KSS_BANK_OFFSET;
  }
  if (game->bank_count != 0 && game->bank_mode == 16) {
    uint32_t bank_data_offset = 0x10 + game->load_size;
    for (i = 0; i < game->bank_count; i++) {
      if (!bank_pack_add_raw(pack, game->file_data + bank_data_offset + i * 0x4000))
        return 1;
    }
  }
  return 0;
}

static void make_main_window(const GAME *game, uint8_t *window) {
  uint32_t begin = game->load_address;
  uint32_t end = (uint32_t)game->load_address + game->initial_size;
  uint32_t copy_begin;
  uint32_t copy_end;

  memset(window, 0, KSS_BANK_SIZE);
  copy_begin = begin > 0x8000 ? begin : 0x8000;
  copy_end = end < 0xC000 ? end : 0xC000;
  if (copy_begin < copy_end)
    memcpy(window + copy_begin - 0x8000, game->initial + copy_begin - begin,
           copy_end - copy_begin);
}

static int add_main_window_bank(GAME *game, BANK_PACK *pack) {
  uint8_t window[KSS_BANK_SIZE];

  make_main_window(game, window);
  game->main_bank = (uint8_t)bank_pack_add_raw(pack, window);
  return game->main_bank ? 0 : 1;
}

static int add_8k_combo_banks(GAME *game, BANK_PACK *pack) {
  uint8_t main_window[KSS_BANK_SIZE];
  uint32_t bank_data_offset = 0x10 + game->load_size;
  uint32_t options = game->bank_count + 2; /* source banks, main, dummy */
  uint32_t page4;
  uint32_t page5;

  if (options > 0xFF)
    return 1;
  make_main_window(game, main_window);
  game->global_bank_base = pack->count + KSS_BANK_OFFSET;
  game->combo_options = (uint8_t)options;
  for (page4 = 0; page4 < options; page4++) {
    for (page5 = 0; page5 < options; page5++) {
      uint8_t combo[KSS_BANK_SIZE];
      memset(combo, 0, sizeof(combo)); /* invalid selectors read as zero */
      if (page4 < game->bank_count)
        memcpy(combo, game->file_data + bank_data_offset + page4 * 0x2000, 0x2000);
      else if (page4 == game->bank_count)
        memcpy(combo, main_window, 0x2000);
      if (page5 < game->bank_count)
        memcpy(combo + 0x2000,
               game->file_data + bank_data_offset + page5 * 0x2000, 0x2000);
      else if (page5 == game->bank_count)
        memcpy(combo + 0x2000, main_window + 0x2000, 0x2000);
      if (!bank_pack_add_raw(pack, combo))
        return 1;
    }
  }
  return 0;
}

static int next_instruction_length(const uint8_t *data, uint32_t size,
                                   uint32_t offset);

static int find_branch_to(const GAME *game, uint32_t target, uint32_t *branch_offset) {
  uint32_t i;
  for (i = 0; i + 1 < game->load_size; i++) {
    uint8_t op = game->initial[i];
    uint32_t destination;
    if (op == 0x10 || op == 0x18 || (op >= 0x20 && op <= 0x38 && (op & 7) == 0)) {
      destination = game->load_address + i + 2 + (int8_t)game->initial[i + 1];
      if (destination == target) {
        if (branch_offset)
          *branch_offset = i;
        return 1;
      }
    } else if (op == 0xC3 || op == 0xCD || op == 0xC2 || op == 0xCA ||
               op == 0xD2 || op == 0xDA || op == 0xE2 || op == 0xEA ||
               op == 0xF2 || op == 0xFA) {
      if (i + 2 < game->load_size && get_word(game->initial, i + 1) == target) {
        if (branch_offset)
          *branch_offset = i;
        return 1;
      }
    }
  }
  return 0;
}

static int collect_8k_mapper_patches(GAME *game) {
  uint32_t i;
  uint32_t count = 0;

  for (i = 0; i + 2 < game->load_size; i++) {
    if (game->initial[i] == 0x32 && game->initial[i + 1] == 0x00 &&
        (game->initial[i + 2] == 0x90 || game->initial[i + 2] == 0xB0)) {
      if (!next_instruction_length(game->initial, game->load_size, i + 3)) {
        fprintf(stderr, "%s: unsupported instruction after 8K mapper write at 0x%04X\n",
                game->path, (unsigned)(game->load_address + i));
        return 1;
      }
      count++;
    }
  }
  if (count == 0) {
    fprintf(stderr, "%s: could not find 8K mapper selectors\n", game->path);
    return 1;
  }
  game->patches = (BANK_PATCH *)calloc(count, sizeof(*game->patches));
  if (!game->patches)
    return 1;
  game->patch_count = count;
  count = 0;
  for (i = 0; i + 2 < game->load_size; i++) {
    int length;
    if (game->initial[i] != 0x32 || game->initial[i + 1] != 0x00 ||
        (game->initial[i + 2] != 0x90 && game->initial[i + 2] != 0xB0))
      continue;
    length = next_instruction_length(game->initial, game->load_size, i + 3);
    game->patches[count].image_offset = i;
    game->patches[count].branch_offset = UINT32_MAX;
    game->patches[count].opcode_length = 3;
    game->patches[count].mapper_port = game->initial[i + 2];
    game->patches[count].next_length = (uint8_t)length;
    memcpy(game->patches[count].next, game->initial + i + 3, (size_t)length);
    game->initial[i] = 0xC3;
    game->initial[i + 1] = 0;
    game->initial[i + 2] = 0;
    count++;
  }
  fprintf(stderr, "%s: normalized %u 8K mapper writes\n", game->name,
          (unsigned)count);
  return 0;
}

static int next_instruction_length(const uint8_t *data, uint32_t size,
                                   uint32_t offset) {
  if (offset >= size)
    return 0;
  if ((data[offset] >= 0x40 && data[offset] <= 0xBF) && data[offset] != 0xCB)
    return 1;
  switch (data[offset]) {
  case 0x06:
  case 0x0E:
  case 0x16:
  case 0x1E:
  case 0x26:
  case 0x2E:
  case 0x36:
  case 0x3E:
    return offset + 1 < size ? 2 : 0;
  case 0x10:
  case 0x18:
  case 0x20:
  case 0x28:
  case 0x30:
  case 0x38:
    return offset + 1 < size ? 2 : 0;
  case 0x01:
  case 0x11:
  case 0x21:
  case 0x2A:
  case 0x22:
  case 0x31:
  case 0x32:
  case 0x3A:
  case 0xC3:
  case 0xCD:
    return offset + 2 < size ? 3 : 0;
  case 0x79:
  case 0x78:
  case 0xC0:
  case 0x0C:
  case 0x23:
  case 0xC9:
  case 0xF1:
  case 0xAF:
  case 0x00:
    return 1;
  default:
    return 0;
  }
}

static int collect_16k_bank_patches(GAME *game) {
  uint32_t i;
  uint32_t count = 0;

  for (i = 0; i + 2 < game->load_size; i++) {
    if (game->initial[i] == 0xD3 && game->initial[i + 1] == 0xFE) {
      int length = next_instruction_length(game->initial, game->load_size, i + 2);
      if (!length) {
        fprintf(stderr, "%s: unsupported instruction after bank switch at 0x%04X\n",
                game->path, (unsigned)(game->load_address + i));
        return 1;
      }
      count++;
    }
  }
  if (count == 0)
    return 0;
  game->patches = (BANK_PATCH *)calloc(count, sizeof(*game->patches));
  if (!game->patches)
    return 1;
  game->patch_count = count;
  count = 0;
  for (i = 0; i + 2 < game->load_size; i++) {
    int length;
    uint32_t branch_offset = UINT32_MAX;
    if (game->initial[i] != 0xD3 || game->initial[i + 1] != 0xFE)
      continue;
    length = next_instruction_length(game->initial, game->load_size, i + 2);
    game->patches[count].image_offset = i;
    find_branch_to(game, game->load_address + i + 2, &branch_offset);
    game->patches[count].branch_offset = branch_offset;
    game->patches[count].opcode_length = 2;
    game->patches[count].mapper_port = 0;
    game->patches[count].next_length = (uint8_t)length;
    memcpy(game->patches[count].next, game->initial + i + 2, (size_t)length);
    game->initial[i] = 0xC3;
    game->initial[i + 1] = 0;
    game->initial[i + 2] = 0;
    count++;
  }
  return 0;
}

static int prepare_sources(GAME *games, uint32_t game_count, BANK_PACK *pack) {
  uint32_t i;
  for (i = 0; i < game_count; i++) {
    if (prepare_source_banks(&games[i], pack))
      return 1;
    if (games[i].bank_mode == 8) {
      if (collect_8k_mapper_patches(&games[i]))
        return 1;
    } else if (collect_16k_bank_patches(&games[i])) {
      return 1;
    }
    if (add_main_window_bank(&games[i], pack))
      return 1;
    if (games[i].bank_mode == 8 && add_8k_combo_banks(&games[i], pack))
      return 1;
  }
  return 0;
}

static int build_common_code(uint8_t *image, uint16_t *play_address) {
  uint32_t pc = MERGED_INIT_ADDRESS;
  uint32_t done_jump;
  uint32_t skip_jump;
  uint32_t copy_done_jump;
  uint32_t direct_jump;
  uint32_t decrement_address;
  uint32_t direct_decode;
  uint32_t loop_address;
  uint32_t stack_return_patch;
  uint32_t post_init_return;
  uint32_t play;

  /* Track table entry: [game index, original song number, game descriptor]. */
  emit_byte(image, &pc, 0x6F); /* LD L,A */
  emit_byte(image, &pc, 0x26); /* LD H,0 */
  emit_byte(image, &pc, 0x00);
  emit_byte(image, &pc, 0x29); /* *2 */
  emit_byte(image, &pc, 0x29); /* *4 */
  emit_byte(image, &pc, 0x11);
  emit_word(image, &pc, MERGED_TRACK_TABLE_ADDRESS);
  emit_byte(image, &pc, 0x19); /* ADD HL,DE */
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_GAME_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_SONG_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x5E);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x56);
  emit_byte(image, &pc, 0xED); /* LD (descriptor),DE */
  emit_byte(image, &pc, 0x53);
  emit_word(image, &pc, CURRENT_DESC_ADDRESS);

  emit_byte(image, &pc, 0x2A); /* chunk pointer from descriptor */
  emit_word(image, &pc, CURRENT_DESC_ADDRESS);
  emit_byte(image, &pc, 0x5E);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x56);
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x53);
  emit_word(image, &pc, CURRENT_CHUNK_PTR_ADDRESS);
  emit_byte(image, &pc, 0x2A); /* chunk count from descriptor +2 */
  emit_word(image, &pc, CURRENT_DESC_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_CHUNK_COUNT_ADDRESS);

  emit_byte(image, &pc, 0x2A); /* select the game's initial 8000H-BFFFH bank */
  emit_word(image, &pc, CURRENT_DESC_ADDRESS);
  for (int i = 0; i < 7; i++)
    emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_MAIN_BANK_ADDRESS);
  emit_byte(image, &pc, 0xD3);
  emit_byte(image, &pc, 0xFE);
  emit_byte(image, &pc, 0x3E); /* 8K mapper pages initially use main RAM */
  emit_byte(image, &pc, 0xFF);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_8K_PAGE4_ADDRESS);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_8K_PAGE5_ADDRESS);

  loop_address = pc;
  emit_byte(image, &pc, 0x3A); /* while count != 0 */
  emit_word(image, &pc, CURRENT_CHUNK_COUNT_ADDRESS);
  emit_byte(image, &pc, 0xB7);
  emit_byte(image, &pc, 0xCA); /* JP Z,done */
  done_jump = pc;
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0x2A); /* HL = chunk descriptor pointer */
  emit_word(image, &pc, CURRENT_CHUNK_PTR_ADDRESS);
  emit_byte(image, &pc, 0x7E); /* select compressed bank, or zero for raw window */
  emit_byte(image, &pc, 0xB7);
  emit_byte(image, &pc, 0xCA); /* JP Z,skip_chunk */
  skip_jump = pc;
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0xD3);
  emit_byte(image, &pc, 0xFE);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x5E); /* source low */
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x56); /* source high */
  emit_byte(image, &pc, 0x23); /* destination low */
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_DEST_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_DEST_ADDRESS + 1);
  emit_byte(image, &pc, 0x23); /* copy offset low */
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_COPY_OFFSET_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_COPY_OFFSET_ADDRESS + 1);
  emit_byte(image, &pc, 0x23); /* copy length low */
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_COPY_LENGTH_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x7E);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_COPY_LENGTH_ADDRESS + 1);
  emit_byte(image, &pc, 0x23); /* next descriptor */
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x63); /* LD (nn),HL */
  emit_word(image, &pc, CURRENT_CHUNK_PTR_ADDRESS);
  emit_byte(image, &pc, 0xEB); /* HL = source, DE = next descriptor */
  emit_byte(image, &pc, 0x3A); /* direct upper-memory decode sentinel */
  emit_word(image, &pc, CURRENT_COPY_OFFSET_ADDRESS + 1);
  emit_byte(image, &pc, 0xFE);
  emit_byte(image, &pc, 0xFF);
  emit_byte(image, &pc, 0xCA);
  direct_jump = pc;
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0x11); /* decode into safe C000H scratch */
  emit_word(image, &pc, 0xC000);
  emit_byte(image, &pc, 0xCD);
  emit_word(image, &pc, MERGED_DECODER_ADDRESS);
  emit_byte(image, &pc, 0x3A); /* restore this game's raw main window */
  emit_word(image, &pc, CURRENT_MAIN_BANK_ADDRESS);
  emit_byte(image, &pc, 0xD3);
  emit_byte(image, &pc, 0xFE);
  emit_byte(image, &pc, 0x21); /* HL = scratch + copy offset */
  emit_word(image, &pc, 0xC000);
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x4B);
  emit_word(image, &pc, CURRENT_COPY_OFFSET_ADDRESS);
  emit_byte(image, &pc, 0x09); /* ADD HL,BC */
  emit_byte(image, &pc, 0xED); /* DE = copy destination */
  emit_byte(image, &pc, 0x5B);
  emit_word(image, &pc, CURRENT_DEST_ADDRESS);
  emit_byte(image, &pc, 0xED); /* BC = copy length */
  emit_byte(image, &pc, 0x4B);
  emit_word(image, &pc, CURRENT_COPY_LENGTH_ADDRESS);
  emit_byte(image, &pc, 0x78); /* skip an all-banked chunk */
  emit_byte(image, &pc, 0xB1);
  emit_byte(image, &pc, 0xCA);
  copy_done_jump = pc;
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xC5); /* LDDR: scratch and C000H+ destinations can overlap */
  emit_byte(image, &pc, 0x09); /* HL = source end */
  emit_byte(image, &pc, 0x2B);
  emit_byte(image, &pc, 0xEB); /* HL = destination start, DE = source end */
  emit_byte(image, &pc, 0xC1); /* restore copy length */
  emit_byte(image, &pc, 0x09); /* HL = destination end */
  emit_byte(image, &pc, 0x2B);
  emit_byte(image, &pc, 0xEB); /* HL = source end, DE = destination end */
  emit_byte(image, &pc, 0xED); /* copy the writable part of the chunk backwards */
  emit_byte(image, &pc, 0xB8);
  patch_word(image, copy_done_jump, (uint16_t)pc);
  decrement_address = pc;
  emit_byte(image, &pc, 0x3A);
  emit_word(image, &pc, CURRENT_CHUNK_COUNT_ADDRESS);
  emit_byte(image, &pc, 0x3D);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_CHUNK_COUNT_ADDRESS);
  emit_byte(image, &pc, 0xC3);
  emit_word(image, &pc, (uint16_t)loop_address);

  direct_decode = pc;
  patch_word(image, direct_jump, (uint16_t)direct_decode);
  emit_byte(image, &pc, 0xED); /* DE = the direct upper-memory destination */
  emit_byte(image, &pc, 0x5B);
  emit_word(image, &pc, CURRENT_DEST_ADDRESS);
  emit_byte(image, &pc, 0xCD);
  emit_word(image, &pc, MERGED_DECODER_ADDRESS);
  emit_byte(image, &pc, 0x3A); /* restore this game's raw main window */
  emit_word(image, &pc, CURRENT_MAIN_BANK_ADDRESS);
  emit_byte(image, &pc, 0xD3);
  emit_byte(image, &pc, 0xFE);
  emit_byte(image, &pc, 0xC3);
  emit_word(image, &pc, (uint16_t)decrement_address);

  patch_word(image, skip_jump, (uint16_t)pc);
  emit_byte(image, &pc, 0x11); /* advance over a raw-window descriptor */
  emit_word(image, &pc, CHUNK_DESCRIPTOR_SIZE);
  emit_byte(image, &pc, 0x19);
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x63); /* LD (nn),HL */
  emit_word(image, &pc, CURRENT_CHUNK_PTR_ADDRESS);
  emit_byte(image, &pc, 0x3A);
  emit_word(image, &pc, CURRENT_CHUNK_COUNT_ADDRESS);
  emit_byte(image, &pc, 0x3D);
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, CURRENT_CHUNK_COUNT_ADDRESS);
  emit_byte(image, &pc, 0xC3);
  emit_word(image, &pc, (uint16_t)loop_address);

  patch_word(image, done_jump, (uint16_t)pc);
  emit_byte(image, &pc, 0x3A); /* keep the game's raw main window mapped */
  emit_word(image, &pc, CURRENT_MAIN_BANK_ADDRESS);
  emit_byte(image, &pc, 0xD3);
  emit_byte(image, &pc, 0xFE);
  emit_byte(image, &pc, 0x2A); /* play address */
  emit_word(image, &pc, CURRENT_DESC_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x5E);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x56);
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x53);
  emit_word(image, &pc, CURRENT_PLAY_ADDRESS);
  emit_byte(image, &pc, 0xF3); /* match VM_reset's disabled interrupts */
  emit_byte(image, &pc, 0xAF); /* A=0, then reset I and R */
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x47);
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x4F);
  emit_byte(image, &pc, 0x01); /* clear the general registers used by init */
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0x11);
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xDD); /* IX=0 */
  emit_byte(image, &pc, 0x21);
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xFD); /* IY=0 */
  emit_byte(image, &pc, 0x21);
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0x3C); /* A=1 and F=0; LD A,song preserves F */
  emit_byte(image, &pc, 0x2A); /* reload init address after register clearing */
  emit_word(image, &pc, CURRENT_DESC_ADDRESS);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x5E);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x56);
  emit_byte(image, &pc, 0xEB); /* HL = init */
  emit_byte(image, &pc, 0x11); /* DE=0, matching VM_reset */
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xE5); /* RET through the init address without changing SP */
  emit_byte(image, &pc, 0x21); /* source init returns through our continuation */
  stack_return_patch = pc;
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xED);
  emit_byte(image, &pc, 0x63); /* replace VM_exec's return address at SP */
  emit_word(image, &pc, 0xF37A);
  emit_byte(image, &pc, 0x3A);
  emit_word(image, &pc, CURRENT_SONG_ADDRESS);
  emit_byte(image, &pc, 0x21); /* HL=0 at the source init entry */
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xC9); /* RET to the source init address */

  post_init_return = pc;
  patch_word(image, stack_return_patch, (uint16_t)post_init_return);
  emit_byte(image, &pc, 0xC3); /* return without changing source registers */
  emit_word(image, &pc, 0xF37C);

  play = pc;
  emit_byte(image, &pc, 0xED); /* DE = current play */
  emit_byte(image, &pc, 0x5B);
  emit_word(image, &pc, CURRENT_PLAY_ADDRESS);
  emit_byte(image, &pc, 0xEB);
  emit_byte(image, &pc, 0xE9);
  *play_address = (uint16_t)play;
  return pc <= MERGED_DECODER_ADDRESS ? 0 : 1;
}

static int patch_bank_stubs(GAME *games, uint32_t game_count, uint8_t *image) {
  uint32_t i;
  uint32_t cursor = MERGED_STUB_ADDRESS;

  for (i = 0; i < game_count; i++) {
    uint32_t j;
    for (j = 0; j < games[i].patch_count; j++) {
      BANK_PATCH *patch = &games[i].patches[j];
      uint32_t source_address = games[i].load_address + patch->image_offset +
                                patch->opcode_length + patch->next_length;
      uint32_t stub_address = cursor;
      uint32_t p = cursor;
      uint32_t global_bank = games[i].bank_count ? games[i].global_bank_base : games[i].main_bank;

      if (games[i].bank_mode == 16 && patch->branch_offset != UINT32_MAX) {
        uint32_t branch_stub = cursor;
        uint32_t normal_stub;
        uint32_t branch_offset = patch->branch_offset;
        uint32_t copy_start = branch_offset + 2;
        uint32_t copy_length = patch->image_offset - copy_start;
        uint32_t relocated_start;
        uint8_t branch_opcode;

        /* A conditional branch entered the byte immediately after D3 FE.
         * Replacing that two-byte mapper write with a three-byte JP would
         * overwrite the branch target, so relocate both paths explicitly. */
        branch_opcode = games[i].initial[branch_offset];
        if (branch_opcode == 0x18)
          branch_opcode = 0xC3;
        else if (branch_opcode == 0x20)
          branch_opcode = 0xC2;
        else if (branch_opcode == 0x28)
          branch_opcode = 0xCA;
        else if (branch_opcode == 0x30)
          branch_opcode = 0xD2;
        else if (branch_opcode == 0x38)
          branch_opcode = 0xDA;
        else {
          fprintf(stderr, "%s: unsupported branch opcode before bank switch at 0x%04X\n",
                  games[i].path, (unsigned)(games[i].load_address + branch_offset));
          return 1;
        }

        for (uint32_t k = 0; k < patch->next_length; k++)
          emit_byte(image, &p, patch->next[k]);
        emit_byte(image, &p, 0xC3);
        emit_word(image, &p, (uint16_t)source_address);
        normal_stub = p;

        /* Relocate the source instructions between the branch and mapper
         * write.  Correct relative branches that target this moved range. */
        relocated_start = p;
        memcpy(image + p - MERGED_LOAD_ADDRESS, games[i].initial + copy_start,
               copy_length);
        p += copy_length;
        for (uint32_t source_offset = copy_start;
             source_offset < patch->image_offset; source_offset++) {
          uint8_t op = games[i].initial[source_offset];
          if (op == 0x10 || op == 0x18 ||
              (op >= 0x20 && op <= 0x38 && (op & 7) == 0)) {
            uint32_t target = games[i].load_address + source_offset + 2 +
                              (int8_t)games[i].initial[source_offset + 1];
            uint32_t range_begin = games[i].load_address + copy_start;
            uint32_t range_end = games[i].load_address + patch->image_offset;
            if (target >= range_begin && target < range_end) {
              uint32_t relocated_target = relocated_start + (target - range_begin);
              int32_t displacement = (int32_t)relocated_target -
                                     (int32_t)(relocated_start +
                                               (source_offset - copy_start) + 2);
              image[relocated_start - MERGED_LOAD_ADDRESS +
                    (source_offset - copy_start) + 1] = (uint8_t)displacement;
            }
          }
        }

        /* Normalize the 16K selector, preserving A and flags until the
         * source's following XOR A. */
        {
          uint32_t jump_low;
          uint32_t jump_high;
          uint32_t jump_invalid;
          uint32_t invalid;
          uint32_t after_map;
          emit_byte(image, &p, 0xF5);
          emit_byte(image, &p, 0xFE);
          emit_byte(image, &p, games[i].bank_offset);
          jump_low = p;
          emit_byte(image, &p, 0x38);
          emit_byte(image, &p, 0);
          emit_byte(image, &p, 0xFE);
          emit_byte(image, &p, (uint8_t)(games[i].bank_offset + games[i].bank_count));
          jump_high = p;
          emit_byte(image, &p, 0x30);
          emit_byte(image, &p, 0);
          emit_byte(image, &p, 0xD6);
          emit_byte(image, &p, games[i].bank_offset);
          emit_byte(image, &p, 0xC6);
          emit_byte(image, &p, (uint8_t)global_bank);
          emit_byte(image, &p, 0xD3);
          emit_byte(image, &p, 0xFE);
          jump_invalid = p;
          emit_byte(image, &p, 0x18);
          emit_byte(image, &p, 0);
          invalid = p;
          patch_relative(image, jump_low, invalid);
          patch_relative(image, jump_high, invalid);
          emit_byte(image, &p, 0x3E);
          emit_byte(image, &p, games[i].main_bank);
          emit_byte(image, &p, 0xD3);
          emit_byte(image, &p, 0xFE);
          after_map = p;
          patch_relative(image, jump_invalid, after_map);
        }
        emit_byte(image, &p, 0xF1);
        for (uint32_t k = 0; k < patch->next_length; k++)
          emit_byte(image, &p, patch->next[k]);
        emit_byte(image, &p, 0xC3);
        emit_word(image, &p, (uint16_t)source_address);
        cursor = p;
        if (cursor >= MERGED_VARIABLE_ADDRESS)
          return 1;

        games[i].initial[branch_offset] = branch_opcode;
        set_word(games[i].initial, branch_offset + 1, (uint16_t)branch_stub);
        games[i].initial[branch_offset + 3] = 0xC3; /* JP normal_stub */
        set_word(games[i].initial, branch_offset + 4, (uint16_t)normal_stub);
        games[i].initial[patch->image_offset] = 0xC9;
        games[i].initial[patch->image_offset + 1] = 0xC9;
        games[i].initial[patch->image_offset + 2] = 0xC9;
        continue;
      } else if (games[i].bank_mode == 8) {
        uint32_t jump_low;
        uint32_t jump_high;
        uint32_t invalid;
        uint32_t state_jump;
        uint32_t page4_ready_jump;
        uint32_t page5_ready_jump;
        uint32_t multiply_loop;
        uint32_t multiply_jump;
        uint32_t state_address = patch->mapper_port == 0x90
                                     ? CURRENT_8K_PAGE4_ADDRESS
                                     : CURRENT_8K_PAGE5_ADDRESS;

        /* A 16K KSS bank maps both 8K pages.  Keep the two source page
         * selections in RAM and select a prebuilt combination bank. */
        emit_byte(image, &p, 0xF5); /* preserve source A and flags */
        emit_byte(image, &p, 0xC5); /* preserve BC */
        emit_byte(image, &p, 0xD5); /* preserve DE */
        emit_byte(image, &p, 0xE5); /* preserve HL */
        emit_byte(image, &p, 0xFE); /* valid source selector? */
        emit_byte(image, &p, games[i].bank_offset);
        jump_low = p;
        emit_byte(image, &p, 0x38); /* JR C,invalid */
        emit_byte(image, &p, 0);
        emit_byte(image, &p, 0xFE);
        emit_byte(image, &p, (uint8_t)(games[i].bank_offset + games[i].bank_count));
        jump_high = p;
        emit_byte(image, &p, 0x30); /* JR NC,invalid */
        emit_byte(image, &p, 0);
        emit_byte(image, &p, 0xD6);
        emit_byte(image, &p, games[i].bank_offset);
        emit_byte(image, &p, 0x32);
        emit_word(image, &p, state_address);
        emit_byte(image, &p, 0x18);
        state_jump = p - 1;
        emit_byte(image, &p, 0);
        invalid = p;
        patch_relative(image, jump_low, invalid);
        patch_relative(image, jump_high, invalid);
        emit_byte(image, &p, 0x3E);
        emit_byte(image, &p, (uint8_t)(games[i].bank_count + 1)); /* dummy */
        emit_byte(image, &p, 0x32);
        emit_word(image, &p, state_address);
        patch_relative(image, state_jump, p);

        emit_byte(image, &p, 0x3A);
        emit_word(image, &p, CURRENT_8K_PAGE4_ADDRESS);
        emit_byte(image, &p, 0xFE);
        emit_byte(image, &p, 0xFF);
        emit_byte(image, &p, 0x20); /* page4 already initialized */
        page4_ready_jump = p - 1;
        emit_byte(image, &p, 0);
        emit_byte(image, &p, 0x3E);
        emit_byte(image, &p, games[i].bank_count);
        emit_byte(image, &p, 0x32);
        emit_word(image, &p, CURRENT_8K_PAGE4_ADDRESS);
        patch_relative(image, page4_ready_jump, p);
        emit_byte(image, &p, 0x3A);
        emit_word(image, &p, CURRENT_8K_PAGE5_ADDRESS);
        emit_byte(image, &p, 0xFE);
        emit_byte(image, &p, 0xFF);
        emit_byte(image, &p, 0x20); /* page5 already initialized */
        page5_ready_jump = p - 1;
        emit_byte(image, &p, 0);
        emit_byte(image, &p, 0x3E);
        emit_byte(image, &p, games[i].bank_count);
        emit_byte(image, &p, 0x32);
        emit_word(image, &p, CURRENT_8K_PAGE5_ADDRESS);
        patch_relative(image, page5_ready_jump, p);

        emit_byte(image, &p, 0x3A); /* page 4 state */
        emit_word(image, &p, CURRENT_8K_PAGE4_ADDRESS);
        emit_byte(image, &p, 0x6F); /* L=page 4 */
        emit_byte(image, &p, 0x26);
        emit_byte(image, &p, 0x00); /* H=0 */
        emit_byte(image, &p, 0x5F); /* E=page 4 */
        emit_byte(image, &p, 0x16);
        emit_byte(image, &p, 0x00); /* D=0 */
        emit_byte(image, &p, 0x21); /* HL=0, accumulated product */
        emit_word(image, &p, 0);
        emit_byte(image, &p, 0x06); /* B=number of combinations per row */
        emit_byte(image, &p, games[i].combo_options);
        multiply_loop = p;
        emit_byte(image, &p, 0x19); /* ADD HL,DE */
        emit_byte(image, &p, 0x10); /* DJNZ multiply_loop */
        multiply_jump = p - 1;
        emit_byte(image, &p, 0);
        patch_relative(image, multiply_jump, multiply_loop);
        emit_byte(image, &p, 0x3A); /* add page 5 state */
        emit_word(image, &p, CURRENT_8K_PAGE5_ADDRESS);
        emit_byte(image, &p, 0x5F); /* E=page 5 */
        emit_byte(image, &p, 0x16);
        emit_byte(image, &p, 0x00); /* D=0 */
        emit_byte(image, &p, 0x19);
        emit_byte(image, &p, 0x11); /* add this game's combo-bank base */
        emit_word(image, &p, (uint16_t)global_bank);
        emit_byte(image, &p, 0x19);
        emit_byte(image, &p, 0x7D); /* A=low byte of global bank */
        emit_byte(image, &p, 0xD3);
        emit_byte(image, &p, 0xFE);
        emit_byte(image, &p, 0xE1); /* restore HL, DE, BC, AF */
        emit_byte(image, &p, 0xD1);
        emit_byte(image, &p, 0xC1);
        emit_byte(image, &p, 0xF1);
        emit_byte(image, &p, 0x32); /* preserve SCC/memory side effect */
        emit_byte(image, &p, 0x00);
        emit_byte(image, &p, patch->mapper_port);
      } else {
        uint32_t jump_low;
        uint32_t jump_high;
        uint32_t jump_invalid;
        uint32_t invalid;
        uint32_t after_map;

        emit_byte(image, &p, 0xF5); /* preserve A and flags */
        emit_byte(image, &p, 0xFE);
        emit_byte(image, &p, games[i].bank_offset);
        jump_low = p;
        emit_byte(image, &p, 0x38); /* JR C,invalid */
        emit_byte(image, &p, 0);
        emit_byte(image, &p, 0xFE);
        emit_byte(image, &p, (uint8_t)(games[i].bank_offset + games[i].bank_count));
        jump_high = p;
        emit_byte(image, &p, 0x30); /* JR NC,invalid */
        emit_byte(image, &p, 0);
        emit_byte(image, &p, 0xD6);
        emit_byte(image, &p, games[i].bank_offset);
        emit_byte(image, &p, 0xC6);
        emit_byte(image, &p, (uint8_t)global_bank);
        emit_byte(image, &p, 0xD3);
        emit_byte(image, &p, 0xFE);
        jump_invalid = p;
        emit_byte(image, &p, 0x18); /* JR after_map */
        emit_byte(image, &p, 0);
        invalid = p;
        patch_relative(image, jump_low, invalid);
        patch_relative(image, jump_high, invalid);
        emit_byte(image, &p, 0x3E);
        emit_byte(image, &p, games[i].main_bank);
        emit_byte(image, &p, 0xD3);
        emit_byte(image, &p, 0xFE);
        after_map = p;
        patch_relative(image, jump_invalid, after_map);
        emit_byte(image, &p, 0xF1);
      }
      for (uint32_t k = 0; k < patch->next_length; k++)
        emit_byte(image, &p, patch->next[k]);
      emit_byte(image, &p, 0xC3);
      emit_word(image, &p, (uint16_t)source_address);
      cursor = p;
      if (cursor >= MERGED_VARIABLE_ADDRESS)
        return 1;
      games[i].initial[patch->image_offset] = 0xC3;
      games[i].initial[patch->image_offset + 1] = (uint8_t)(stub_address & 0xFF);
      games[i].initial[patch->image_offset + 2] = (uint8_t)(stub_address >> 8);
    }
  }
  return 0;
}

static int compress_source_images(GAME *games, uint32_t game_count, BANK_PACK *pack) {
  uint32_t i;
  pack->stream_offset = 0;
  for (i = 0; i < game_count; i++) {
    uint32_t j;
    games[i].chunk_count = games[i].initial_size / INITIAL_CHUNK_SIZE;
    games[i].chunks = (CHUNK_REFERENCE *)calloc(games[i].chunk_count, sizeof(*games[i].chunks));
    if (!games[i].chunks)
      return 1;
    for (j = 0; j < games[i].chunk_count; j++) {
      uint8_t *compressed = NULL;
      uint32_t compressed_size = 0;
      uint32_t delta = 0;
      uint16_t source;
      uint32_t bank;
      uint32_t destination = games[i].load_address + j * INITIAL_CHUNK_SIZE;
      uint32_t end = destination + INITIAL_CHUNK_SIZE;

      if (destination >= 0x8000 && end <= 0xC000) {
        /* This part is supplied by the game's raw main-window bank. */
        games[i].chunks[j].bank = 0;
        continue;
      }
      if (!zx0_compress_data(games[i].initial + j * INITIAL_CHUNK_SIZE,
                             INITIAL_CHUNK_SIZE, &compressed, &compressed_size, &delta)) {
        fprintf(stderr, "%s: ZX0 failed for initial chunk %u\n", games[i].name,
                (unsigned)j);
        free(compressed);
        return 1;
      }
      bank = bank_pack_add_stream(pack, compressed, compressed_size, &source);
      free(compressed);
      if (!bank) {
        fprintf(stderr, "%s: could not pack compressed chunk %u (%u bytes)\n",
                games[i].name, (unsigned)j, (unsigned)compressed_size);
        return 1;
      }

      games[i].chunks[j].bank = (uint8_t)bank;
      games[i].chunks[j].source = source;
      if (destination >= 0xC000) {
        /* Decode directly into C000H+; otherwise the scratch buffer would
         * overwrite another already-loaded upper-memory chunk. */
        games[i].chunks[j].destination = (uint16_t)destination;
        games[i].chunks[j].copy_offset = 0xFFFF;
        games[i].chunks[j].copy_length = 0;
      } else if (destination < 0x8000) {
        games[i].chunks[j].destination = (uint16_t)destination;
        games[i].chunks[j].copy_offset = 0;
        games[i].chunks[j].copy_length =
            (uint16_t)((end < 0x8000 ? end : 0x8000) - destination);
      } else {
        /* The lower part is already in the raw main-window bank; relocate
         * only the upper tail into writable C000H+ RAM. */
        games[i].chunks[j].destination = 0xC000;
        games[i].chunks[j].copy_offset = (uint16_t)(0xC000 - destination);
        games[i].chunks[j].copy_length = (uint16_t)(end - 0xC000);
      }
    }
  }
  return 0;
}

static const char *display_game_title(const GAME *game) {
  if (game->meta.full_title[0])
    return game->meta.full_title;
  if (game->meta.title[0])
    return game->meta.title;
  return game->name;
}

static void append_title(char *destination, size_t size, const char *text) {
  size_t used = strlen(destination);
  if (used + 1 < size)
    snprintf(destination + used, size - used, "%s", text);
}

static void make_track_title(const GAME *game, const TRACK_META *track,
                             char *destination, size_t size) {
  char suffix[512];
  destination[0] = '\0';
  snprintf(destination, size, "%s [#%d] - %s", display_game_title(game),
           track->id, track->title);
  suffix[0] = '\0';
  if (game->meta.year[0]) {
    snprintf(suffix, sizeof(suffix), " (%s", game->meta.year);
    append_title(destination, size, suffix);
    if (game->meta.vendor[0]) {
      snprintf(suffix, sizeof(suffix), ", %s", game->meta.vendor);
      append_title(destination, size, suffix);
    }
    append_title(destination, size, ")");
  }
  if (game->meta.chips[0]) {
    snprintf(suffix, sizeof(suffix), " [chips: %s]", game->meta.chips);
    append_title(destination, size, suffix);
  }
  if (game->meta.composers[0]) {
    snprintf(suffix, sizeof(suffix), " [composers: %s]", game->meta.composers);
    append_title(destination, size, suffix);
  }
  if (game->meta.japanese_title[0]) {
    snprintf(suffix, sizeof(suffix), " [JP: %s]", game->meta.japanese_title);
    append_title(destination, size, suffix);
  }
}

static uint32_t collect_tracks(GAME *games, uint32_t game_count,
                               OUTPUT_TRACK *tracks) {
  uint32_t i;
  uint32_t count = 0;
  for (i = 0; i < game_count; i++) {
    uint32_t j;
    for (j = 0; j < games[i].meta.track_count; j++) {
      int id = games[i].meta.tracks_to_play[j];
      TRACK_META *track = &games[i].track_meta[id];
      if (!track->valid)
        continue;
      if (track->seconds < 10) {
        fprintf(stderr, "skipping %s track %d: duration %d seconds (<10)\n",
                games[i].name, id, track->seconds);
        continue;
      }
      if (count >= KSS_MAX_TRACKS) {
        fprintf(stderr, "skipping %s track %d: KSS song limit reached\n",
                games[i].name, id);
        continue;
      }
      tracks[count].game = &games[i];
      tracks[count].source_song = id;
      tracks[count].seconds = track->seconds;
      tracks[count].fade_seconds = track->fade_seconds;
      tracks[count].loop = track->loop;
      make_track_title(&games[i], track, tracks[count].title, sizeof(tracks[count].title));
      count++;
    }
  }
  return count;
}

static uint32_t info_size(const OUTPUT_TRACK *tracks, uint32_t count) {
  uint32_t size = 0x10;
  uint32_t i;
  for (i = 0; i < count; i++)
    size += 10 + (uint32_t)strlen(tracks[i].title) + 1;
  return size;
}

static void write_info(uint8_t *data, const OUTPUT_TRACK *tracks, uint32_t count) {
  uint32_t i;
  uint32_t offset = 0x10;
  memcpy(data, "INFO", 4);
  set_dword(data, 4, info_size(tracks, count) - 0x10);
  set_word(data, 8, (uint16_t)count);
  memset(data + 10, 0, 6);
  for (i = 0; i < count; i++) {
    data[offset++] = (uint8_t)i;
    data[offset++] = (uint8_t)(tracks[i].loop ? 1 : 0);
    set_dword(data, offset, (uint32_t)tracks[i].seconds * 1000u);
    offset += 4;
    set_dword(data, offset, (uint32_t)tracks[i].fade_seconds * 1000u);
    offset += 4;
    strcpy((char *)(data + offset), tracks[i].title);
    offset += (uint32_t)strlen(tracks[i].title) + 1;
  }
}

static int write_merged_metadata(uint8_t *image, GAME *games, uint32_t game_count,
                                 OUTPUT_TRACK *tracks, uint32_t track_count) {
  uint32_t i;
  uint32_t descriptor = MERGED_GAME_DESCRIPTOR_ADDRESS;
  uint32_t chunk_descriptor = MERGED_CHUNK_DESCRIPTOR_ADDRESS;
  uint32_t track_table = MERGED_TRACK_TABLE_ADDRESS;

  for (i = 0; i < game_count; i++) {
    uint32_t j;
    games[i].descriptor_address = (uint16_t)descriptor;
    set_word(image, descriptor - MERGED_LOAD_ADDRESS, (uint16_t)chunk_descriptor);
    image[descriptor - MERGED_LOAD_ADDRESS + 2] = (uint8_t)games[i].chunk_count;
    set_word(image, descriptor - MERGED_LOAD_ADDRESS + 3, games[i].init_address);
    set_word(image, descriptor - MERGED_LOAD_ADDRESS + 5, games[i].play_address);
    image[descriptor - MERGED_LOAD_ADDRESS + 7] = games[i].main_bank;
    for (j = 0; j < games[i].chunk_count; j++) {
      uint32_t offset = chunk_descriptor - MERGED_LOAD_ADDRESS + j * CHUNK_DESCRIPTOR_SIZE;
      image[offset] = games[i].chunks[j].bank;
      set_word(image, offset + 1, games[i].chunks[j].source);
      set_word(image, offset + 3, games[i].chunks[j].destination);
      set_word(image, offset + 5, games[i].chunks[j].copy_offset);
      set_word(image, offset + 7, games[i].chunks[j].copy_length);
    }
    descriptor += GAME_DESCRIPTOR_SIZE;
    chunk_descriptor += games[i].chunk_count * CHUNK_DESCRIPTOR_SIZE;
  }
  if (chunk_descriptor > MERGED_TRACK_TABLE_ADDRESS) {
    fprintf(stderr, "chunk descriptor end 0x%04X, track table 0x%04X\n",
            (unsigned)chunk_descriptor, MERGED_TRACK_TABLE_ADDRESS);
    return 1;
  }

  for (i = 0; i < track_count; i++) {
    uint32_t offset = track_table - MERGED_LOAD_ADDRESS + i * 4;
    image[offset] = (uint8_t)(tracks[i].game - games);
    image[offset + 1] = (uint8_t)tracks[i].source_song;
    set_word(image, offset + 2, tracks[i].game->descriptor_address);
  }
  return track_table + track_count * 4 > 0x10000;
}

static void write_header(uint8_t *data, uint16_t play_address,
                         uint32_t bank_count, uint32_t info_offset,
                         uint32_t track_count) {
  memcpy(data, "KSSX", 4);
  set_word(data, 4, MERGED_LOAD_ADDRESS);
  set_word(data, 6, KSS_MAX_LOAD_SIZE);
  set_word(data, 8, MERGED_INIT_ADDRESS);
  set_word(data, 0x0A, play_address);
  data[0x0C] = KSS_BANK_OFFSET;
  data[0x0D] = (uint8_t)bank_count; /* common archive uses 16K banks */
  data[0x0E] = 0x10;
  data[0x0F] = 0x00; /* MSX PSG/SCC, NTSC */
  set_dword(data, 0x10, info_offset);
  set_dword(data, 0x14, 0);
  set_word(data, 0x18, 0);
  set_word(data, 0x1A, (uint16_t)(track_count - 1));
  /* KSSPLAY treats 0 as the neutral device volume.  0x80 would clip to +48 dB
   * for KSSX archives and turns otherwise normal PSG/SCC output into noise. */
  data[0x1C] = 0;
  data[0x1D] = 0;
  data[0x1E] = 0;
  data[0x1F] = 0;
}

static OUTPUT_KSS *build_output(GAME *games, uint32_t game_count,
                                OUTPUT_TRACK *tracks, uint32_t track_count,
                                BANK_PACK *pack) {
  uint8_t *image = NULL;
  uint32_t play_address = 0;
  uint32_t info_offset;
  uint32_t total_size;
  OUTPUT_KSS *result = NULL;

  image = (uint8_t *)malloc(KSS_MAX_LOAD_SIZE);
  if (!image)
    goto cleanup;
  memset(image, 0xC9, KSS_MAX_LOAD_SIZE);
  /* Match VM_init_memory: MSX RAM from 4000H upward starts cleared. */
  memset(image + 0x4000 - MERGED_LOAD_ADDRESS, 0, 0xC000);
  memcpy(image + MERGED_DECODER_ADDRESS - MERGED_LOAD_ADDRESS,
         zx0_standard_decompressor, ZX0_STANDARD_DECOMPRESSOR_SIZE);
  {
    uint8_t *decoder = image + MERGED_DECODER_ADDRESS - MERGED_LOAD_ADDRESS;
    set_word(decoder, 8, (uint16_t)(MERGED_DECODER_ADDRESS + 0x35));
    set_word(decoder, 16, (uint16_t)(MERGED_DECODER_ADDRESS + 0x35));
    set_word(decoder, 32, (uint16_t)(MERGED_DECODER_ADDRESS + 0x36));
    set_word(decoder, 48, (uint16_t)(MERGED_DECODER_ADDRESS + 0x3D));
  }
  if (build_common_code(image, (uint16_t *)&play_address)) {
    fprintf(stderr, "common dispatcher overlaps decoder\n");
    goto cleanup;
  }
  if (write_merged_metadata(image, games, game_count, tracks, track_count)) {
    fprintf(stderr, "metadata tables overlap the track table\n");
    goto cleanup;
  }
  if (patch_bank_stubs(games, game_count, image)) {
    fprintf(stderr, "bank-switch stubs overlap merged variables\n");
    goto cleanup;
  }
  if (compress_source_images(games, game_count, pack)) {
    fprintf(stderr, "could not compress or pack source images\n");
    goto cleanup;
  }
  if (write_merged_metadata(image, games, game_count, tracks, track_count)) {
    fprintf(stderr, "compressed chunk metadata overlaps the track table\n");
    goto cleanup;
  }

  info_offset = KSS_MAX_LOAD_SIZE + pack->count * KSS_BANK_SIZE;
  total_size = KSS_HEADER_SIZE + info_offset + info_size(tracks, track_count);
  result = (OUTPUT_KSS *)calloc(1, sizeof(*result));
  if (!result)
    goto cleanup;
  result->data = (uint8_t *)malloc(total_size);
  if (!result->data) {
    free(result);
    result = NULL;
    goto cleanup;
  }
  result->size = total_size;
  memset(result->data, 0xC9, total_size);
  write_header(result->data, (uint16_t)play_address, pack->count, info_offset, track_count);
  memcpy(result->data + KSS_HEADER_SIZE, image, KSS_MAX_LOAD_SIZE);
  memcpy(result->data + KSS_HEADER_SIZE + KSS_MAX_LOAD_SIZE,
         pack->data, pack->count * KSS_BANK_SIZE);
  write_info(result->data + KSS_HEADER_SIZE + info_offset, tracks, track_count);

cleanup:
  free(image);
  return result;
}

static void free_games(GAME *games, uint32_t count) {
  uint32_t i;
  for (i = 0; i < count; i++) {
    free(games[i].file_data);
    free(games[i].initial);
    free(games[i].chunks);
    free(games[i].patches);
  }
  free(games);
}

static int scan_games(const char *directory, GAME **games_out, uint32_t *count_out) {
  DIR *dir;
  struct dirent *entry;
  GAME *games = NULL;
  uint32_t count = 0;

  dir = opendir(directory);
  if (!dir) {
    fprintf(stderr, "%s: %s\n", directory, strerror(errno));
    return 1;
  }
  while ((entry = readdir(dir)) != NULL) {
    GAME game;
    const char *dot;
    size_t name_length;
    char gameinfo_path[1024];
    char trackinfo_path[1024];

    if (entry->d_name[0] == '.' || !is_extension(entry->d_name, ".kss"))
      continue;
    memset(&game, 0, sizeof(game));
    dot = strrchr(entry->d_name, '.');
    name_length = dot ? (size_t)(dot - entry->d_name) : strlen(entry->d_name);
    if (name_length >= sizeof(game.name))
      name_length = sizeof(game.name) - 1;
    memcpy(game.name, entry->d_name, name_length);
    game.name[name_length] = '\0';
    snprintf(game.path, sizeof(game.path), "%s/%s", directory, entry->d_name);
    make_path(gameinfo_path, sizeof(gameinfo_path), directory, game.name, ".gameinfo");
    make_path(trackinfo_path, sizeof(trackinfo_path), directory, game.name, ".trackinfo");
    if (read_gameinfo(gameinfo_path, &game.meta)) {
      fprintf(stderr, "skipping %s: missing or invalid tracks_to_play metadata\n", game.name);
      continue;
    }
    if (read_trackinfo(trackinfo_path, game.track_meta)) {
      fprintf(stderr, "skipping %s: missing trackinfo metadata\n", game.name);
      continue;
    }
    if (load_source_kss(&game)) {
      free(game.file_data);
      free(game.initial);
      continue;
    }
    {
      GAME *new_games = (GAME *)realloc(games, (count + 1) * sizeof(*games));
      if (!new_games) {
        free(game.file_data);
        free(game.initial);
        free_games(games, count);
        closedir(dir);
        return 1;
      }
      games = new_games;
      games[count++] = game;
    }
  }
  closedir(dir);
  if (count == 0) {
    fprintf(stderr, "%s: no usable KSS/gameinfo/trackinfo sets found\n", directory);
    free(games);
    return 1;
  }
  qsort(games, count, sizeof(*games), compare_games);
  *games_out = games;
  *count_out = count;
  return 0;
}

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [-o output.kss] [vigamup-directory]\n", program);
  fprintf(stderr, "Default directory: vigamup\n");
  fprintf(stderr, "Tracks shorter than 10 seconds are skipped; source songs follow gameinfo tracks_to_play.\n");
}

int main(int argc, char **argv) {
  const char *output = "vigamup.kss";
  const char *directory = "vigamup";
  GAME *games = NULL;
  OUTPUT_TRACK tracks[KSS_MAX_TRACKS];
  uint32_t game_count = 0;
  uint32_t track_count;
  uint32_t i;
  BANK_PACK pack = {0};
  OUTPUT_KSS *kss = NULL;
  int first_argument = 1;
  int result = 1;

  if (argc > 1 && strcmp(argv[1], "-h") == 0) {
    usage(argv[0]);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "-o") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 1;
    }
    output = argv[2];
    first_argument = 3;
  } else if (argc > 1 && strncmp(argv[1], "-o", 2) == 0) {
    output = argv[1] + 2;
    first_argument = 2;
  }
  if (first_argument < argc)
    directory = argv[first_argument];
  if (first_argument + 1 < argc) {
    usage(argv[0]);
    return 1;
  }

  if (scan_games(directory, &games, &game_count))
    goto cleanup;
  track_count = collect_tracks(games, game_count, tracks);
  if (track_count == 0 || track_count > KSS_MAX_TRACKS) {
    fprintf(stderr, "no tracks fit the KSS song range\n");
    goto cleanup;
  }
  fprintf(stderr, "selected %u tracks from %u games\n", (unsigned)track_count,
          (unsigned)game_count);
  if (prepare_sources(games, game_count, &pack))
    goto cleanup;
  fprintf(stderr, "reserved %u raw 16K source banks\n", (unsigned)pack.raw_count);
  for (i = 0; i < track_count; i++)
    fprintf(stderr, "adding track %u: %s\n", (unsigned)i, tracks[i].title);
  fprintf(stderr, "compressing %u game images with ZX0...\n", (unsigned)game_count);
  kss = build_output(games, game_count, tracks, track_count, &pack);
  if (!kss) {
    fprintf(stderr, "failed to build merged VGM Up KSS\n");
    goto cleanup;
  }
  if (write_file(output, kss->data, kss->size))
    goto cleanup;
  printf("wrote %s (%u tracks, %u banks, %u bytes)\n", output,
         (unsigned)track_count, (unsigned)pack.count, (unsigned)kss->size);
  result = 0;

cleanup:
  if (kss) {
    free(kss->data);
    free(kss);
  }
  free(pack.data);
  free_games(games, game_count);
  return result;
}
