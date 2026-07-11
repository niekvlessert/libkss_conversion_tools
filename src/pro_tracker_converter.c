#include <stdlib.h>
#include <string.h>

#include "pro_tracker_converter.h"
#include "pro_tracker_player.h"

#define KSS_HEADER_SIZE 0x20
#define KSS_MAX_LOAD_SIZE 0xFE00
#define PRO_TRACKER_DATA_ADDRESS 0x0000
#define PRO_TRACKER_MIN_SIZE 0x146
#define PRO_TRACKER_SLOT_CALL_OFFSET 0x762
#define PRO_TRACKER_MUSIC_VOLUME 0x20 /* KSS volume scale: 0x20 is 4x */

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

static void write_kss_header(uint8_t *data, uint16_t load_size, uint32_t info_offset) {
  memcpy(data, "KSSX", 4);
  set_word(data, 0x04, PRO_TRACKER_DATA_ADDRESS);
  set_word(data, 0x06, load_size);
  set_word(data, 0x08, PRO_TRACKER_PLAYER_INIT_ADDRESS);
  set_word(data, 0x0A, PRO_TRACKER_PLAYER_PLAY_ADDRESS);
  data[0x0C] = 0;
  data[0x0D] = 0;
  data[0x0E] = 0x10;
  data[0x0F] = 0x41; /* PAL MSX-MUSIC / YM2413 */
  set_dword(data, 0x10, info_offset);
  set_dword(data, 0x14, 0);
  set_word(data, 0x18, 0);
  set_word(data, 0x1A, 0);
  data[0x1C] = 0;
  data[0x1D] = 0;
  data[0x1E] = PRO_TRACKER_MUSIC_VOLUME;
  data[0x1F] = 0;
}

static uint32_t info_size(const char *title) {
  const char *value = title ? title : "-";
  return 0x10 + 10 + (uint32_t)strlen(value) + 1;
}

static void write_info(uint8_t *data, const char *title) {
  const char *value = title ? title : "-";
  uint32_t offset = 0x10;

  memcpy(data, "INFO", 4);
  set_dword(data, 4, info_size(value) - 0x10);
  set_word(data, 8, 1);
  memset(data + 10, 0, 6);

  data[offset++] = 0;
  data[offset++] = 0;
  set_dword(data, offset, 0);
  offset += 4;
  set_dword(data, offset, 0);
  offset += 4;
  strcpy((char *)(data + offset), value);
}

PRO_KSS *pro_tracker_to_kss(const uint8_t *data, uint32_t size, const char *title) {
  const uint32_t player_end = PRO_TRACKER_PLAYER_LOAD_ADDRESS + PRO_TRACKER_PLAYER_SIZE;
  const uint32_t load_size = size > player_end ? size : player_end;
  const uint32_t metadata_size = info_size(title);
  const uint32_t total_size = KSS_HEADER_SIZE + load_size + metadata_size;
  PRO_KSS *kss;

  if (!pro_tracker_is_valid(data, size) || size > PRO_TRACKER_PLAYER_LOAD_ADDRESS ||
      load_size > KSS_MAX_LOAD_SIZE)
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
  write_kss_header(kss->data, (uint16_t)load_size, load_size);
  memcpy(kss->data + KSS_HEADER_SIZE + PRO_TRACKER_DATA_ADDRESS, data, size);
  memcpy(kss->data + KSS_HEADER_SIZE + PRO_TRACKER_PLAYER_LOAD_ADDRESS, pro_tracker_player,
         PRO_TRACKER_PLAYER_SIZE);
  /* The original player calls the MSX BIOS slot-switch routine at 0024H. KSS maps the image
   * into one already-selected RAM slot, so that call would enter the PRO data at 0024H. */
  memset(kss->data + KSS_HEADER_SIZE + PRO_TRACKER_PLAYER_LOAD_ADDRESS + PRO_TRACKER_SLOT_CALL_OFFSET, 0, 3);
  write_info(kss->data + KSS_HEADER_SIZE + load_size, title);
  return kss;
}

void pro_kss_delete(PRO_KSS *kss) {
  if (kss) {
    free(kss->data);
    free(kss);
  }
}
