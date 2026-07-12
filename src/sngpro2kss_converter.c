#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pro_tracker_converter.h"
#include "pro_tracker_player.h"
#include "sng2kss_converter.h"
#include "sngpro2kss_converter.h"
#include "zx_compressor.h"
#include "zx_decompressors.h"

#define KSS_HEADER_SIZE 0x20
#define KSS_MAX_LOAD_SIZE 0xFE00
#define KSS_BANK_SIZE 0x2000
#define KSS_BANK_OFFSET 1
#define KSS_MAX_BANKS 127
#define MAX_TRACKS 256

#define MERGED_LOAD_ADDRESS 0x0200
#define MERGED_COMMON_ADDRESS 0x3000
#define MERGED_SNG_DRIVER_ADDRESS 0x4000
#define MERGED_SNG_PLAY_ADDRESS (MERGED_SNG_DRIVER_ADDRESS + 0x30)
#define MERGED_SNG_TABLE_ADDRESS (MERGED_SNG_DRIVER_ADDRESS + 143)
#define MERGED_PRO_PLAYER_ADDRESS 0xCA00
#define MERGED_DECODER_ADDRESS 0xD500
#define MERGED_METADATA_ADDRESS 0xE000
#define MERGED_SNG_BUFFER_ADDRESS 0x6000
#define PRO_TRACKER_SLOT_CALL_OFFSET 0x762

#define MERGED_MAP_POINTER_ADDRESS 0x3FF0
#define MERGED_CURRENT_BANK_ADDRESS 0x3FF2
#define MERGED_ENGINE_ADDRESS 0x3FF3
#define SNG_PTR_ADDRESS 0x3FF8
#define SNG_START_ADDRESS 0x3FFA
#define SNG_PTR_BANK_ADDRESS 0x3FFC
#define SNG_START_BANK_ADDRESS 0x3FFD
#define SNG_WAIT_ADDRESS 0x3FFE

#define SNG_DRIVER_SIZE 143
#define SNG_TABLE_SIZE 3
#define SNG_MAP_ENTRY_SIZE 4
#define SNG_DESCRIPTOR_SIZE 5
#define PRO_DESCRIPTOR_SIZE 3

typedef struct {
  uint8_t bank;
  uint16_t source;
} PACK_REFERENCE;

typedef struct {
  PACK_REFERENCE *entries;
  uint32_t count;
  uint8_t start_bank;
  uint16_t start_pointer;
  uint16_t map_address;
  uint16_t descriptor_address;
} SNG_BUILD;

typedef struct {
  PACK_REFERENCE reference;
  uint16_t descriptor_address;
} PRO_BUILD;

typedef struct {
  SNGPRO_TRACK_TYPE type;
  const char *title;
  union {
    SNG_BUILD sng;
    PRO_BUILD pro;
  } content;
} TRACK_BUILD;

typedef struct {
  uint8_t *data;
  uint32_t bank_count;
  uint32_t offset;
} BANK_PACK;

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

static int emit_byte(uint8_t *image, uint32_t *address, uint8_t value) {
  image[*address - MERGED_LOAD_ADDRESS] = value;
  (*address)++;
  return 0;
}

static int emit_word(uint8_t *image, uint32_t *address, uint16_t value) {
  emit_byte(image, address, (uint8_t)(value & 0xFF));
  emit_byte(image, address, (uint8_t)(value >> 8));
  return 0;
}

static void patch_word(uint8_t *image, uint32_t address, uint16_t value) {
  set_word(image, address - MERGED_LOAD_ADDRESS, value);
}

static int bank_pack_add(BANK_PACK *pack, const uint8_t *data, uint32_t size,
                         PACK_REFERENCE *reference) {
  uint32_t new_count;
  uint8_t *new_data;

  if (!pack || !data || !reference || size == 0 || size > KSS_BANK_SIZE)
    return 1;

  if (pack->bank_count == 0 || pack->offset + size > KSS_BANK_SIZE) {
    new_count = pack->bank_count + 1;
    if (new_count > KSS_MAX_BANKS)
      return 1;
    new_data = (uint8_t *)realloc(pack->data, new_count * KSS_BANK_SIZE);
    if (!new_data)
      return 1;
    pack->data = new_data;
    memset(pack->data + pack->bank_count * KSS_BANK_SIZE, 0xC9, KSS_BANK_SIZE);
    pack->bank_count = new_count;
    pack->offset = 0;
  }

  reference->bank = (uint8_t)(KSS_BANK_OFFSET + pack->bank_count - 1);
  reference->source = (uint16_t)(0x8000 + pack->offset);
  memcpy(pack->data + (pack->bank_count - 1) * KSS_BANK_SIZE + pack->offset, data, size);
  pack->offset += size;
  return 0;
}

