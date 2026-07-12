#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pro_tracker_converter.h"
#include "pro_tracker_player.h"
#include "zx_compressor.h"
#include "zx_decompressors.h"

#define KSS_HEADER_SIZE 0x20
#define KSS_MAX_LOAD_SIZE 0xFE00
#define KSS_BANK_SIZE 0x4000
#define KSS_MAX_BANK_COUNT 127
#define PRO_TRACKER_DATA_ADDRESS 0x0000
#define PRO_TRACKER_MIN_SIZE 0x146
#define PRO_TRACKER_SLOT_CALL_OFFSET 0x762
#define PRO_TRACKER_MUSIC_VOLUME 0x20 /* KSS volume scale: 0x20 is 4x */
#define PRO_TRACKER_IMAGE_SIZE (PRO_TRACKER_PLAYER_LOAD_ADDRESS + PRO_TRACKER_PLAYER_SIZE)
#define PRO_TRACKER_COMPRESSED_LOAD_ADDRESS 0xD500
#define PRO_TRACKER_BOOTSTRAP_SIZE 13

typedef struct {
  uint8_t bank;
  uint16_t source;
} PRO_TRACKER_BANK_REF;

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

int pro_tracker_is_valid(const uint8_t *data, uint32_t size) {
  return data && size >= PRO_TRACKER_MIN_SIZE && memcmp(data, "PT10", 4) == 0;
}

static void write_kss_header(uint8_t *data, uint16_t load_address, uint16_t load_size,
                             uint16_t init_address, uint32_t bank_count, uint32_t info_offset,
                             uint32_t track_count) {
  memcpy(data, "KSSX", 4);
  set_word(data, 0x04, load_address);
  set_word(data, 0x06, load_size);
  set_word(data, 0x08, init_address);
  set_word(data, 0x0A, PRO_TRACKER_PLAYER_PLAY_ADDRESS);
  data[0x0C] = 0;
  data[0x0D] = (uint8_t)bank_count; /* 16K banks; bit 7 remains clear */
  data[0x0E] = 0x10;
  data[0x0F] = 0x41; /* PAL MSX-MUSIC / YM2413 */
  set_dword(data, 0x10, info_offset);
  set_dword(data, 0x14, 0);
  set_word(data, 0x18, 0);
  set_word(data, 0x1A, (uint16_t)(track_count - 1));
  data[0x1C] = 0;
  data[0x1D] = 0;
  data[0x1E] = PRO_TRACKER_MUSIC_VOLUME;
  data[0x1F] = 0;
}

static uint32_t info_size(const PRO_TRACKER_INPUT *tracks, uint32_t count) {
  uint32_t size = 0x10;
  uint32_t i;

  for (i = 0; i < count; ++i) {
    const char *title = tracks[i].title ? tracks[i].title : "-";
    size += 10 + (uint32_t)strlen(title) + 1;
  }
  return size;
}