static int compress_and_pack(BANK_PACK *pack, const uint8_t *data, uint32_t size,
                             PACK_REFERENCE *reference) {
  uint8_t *compressed = NULL;
  uint32_t compressed_size = 0;
  uint32_t delta = 0;
  int result;

  if (!zx0_compress_data(data, size, &compressed, &compressed_size, &delta) ||
      compressed_size > KSS_BANK_SIZE) {
    free(compressed);
    return 1;
  }
  result = bank_pack_add(pack, compressed, compressed_size, reference);
  free(compressed);
  return result;
}

static void patch_sng_buffer_jumps(uint8_t *data, uint32_t size) {
  uint32_t i;

  /* The original SNG driver jumps to 9900H to skip the SCC mapper gap. The merged driver
   * reads from a decompressed RAM window at 6000H, so the equivalent address is 7900H. */
  for (i = 0; i + 2 < size; ++i) {
    if (data[i] == 0xFB && data[i + 1] == 0x00 && data[i + 2] == 0x99)
      data[i + 2] = 0x79;
  }
}

static int build_sng_track(const SNGPRO_INPUT *input, BANK_PACK *pack,
                           SNG_BUILD *build, uint8_t *driver, int *driver_ready) {
  const uint8_t *data[] = {input->data};
  const uint32_t sizes[] = {input->size};
  const char *titles[] = {input->title};
  SNG_KSS *source;
  uint32_t load_address;
  uint32_t load_size;
  uint32_t bank_count;
  uint32_t bank_offset;
  uint32_t table_offset;
  uint32_t i;
  uint8_t *chunk;

  source = sngs_to_kss(data, sizes, titles, 1);
  if (!source)
    return 1;

  load_address = get_word(source->data, 4);
  load_size = get_word(source->data, 6);
  bank_count = source->data[0x0D] & 0x7F;
  bank_offset = source->data[0x0C];
  table_offset = MERGED_SNG_TABLE_ADDRESS - load_address;
  if (load_address != MERGED_LOAD_ADDRESS || bank_count == 0 || bank_offset != KSS_BANK_OFFSET ||
      table_offset + SNG_TABLE_SIZE > load_size ||
      0x20 + load_size + bank_count * KSS_BANK_SIZE > source->size) {
    sng_kss_delete(source);
    return 1;
  }

  if (!*driver_ready) {
    uint32_t driver_offset = MERGED_SNG_DRIVER_ADDRESS - load_address;
    if (driver_offset + SNG_DRIVER_SIZE > load_size) {
      sng_kss_delete(source);
      return 1;
    }
    memcpy(driver, source->data + KSS_HEADER_SIZE + driver_offset, SNG_DRIVER_SIZE);
    *driver_ready = 1;
  }

  build->count = bank_count + KSS_BANK_OFFSET;
  build->entries = (PACK_REFERENCE *)calloc(build->count, sizeof(*build->entries));
  if (!build->entries) {
    sng_kss_delete(source);
    return 1;
  }

  build->start_bank = source->data[KSS_HEADER_SIZE + table_offset];
  {
    uint16_t start = get_word(source->data, KSS_HEADER_SIZE + table_offset + 1);
    if (build->start_bank < KSS_BANK_OFFSET || start < 0x8000 || start >= 0xA000) {
      free(build->entries);
      build->entries = NULL;
      sng_kss_delete(source);
      return 1;
    }
    build->start_pointer = (uint16_t)(MERGED_SNG_BUFFER_ADDRESS + (start & 0x1FFF));
  }

  chunk = (uint8_t *)malloc(KSS_BANK_SIZE);
  if (!chunk)
    goto fail;
  for (i = 0; i < bank_count; ++i) {
    memcpy(chunk, source->data + KSS_HEADER_SIZE + load_size + i * KSS_BANK_SIZE, KSS_BANK_SIZE);
    patch_sng_buffer_jumps(chunk, KSS_BANK_SIZE);
    if (compress_and_pack(pack, chunk, KSS_BANK_SIZE,
                          &build->entries[KSS_BANK_OFFSET + i]))
      goto fail;
  }
  free(chunk);
  sng_kss_delete(source);
  return 0;

fail:
  free(chunk);
  free(build->entries);
  build->entries = NULL;
  sng_kss_delete(source);
  return 1;
}

static int build_pro_track(const SNGPRO_INPUT *input, BANK_PACK *pack, PRO_BUILD *build) {
  return compress_and_pack(pack, input->data, input->size, &build->reference);
}

static void patch_driver(uint8_t *driver, uint16_t loader_address) {
  uint32_t i;
  uint32_t bank_calls = 0;
  uint32_t buffer_loads = 0;

  for (i = 0; i + 2 < SNG_DRIVER_SIZE; ++i) {
    if (driver[i] == 0x32 && driver[i + 1] == 0x00 && driver[i + 2] == 0x90) {
      driver[i] = 0xCD;
      set_word(driver, i + 1, loader_address);
      bank_calls++;
    }
    if (driver[i] == 0x21 && driver[i + 1] == 0x00 && driver[i + 2] == 0x80) {
      driver[i + 2] = 0x60;
      buffer_loads++;
    }
  }

  (void)bank_calls;
  (void)buffer_loads;
}

static uint32_t build_common_code(uint8_t *image, uint16_t dispatch_address,
                                  uint16_t decoder_address, uint16_t *play_address) {
  uint32_t pc = MERGED_COMMON_ADDRESS;
  uint32_t pro_jump_operand;
  uint32_t prepare_call_operand;
  uint32_t prepare_address;
  uint32_t loader_address;
  uint32_t play;

  /* Common init: dispatch on the KSS song number in A. Each table record is
   * [engine, reserved, descriptor-address]. */
  emit_byte(image, &pc, 0x6F); /* LD L,A */
  emit_byte(image, &pc, 0x26); /* LD H,0 */
  emit_byte(image, &pc, 0x00);
  emit_byte(image, &pc, 0x29); /* ADD HL,HL */
  emit_byte(image, &pc, 0x29);
  emit_byte(image, &pc, 0x11); /* LD DE,dispatch table */
  emit_word(image, &pc, dispatch_address);
  emit_byte(image, &pc, 0x19); /* ADD HL,DE */
  emit_byte(image, &pc, 0x7E); /* LD A,(HL) */
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x5E); /* LD E,(HL) */
  emit_byte(image, &pc, 0x23);
  emit_byte(image, &pc, 0x56); /* LD D,(HL) */
  emit_byte(image, &pc, 0xB7); /* OR A */
  emit_byte(image, &pc, 0xC2); /* JP NZ,pro */
  pro_jump_operand = pc;
  emit_word(image, &pc, 0);

  emit_byte(image, &pc, 0xAF); /* XOR A */
  emit_byte(image, &pc, 0x32); /* LD (engine),A */
  emit_word(image, &pc, MERGED_ENGINE_ADDRESS);
  emit_byte(image, &pc, 0xCD); /* CALL prepare SNG */
  prepare_call_operand = pc;
  emit_word(image, &pc, 0);
  emit_byte(image, &pc, 0xAF); /* LD A,0 for the single-entry SNG driver */
  emit_byte(image, &pc, 0xCD);
  emit_word(image, &pc, MERGED_SNG_DRIVER_ADDRESS);
  emit_byte(image, &pc, 0xC9); /* RET */

  patch_word(image, pro_jump_operand, (uint16_t)pc);
  emit_byte(image, &pc, 0x3E); /* LD A,1 */
  emit_byte(image, &pc, 0x01);
  emit_byte(image, &pc, 0x32); /* LD (engine),A */
  emit_word(image, &pc, MERGED_ENGINE_ADDRESS);
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) - physical bank */
  emit_byte(image, &pc, 0x32); /* LD (9000),A */
  emit_word(image, &pc, 0x9000);
  emit_byte(image, &pc, 0x13); /* INC DE */
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x6F); /* LD L,A */
  emit_byte(image, &pc, 0x13);
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x67); /* LD H,A */
  emit_byte(image, &pc, 0x11); /* LD DE,0000H */
  emit_word(image, &pc, 0x0000);
  emit_byte(image, &pc, 0xCD); /* CALL decoder */
  emit_word(image, &pc, decoder_address);
  emit_byte(image, &pc, 0xAF); /* player init sees song zero */
  emit_byte(image, &pc, 0xC3); /* JP Pro Tracker init */
  emit_word(image, &pc, MERGED_PRO_PLAYER_ADDRESS);

  prepare_address = pc;
  patch_word(image, prepare_call_operand, (uint16_t)prepare_address);
  /* DE points at [map pointer, start bank, start pointer]. */
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x6F); /* LD L,A */
  emit_byte(image, &pc, 0x13);
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x67); /* LD H,A */
  emit_byte(image, &pc, 0x13);
  emit_byte(image, &pc, 0x22); /* LD (map pointer),HL */
  emit_word(image, &pc, MERGED_MAP_POINTER_ADDRESS);
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x13);
  emit_byte(image, &pc, 0x32); /* LD (SNG table bank),A */
  emit_word(image, &pc, MERGED_SNG_TABLE_ADDRESS);
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x6F); /* LD L,A */
  emit_byte(image, &pc, 0x13);
  emit_byte(image, &pc, 0x1A); /* LD A,(DE) */
  emit_byte(image, &pc, 0x67); /* LD H,A */
  emit_byte(image, &pc, 0x22); /* LD (SNG table pointer),HL */
  emit_word(image, &pc, MERGED_SNG_TABLE_ADDRESS + 1);
  emit_byte(image, &pc, 0xAF); /* current logical bank = 0 */
  emit_byte(image, &pc, 0x32);
  emit_word(image, &pc, MERGED_CURRENT_BANK_ADDRESS);
  emit_byte(image, &pc, 0xC9);

  loader_address = pc;
  /* SNG bank loader. A is a logical per-track bank number. The map entry is
   * [physical-bank, compressed-source-address, flags]. */
  emit_byte(image, &pc, 0xE5); /* PUSH HL: SNG init still needs its table pointer */
  emit_byte(image, &pc, 0x47); /* LD B,A */
  emit_byte(image, &pc, 0x3A); /* LD A,(current bank) */
  emit_word(image, &pc, MERGED_CURRENT_BANK_ADDRESS);
  emit_byte(image, &pc, 0xB8); /* CP B */
  emit_byte(image, &pc, 0xC2); /* JP NZ,load */
  {
    uint32_t load_jump_operand = pc;
    uint32_t load_body;
    emit_word(image, &pc, 0);
    emit_byte(image, &pc, 0xE1); /* POP HL */
    emit_byte(image, &pc, 0xC9); /* RET */
    load_body = pc;
    patch_word(image, load_jump_operand, (uint16_t)load_body);
    emit_byte(image, &pc, 0x78); /* LD A,B */
  
    emit_byte(image, &pc, 0x32);
    emit_word(image, &pc, MERGED_CURRENT_BANK_ADDRESS);
    emit_byte(image, &pc, 0x6F); /* LD L,A */
    emit_byte(image, &pc, 0x26); /* LD H,0 */
    emit_byte(image, &pc, 0x00);
    emit_byte(image, &pc, 0x29);
    emit_byte(image, &pc, 0x29);
    emit_byte(image, &pc, 0xED); /* LD DE,(map pointer) */
    emit_byte(image, &pc, 0x5B);
    emit_word(image, &pc, MERGED_MAP_POINTER_ADDRESS);
    emit_byte(image, &pc, 0x19); /* ADD HL,DE */
    emit_byte(image, &pc, 0x7E); /* physical bank */
    emit_byte(image, &pc, 0x32); /* LD (9000),A */
    emit_word(image, &pc, 0x9000);
    emit_byte(image, &pc, 0x23);
    emit_byte(image, &pc, 0x5E); /* source low */
    emit_byte(image, &pc, 0x23);
    emit_byte(image, &pc, 0x56); /* source high */
    emit_byte(image, &pc, 0xEB); /* HL = source */
    emit_byte(image, &pc, 0x11); /* DE = buffer */
    emit_word(image, &pc, MERGED_SNG_BUFFER_ADDRESS);
    emit_byte(image, &pc, 0xCD);
    emit_word(image, &pc, decoder_address);
    emit_byte(image, &pc, 0xE1); /* POP HL */
    emit_byte(image, &pc, 0xC9);
  }

  play = pc;
  emit_byte(image, &pc, 0x3A); /* LD A,(engine) */
  emit_word(image, &pc, MERGED_ENGINE_ADDRESS);
  emit_byte(image, &pc, 0xB7);
  emit_byte(image, &pc, 0xC2); /* JP NZ,Pro play */
  emit_word(image, &pc, PRO_TRACKER_PLAYER_PLAY_ADDRESS);
  emit_byte(image, &pc, 0xC3); /* JP SNG play */
  emit_word(image, &pc, MERGED_SNG_PLAY_ADDRESS);

  patch_driver(image + MERGED_SNG_DRIVER_ADDRESS - MERGED_LOAD_ADDRESS, (uint16_t)loader_address);
  *play_address = (uint16_t)play;
  return pc;
}