static void write_info(uint8_t *data, const PRO_TRACKER_INPUT *tracks, uint32_t count) {
  uint32_t offset = 0x10;
  uint32_t i;

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

static PRO_KSS *make_kss(const uint8_t *main_data, uint32_t load_size,
                         const uint8_t *bank_data, uint32_t bank_count,
                         const PRO_TRACKER_INPUT *tracks, uint32_t track_count) {
  uint32_t bank_size = bank_count * KSS_BANK_SIZE;
  uint32_t metadata_size = info_size(tracks, track_count);
  uint32_t info_offset = load_size + bank_size;
  uint32_t total_size = KSS_HEADER_SIZE + info_offset + metadata_size;
  PRO_KSS *kss;

  if (!main_data || load_size > KSS_MAX_LOAD_SIZE || bank_count > KSS_MAX_BANK_COUNT ||
      (bank_count && !bank_data) || total_size < info_offset)
    return NULL;

  kss = (PRO_KSS *)malloc(sizeof(*kss));
  if (!kss)
    return NULL;
  kss->data = (uint8_t *)malloc(total_size);
  if (!kss->data) {
    free(kss);
    return NULL;
  }
  kss->size = total_size;

  memset(kss->data, 0xC9, total_size);
  write_kss_header(kss->data, PRO_TRACKER_COMPRESSED_LOAD_ADDRESS, (uint16_t)load_size,
                   PRO_TRACKER_COMPRESSED_LOAD_ADDRESS, bank_count, info_offset, track_count);
  memcpy(kss->data + KSS_HEADER_SIZE, main_data, load_size);
  if (bank_count)
    memcpy(kss->data + KSS_HEADER_SIZE + load_size, bank_data, bank_size);
  write_info(kss->data + KSS_HEADER_SIZE + info_offset, tracks, track_count);
  return kss;
}

static void relocate_decoder(uint8_t *decoder, uint16_t address) {
  set_word(decoder, 8, (uint16_t)(address + 0x35));
  set_word(decoder, 16, (uint16_t)(address + 0x35));
  set_word(decoder, 32, (uint16_t)(address + 0x36));
  set_word(decoder, 48, (uint16_t)(address + 0x3D));
}

static PRO_KSS *build_single(const PRO_TRACKER_INPUT *track) {
  const uint32_t metadata_size = info_size(track, 1);
  const uint32_t image_size = PRO_TRACKER_IMAGE_SIZE;
  uint8_t *image = NULL;
  uint8_t *compressed = NULL;
  uint32_t compressed_size = 0;
  uint32_t compressed_delta = 0;
  uint32_t load_size;
  PRO_KSS *kss = NULL;

  if (track->size > PRO_TRACKER_PLAYER_LOAD_ADDRESS)
    return NULL;

  image = (uint8_t *)malloc(image_size);
  if (!image)
    return NULL;
  memset(image, 0xC9, image_size);
  memcpy(image + PRO_TRACKER_DATA_ADDRESS, track->data, track->size);
  memcpy(image + PRO_TRACKER_PLAYER_LOAD_ADDRESS, pro_tracker_player, PRO_TRACKER_PLAYER_SIZE);
  /* The original player calls the MSX BIOS slot-switch routine at 0024H. KSS maps the image
   * into one already-selected RAM slot, so that call would enter the PRO data at 0024H. */
  memset(image + PRO_TRACKER_PLAYER_LOAD_ADDRESS + PRO_TRACKER_SLOT_CALL_OFFSET, 0, 3);

  if (zx0_compress_data(image, image_size, &compressed, &compressed_size, &compressed_delta) &&
      PRO_TRACKER_BOOTSTRAP_SIZE + ZX0_STANDARD_DECOMPRESSOR_SIZE + compressed_size <= KSS_MAX_LOAD_SIZE &&
      PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + PRO_TRACKER_BOOTSTRAP_SIZE +
          ZX0_STANDARD_DECOMPRESSOR_SIZE + compressed_size <= 0x10000) {
    const uint32_t bootstrap_size = PRO_TRACKER_BOOTSTRAP_SIZE + ZX0_STANDARD_DECOMPRESSOR_SIZE;
    const uint16_t decoder_address = (uint16_t)(PRO_TRACKER_COMPRESSED_LOAD_ADDRESS +
                                                PRO_TRACKER_BOOTSTRAP_SIZE);
    const uint16_t source_address = (uint16_t)(PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + bootstrap_size);
    uint8_t *bootstrap;

    load_size = bootstrap_size + compressed_size;
    bootstrap = (uint8_t *)malloc(load_size);
    if (bootstrap) {
      memset(bootstrap, 0xC9, load_size);
      bootstrap[0] = 0x21; /* LD HL,source */
      set_word(bootstrap, 1, source_address);
      bootstrap[3] = 0x11; /* LD DE,0000H */
      bootstrap[4] = 0x00;
      bootstrap[5] = 0x00;
      bootstrap[6] = 0xCD; /* CALL decoder */
      set_word(bootstrap, 7, decoder_address);
      bootstrap[9] = 0xC3; /* JP original player init */
      set_word(bootstrap, 10, PRO_TRACKER_PLAYER_INIT_ADDRESS);
      memcpy(bootstrap + PRO_TRACKER_BOOTSTRAP_SIZE, zx0_standard_decompressor,
             ZX0_STANDARD_DECOMPRESSOR_SIZE);
      relocate_decoder(bootstrap + PRO_TRACKER_BOOTSTRAP_SIZE, decoder_address);
      memcpy(bootstrap + bootstrap_size, compressed, compressed_size);
      kss = make_kss(bootstrap, load_size, NULL, 0, track, 1);
      free(bootstrap);
    }
  }

  free(compressed);
  free(image);

  if (kss)
    return kss;

  /* Compression is normally used, but retain the original uncompressed fallback for a
   * malformed/unsupported compressor result. */
  image = (uint8_t *)malloc(image_size);
  if (!image)
    return NULL;
  memset(image, 0xC9, image_size);
  memcpy(image, track->data, track->size);
  memcpy(image + PRO_TRACKER_PLAYER_LOAD_ADDRESS, pro_tracker_player, PRO_TRACKER_PLAYER_SIZE);
  memset(image + PRO_TRACKER_PLAYER_LOAD_ADDRESS + PRO_TRACKER_SLOT_CALL_OFFSET, 0, 3);
  if (image_size > KSS_MAX_LOAD_SIZE) {
    free(image);
    return NULL;
  }

  /* The fallback is a regular load at address zero. Build its header here because make_kss
   * is intentionally specialized for the compressed high-load dispatcher. */
  {
    uint32_t total_size = KSS_HEADER_SIZE + image_size + metadata_size;
    PRO_KSS *fallback = (PRO_KSS *)malloc(sizeof(*fallback));
    if (!fallback) {
      free(image);
      return NULL;
    }
    fallback->data = (uint8_t *)malloc(total_size);
    if (!fallback->data) {
      free(fallback);
      free(image);
      return NULL;
    }
    fallback->size = total_size;
    memset(fallback->data, 0xC9, total_size);
    write_kss_header(fallback->data, PRO_TRACKER_DATA_ADDRESS, (uint16_t)image_size,
                     PRO_TRACKER_PLAYER_INIT_ADDRESS, 0, image_size, 1);
    memcpy(fallback->data + KSS_HEADER_SIZE, image, image_size);
    write_info(fallback->data + KSS_HEADER_SIZE + image_size, track, 1);
    free(image);
    return fallback;
  }
}

static int emit_byte(uint8_t *data, uint32_t *offset, uint8_t value) {
  data[(*offset)++] = value;
  return 1;
}

static int emit_word(uint8_t *data, uint32_t *offset, uint16_t value) {
  data[(*offset)++] = (uint8_t)(value & 0xFF);
  data[(*offset)++] = (uint8_t)(value >> 8);
  return 1;
}

static PRO_KSS *build_multi(const PRO_TRACKER_INPUT *tracks, uint32_t count) {
  const uint32_t decoder_size = ZX0_STANDARD_DECOMPRESSOR_SIZE;
  const uint32_t main_capacity = 128 + decoder_size + count * 4 + PRO_TRACKER_PLAYER_SIZE;
  uint8_t **compressed = NULL;
  uint32_t *compressed_sizes = NULL;
  PRO_TRACKER_BANK_REF *refs = NULL;
  uint8_t *bank_data = NULL;
  uint32_t bank_capacity = 0;
  uint32_t bank_data_used = 0;
  uint32_t bank_count = 0;
  uint8_t *main_data = NULL;
  uint32_t main_offset = 0;
  uint32_t table_offset;
  uint32_t player_offset;
  uint32_t table_operand;
  uint32_t decoder_operand;
  uint32_t player_operand;
  uint32_t i;
  PRO_KSS *kss = NULL;

  compressed = (uint8_t **)calloc(count, sizeof(*compressed));
  compressed_sizes = (uint32_t *)calloc(count, sizeof(*compressed_sizes));
  refs = (PRO_TRACKER_BANK_REF *)calloc(count, sizeof(*refs));
  main_data = (uint8_t *)malloc(main_capacity);
  if (!compressed || !compressed_sizes || !refs || !main_data)
    goto cleanup;

  for (i = 0; i < count; ++i) {
    uint32_t delta;
    if (!zx0_compress_data(tracks[i].data, tracks[i].size, &compressed[i],
                           &compressed_sizes[i], &delta))
      goto cleanup;
    if (compressed_sizes[i] > KSS_BANK_SIZE)
      goto cleanup;
    if (bank_data_used % KSS_BANK_SIZE + compressed_sizes[i] > KSS_BANK_SIZE)
      bank_data_used = (bank_data_used + KSS_BANK_SIZE - 1) & ~(KSS_BANK_SIZE - 1);
    bank_count = (bank_data_used + compressed_sizes[i] + KSS_BANK_SIZE - 1) / KSS_BANK_SIZE;
    if (bank_count > KSS_MAX_BANK_COUNT)
      goto cleanup;
    if (bank_count * KSS_BANK_SIZE > bank_capacity) {
      uint32_t old_capacity = bank_capacity;
      uint8_t *new_data = (uint8_t *)realloc(bank_data, bank_count * KSS_BANK_SIZE);
      if (!new_data)
        goto cleanup;
      bank_data = new_data;
      memset(bank_data + old_capacity, 0, bank_count * KSS_BANK_SIZE - old_capacity);
      bank_capacity = bank_count * KSS_BANK_SIZE;
    }
    refs[i].bank = (uint8_t)(bank_data_used / KSS_BANK_SIZE);
    refs[i].source = (uint16_t)(0x8000 + bank_data_used % KSS_BANK_SIZE);
    memcpy(bank_data + bank_data_used, compressed[i], compressed_sizes[i]);
    bank_data_used += compressed_sizes[i];
  }
  bank_data_used = bank_count * KSS_BANK_SIZE;

  memset(main_data, 0xC9, main_capacity);

  /* Clear the data area to the same C9 fill used by the original single-track image. */
  emit_byte(main_data, &main_offset, 0x21); /* LD HL,0000H */
  emit_word(main_data, &main_offset, 0x0000);
  emit_byte(main_data, &main_offset, 0x11); /* LD DE,0001H */
  emit_word(main_data, &main_offset, 0x0001);
  emit_byte(main_data, &main_offset, 0x01); /* LD BC,0C9FFH */
  emit_word(main_data, &main_offset, 0xC9FF);
  emit_byte(main_data, &main_offset, 0x36); /* LD (HL),0C9H */
  emit_byte(main_data, &main_offset, 0xC9);
  emit_byte(main_data, &main_offset, 0xED); /* LDIR */
  emit_byte(main_data, &main_offset, 0xB0);

  emit_byte(main_data, &main_offset, 0x6F); /* LD L,A */
  emit_byte(main_data, &main_offset, 0x26); /* LD H,0 */
  emit_byte(main_data, &main_offset, 0x00);
  emit_byte(main_data, &main_offset, 0x29); /* ADD HL,HL */
  emit_byte(main_data, &main_offset, 0x29);
  emit_byte(main_data, &main_offset, 0x11); /* LD DE,table */
  table_operand = main_offset;
  emit_word(main_data, &main_offset, 0);
  emit_byte(main_data, &main_offset, 0x19); /* ADD HL,DE */
  emit_byte(main_data, &main_offset, 0x7E); /* LD A,(HL) */
  emit_byte(main_data, &main_offset, 0xD3); /* OUT (FEH),A */
  emit_byte(main_data, &main_offset, 0xFE);
  emit_byte(main_data, &main_offset, 0x23); /* INC HL */
  emit_byte(main_data, &main_offset, 0x5E); /* LD E,(HL) */
  emit_byte(main_data, &main_offset, 0x23);
  emit_byte(main_data, &main_offset, 0x56); /* LD D,(HL) */
  emit_byte(main_data, &main_offset, 0xEB); /* EX DE,HL */
  emit_byte(main_data, &main_offset, 0x11); /* LD DE,0000H */
  emit_word(main_data, &main_offset, 0x0000);
  emit_byte(main_data, &main_offset, 0xCD); /* CALL decoder */
  decoder_operand = main_offset;
  emit_word(main_data, &main_offset, 0);
  emit_byte(main_data, &main_offset, 0x21); /* LD HL,player */
  player_operand = main_offset;
  emit_word(main_data, &main_offset, 0);
  emit_byte(main_data, &main_offset, 0x11); /* LD DE,CA00H */
  emit_word(main_data, &main_offset, PRO_TRACKER_PLAYER_LOAD_ADDRESS);
  emit_byte(main_data, &main_offset, 0x01); /* LD BC,player size */
  emit_word(main_data, &main_offset, (uint16_t)PRO_TRACKER_PLAYER_SIZE);
  emit_byte(main_data, &main_offset, 0xED); /* LDIR */
  emit_byte(main_data, &main_offset, 0xB0);
  emit_byte(main_data, &main_offset, 0xAF); /* XOR A: player init sees a stable song value */
  emit_byte(main_data, &main_offset, 0xC3); /* JP CA00H */
  emit_word(main_data, &main_offset, PRO_TRACKER_PLAYER_INIT_ADDRESS);

  table_offset = main_offset + decoder_size;
  player_offset = table_offset + count * 4;
  if (player_offset + PRO_TRACKER_PLAYER_SIZE > main_capacity)
    goto cleanup;

  set_word(main_data, table_operand,
           (uint16_t)(PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + table_offset));
  set_word(main_data, decoder_operand,
           (uint16_t)(PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + main_offset));
  set_word(main_data, player_operand,
           (uint16_t)(PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + player_offset));
  memcpy(main_data + main_offset, zx0_standard_decompressor, decoder_size);
  relocate_decoder(main_data + main_offset,
                   (uint16_t)(PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + main_offset));
  for (i = 0; i < count; ++i) {
    uint32_t table_entry = table_offset + i * 4;
    main_data[table_entry] = refs[i].bank;
    set_word(main_data, table_entry + 1, refs[i].source);
    main_data[table_entry + 3] = 0;
  }
  memcpy(main_data + player_offset, pro_tracker_player, PRO_TRACKER_PLAYER_SIZE);
  memset(main_data + player_offset + PRO_TRACKER_SLOT_CALL_OFFSET, 0, 3);
  main_offset = player_offset + PRO_TRACKER_PLAYER_SIZE;

  if (main_offset > KSS_MAX_LOAD_SIZE ||
      PRO_TRACKER_COMPRESSED_LOAD_ADDRESS + main_offset > 0x10000)
    goto cleanup;
  kss = make_kss(main_data, main_offset, bank_data, bank_count, tracks, count);

cleanup:
  if (compressed) {
    for (i = 0; i < count; ++i)
      free(compressed[i]);
  }
  free(compressed);
  free(compressed_sizes);
  free(refs);
  free(bank_data);
  free(main_data);
  return kss;
}

PRO_KSS *pro_trackers_to_kss(const PRO_TRACKER_INPUT *tracks, uint32_t count) {
  uint32_t i;

  if (!tracks || count == 0 || count > 256)
    return NULL;
  for (i = 0; i < count; ++i) {
    if (!pro_tracker_is_valid(tracks[i].data, tracks[i].size) ||
        tracks[i].size > PRO_TRACKER_PLAYER_LOAD_ADDRESS)
      return NULL;
  }
  return count == 1 ? build_single(&tracks[0]) : build_multi(tracks, count);
}

PRO_KSS *pro_tracker_to_kss(const uint8_t *data, uint32_t size, const char *title) {
  PRO_TRACKER_INPUT track;

  track.data = data;
  track.size = size;
  track.title = title;
  return pro_trackers_to_kss(&track, 1);
}

void pro_kss_delete(PRO_KSS *kss) {
  if (kss) {
    free(kss->data);
    free(kss);
  }
}