static uint32_t info_size(const SNGPRO_INPUT *tracks, uint32_t count) {
  uint32_t i;
  uint32_t size = 0x10;
  for (i = 0; i < count; ++i) {
    const char *title = tracks[i].title ? tracks[i].title : "-";
    size += 10 + (uint32_t)strlen(title) + 1;
  }
  return size;
}

static void write_info(uint8_t *data, const SNGPRO_INPUT *tracks, uint32_t count) {
  uint32_t i;
  uint32_t offset = 0x10;
  memcpy(data, "INFO", 4);
  set_dword(data, 4, info_size(tracks, count) - 0x10);
  set_word(data, 8, (uint16_t)count);
  memset(data + 10, 0, 6);
  for (i = 0; i < count; ++i) {
    const char *title = tracks[i].title ? tracks[i].title : "-";
    data[offset++] = (uint8_t)i;
    data[offset++] = 0;
    set_dword(data, offset, 0);
    offset += 4;
    set_dword(data, offset, 0);
    offset += 4;
    strcpy((char *)(data + offset), title);
    offset += (uint32_t)strlen(title) + 1;
  }
}

static void write_header(uint8_t *data, uint16_t load_size, uint32_t bank_count,
                         uint32_t info_offset, uint32_t track_count) {
  memcpy(data, "KSSX", 4);
  set_word(data, 4, MERGED_LOAD_ADDRESS);
  set_word(data, 6, load_size);
  set_word(data, 8, MERGED_COMMON_ADDRESS);
  set_word(data, 0x0A, 0x0000); /* patched by caller after common play is built */
  data[0x0C] = KSS_BANK_OFFSET;
  data[0x0D] = (uint8_t)(0x80 | bank_count);
  data[0x0E] = 0x10;
  data[0x0F] = 0x41; /* PAL + YM2413; PSG/SCC remain available to the SNG engine */
  set_dword(data, 0x10, info_offset);
  set_dword(data, 0x14, 0);
  set_word(data, 0x18, 0);
  set_word(data, 0x1A, (uint16_t)(track_count - 1));
  data[0x1C] = 0;
  data[0x1D] = 0;
  data[0x1E] = 0x20;
  data[0x1F] = 0;
}

SNGPRO_KSS *sngpro_tracks_to_kss(const SNGPRO_INPUT *tracks, uint32_t count) {
  BANK_PACK pack = {0};
  TRACK_BUILD *built = NULL;
  uint8_t driver[SNG_DRIVER_SIZE];
  int driver_ready = 0;
  uint32_t sng_count = 0;
  uint32_t pro_count = 0;
  uint32_t i;
  uint32_t cursor;
  uint32_t dispatch_address;
  uint32_t sng_descriptor_cursor;
  uint32_t map_cursor;
  uint32_t map_start_cursor;
  uint32_t pro_descriptor_cursor;
  uint32_t pro_descriptor_start_cursor;
  uint32_t metadata_end;
  uint32_t load_size;
  uint8_t *image = NULL;
  uint32_t play_address;
  uint32_t info_offset;
  uint32_t total_size;
  SNGPRO_KSS *result = NULL;

  if (!tracks || count == 0 || count > MAX_TRACKS)
    return NULL;
  for (i = 0; i < count; ++i) {
    if (!tracks[i].data || (tracks[i].type != SNGPRO_TRACK_SNG && tracks[i].type != SNGPRO_TRACK_PRO))
      return NULL;
    if (tracks[i].type == SNGPRO_TRACK_SNG)
      sng_count++;
    else
      pro_count++;
  }
  if (sng_count == 0 || pro_count == 0)
    return NULL;

  built = (TRACK_BUILD *)calloc(count, sizeof(*built));
  if (!built)
    goto cleanup;

  for (i = 0; i < count; ++i) {
    built[i].type = tracks[i].type;
    built[i].title = tracks[i].title;
    if (tracks[i].type == SNGPRO_TRACK_SNG) {
      if (build_sng_track(&tracks[i], &pack, &built[i].content.sng, driver, &driver_ready))
        goto cleanup;
    } else if (build_pro_track(&tracks[i], &pack, &built[i].content.pro)) {
      goto cleanup;
    }
  }
  if (!driver_ready || pack.bank_count == 0 || pack.bank_count > KSS_MAX_BANKS)
    goto cleanup;

  dispatch_address = MERGED_METADATA_ADDRESS;
  cursor = dispatch_address + MAX_TRACKS * 4;
  sng_descriptor_cursor = cursor;
  cursor += sng_count * SNG_DESCRIPTOR_SIZE;
  map_cursor = cursor;
  map_start_cursor = map_cursor;
  for (i = 0; i < count; ++i) {
    if (built[i].type == SNGPRO_TRACK_SNG) {
      built[i].content.sng.map_address = (uint16_t)map_cursor;
      map_cursor += built[i].content.sng.count * SNG_MAP_ENTRY_SIZE;
    }
  }
  pro_descriptor_cursor = map_cursor;
  pro_descriptor_start_cursor = pro_descriptor_cursor;
  cursor += 0;
  for (i = 0; i < count; ++i) {
    if (built[i].type == SNGPRO_TRACK_PRO) {
      built[i].content.pro.descriptor_address = (uint16_t)pro_descriptor_cursor;
      pro_descriptor_cursor += PRO_DESCRIPTOR_SIZE;
    }
  }
  metadata_end = pro_descriptor_cursor;
  if (metadata_end > 0x10000 || metadata_end <= MERGED_METADATA_ADDRESS)
    goto cleanup;

  load_size = metadata_end - MERGED_LOAD_ADDRESS;
  if (load_size > KSS_MAX_LOAD_SIZE || MERGED_LOAD_ADDRESS + load_size > 0x10000)
    goto cleanup;
  image = (uint8_t *)malloc(load_size);
  if (!image)
    goto cleanup;
  memset(image, 0xC9, load_size);

  /* Put the shared engines and decoder into the one contiguous KSS load image. */
  memcpy(image + MERGED_PRO_PLAYER_ADDRESS - MERGED_LOAD_ADDRESS,
         pro_tracker_player, PRO_TRACKER_PLAYER_SIZE);
  memset(image + MERGED_PRO_PLAYER_ADDRESS - MERGED_LOAD_ADDRESS + PRO_TRACKER_SLOT_CALL_OFFSET, 0, 3);
  memcpy(image + MERGED_DECODER_ADDRESS - MERGED_LOAD_ADDRESS,
         zx0_standard_decompressor, ZX0_STANDARD_DECOMPRESSOR_SIZE);
  {
    uint8_t *decoder = image + MERGED_DECODER_ADDRESS - MERGED_LOAD_ADDRESS;
    set_word(decoder, 8, (uint16_t)(MERGED_DECODER_ADDRESS + 0x35));
    set_word(decoder, 16, (uint16_t)(MERGED_DECODER_ADDRESS + 0x35));
    set_word(decoder, 32, (uint16_t)(MERGED_DECODER_ADDRESS + 0x36));
    set_word(decoder, 48, (uint16_t)(MERGED_DECODER_ADDRESS + 0x3D));
  }
  memcpy(image + MERGED_SNG_DRIVER_ADDRESS - MERGED_LOAD_ADDRESS, driver, SNG_DRIVER_SIZE);
  play_address = 0;
  if (build_common_code(image, (uint16_t)dispatch_address, MERGED_DECODER_ADDRESS,
                        (uint16_t *)&play_address) > 0x4000)
    goto cleanup;
  set_word(image, MERGED_SNG_TABLE_ADDRESS - MERGED_LOAD_ADDRESS, 0);
  set_word(image, MERGED_SNG_TABLE_ADDRESS - MERGED_LOAD_ADDRESS + 1, MERGED_SNG_BUFFER_ADDRESS);

  /* Dispatch table and per-engine metadata. */
  {
    uint32_t sng_index = 0;
    uint32_t pro_index = 0;
    uint32_t map_write = map_start_cursor;
    for (i = 0; i < count; ++i) {
      uint32_t dispatch_offset = dispatch_address - MERGED_LOAD_ADDRESS + i * 4;
      uint32_t descriptor;
      if (built[i].type == SNGPRO_TRACK_SNG) {
        descriptor = sng_descriptor_cursor + sng_index * SNG_DESCRIPTOR_SIZE;
        built[i].content.sng.descriptor_address = (uint16_t)descriptor;
        set_word(image, descriptor - MERGED_LOAD_ADDRESS, (uint16_t)map_write);
        image[descriptor - MERGED_LOAD_ADDRESS + 2] = built[i].content.sng.start_bank;
        set_word(image, descriptor - MERGED_LOAD_ADDRESS + 3, built[i].content.sng.start_pointer);
        for (uint32_t j = 0; j < built[i].content.sng.count; ++j) {
          uint32_t off = map_write - MERGED_LOAD_ADDRESS + j * SNG_MAP_ENTRY_SIZE;
          image[off] = built[i].content.sng.entries[j].bank;
          set_word(image, off + 1, built[i].content.sng.entries[j].source);
          image[off + 3] = 0;
        }
        map_write += built[i].content.sng.count * SNG_MAP_ENTRY_SIZE;
        sng_index++;
      } else {
        descriptor = pro_descriptor_start_cursor + pro_index * PRO_DESCRIPTOR_SIZE;
        built[i].content.pro.descriptor_address = (uint16_t)descriptor;
        image[descriptor - MERGED_LOAD_ADDRESS] = built[i].content.pro.reference.bank;
        set_word(image, descriptor - MERGED_LOAD_ADDRESS + 1, built[i].content.pro.reference.source);
        pro_index++;
      }
      image[dispatch_offset] = (uint8_t)built[i].type;
      image[dispatch_offset + 1] = 0;
      set_word(image, dispatch_offset + 2, (uint16_t)descriptor);
    }
  }

  info_offset = load_size + pack.bank_count * KSS_BANK_SIZE;
  total_size = KSS_HEADER_SIZE + info_offset + info_size(tracks, count);
  result = (SNGPRO_KSS *)malloc(sizeof(*result));
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
  write_header(result->data, (uint16_t)load_size, pack.bank_count, info_offset, count);
  set_word(result->data, 0x0A, (uint16_t)play_address);
  memcpy(result->data + KSS_HEADER_SIZE, image, load_size);
  memcpy(result->data + KSS_HEADER_SIZE + load_size, pack.data, pack.bank_count * KSS_BANK_SIZE);
  write_info(result->data + KSS_HEADER_SIZE + info_offset, tracks, count);

cleanup:
  if (built) {
    for (i = 0; i < count; ++i)
      if (built[i].type == SNGPRO_TRACK_SNG)
        free(built[i].content.sng.entries);
  }
  free(built);
  free(pack.data);
  free(image);
  return result;
}

void sngpro_kss_delete(SNGPRO_KSS *kss) {
  if (kss) {
    free(kss->data);
    free(kss);
  }
}
