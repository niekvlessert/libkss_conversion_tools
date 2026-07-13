#include "ksp/engine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_error(char *error, size_t size, const char *message) {
  if (error && size) snprintf(error, size, "%s", message);
}

static char *trim(char *text) {
  char *end;
  while (*text && isspace((unsigned char)*text)) text++;
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) --end;
  *end = 0;
  return text;
}

static int parse_number(const char *text, uint32_t *value) {
  char *end;
  unsigned long parsed = strtoul(text, &end, 0);
  if (end == text || *trim(end) != 0 || parsed > UINT32_MAX) return 0;
  *value = (uint32_t)parsed;
  return 1;
}

static int set_field(KSP_ENGINE_DESCRIPTOR *d, const char *key,
                     const char *value) {
  uint32_t number;
  if (!strcmp(key, "engine_type") && !strcmp(value, "mbwave")) number = 1;
  else if (!parse_number(value, &number)) return 0;

  if (!strcmp(key, "engine_type")) d->engine_type = (uint16_t)number;
  else if (!strcmp(key, "load_address")) d->load_address = (uint16_t)number;
  else if (!strcmp(key, "init_address")) d->init_address = (uint16_t)number;
  else if (!strcmp(key, "play_address")) d->play_address = (uint16_t)number;
  else if (!strcmp(key, "stop_address")) d->stop_address = (uint16_t)number;
  else if (!strcmp(key, "capability_address")) d->capability_address = (uint16_t)number;
  else if (!strcmp(key, "song_window_address")) d->song_window_address = (uint16_t)number;
  else if (!strcmp(key, "work_address")) d->work_address = (uint16_t)number;
  else if (!strcmp(key, "work_size")) d->work_size = (uint16_t)number;
  else if (!strcmp(key, "tick_rate_num")) d->tick_rate_num = (uint16_t)number;
  else if (!strcmp(key, "tick_rate_den")) d->tick_rate_den = (uint16_t)number;
  else if (!strcmp(key, "minimum_mapper_ram")) d->minimum_mapper_ram = number;
  else if (!strcmp(key, "flags")) d->flags = number;
  else return -1; /* unknown fields are forward-compatible */
  return 1;
}

int ksp_read_engine_descriptor_text(const char *path,
                                    KSP_ENGINE_DESCRIPTOR *descriptor,
                                    char *error, size_t error_size) {
  FILE *file = fopen(path, "r");
  char line[512];
  unsigned line_number = 0;
  KSP_ENGINE_DESCRIPTOR d;

  if (!file) {
    snprintf(error, error_size, "could not open descriptor %s", path);
    return 0;
  }
  memset(&d, 0, sizeof(d));
  d.engine_type = 1;
  d.tick_rate_num = 50;
  d.tick_rate_den = 1;
  while (fgets(line, sizeof(line), file)) {
    char *text;
    char *equals;
    int result;
    line_number++;
    text = trim(line);
    if (!*text || *text == '#') continue;
    equals = strchr(text, '=');
    if (!equals) {
      snprintf(error, error_size, "%s:%u: expected key=value", path, line_number);
      fclose(file);
      return 0;
    }
    *equals++ = 0;
    text = trim(text);
    equals = trim(equals);
    result = set_field(&d, text, equals);
    if (result == 0) {
      snprintf(error, error_size, "%s:%u: invalid value", path, line_number);
      fclose(file);
      return 0;
    }
  }
  fclose(file);
  if (!d.load_address || !d.init_address || !d.play_address ||
      !d.tick_rate_num || !d.tick_rate_den) {
    set_error(error, error_size, "descriptor is missing required addresses or tick rate");
    return 0;
  }
  *descriptor = d;
  return 1;
}

void ksp_encode_engine_descriptor(const KSP_ENGINE_DESCRIPTOR *d,
                                  uint8_t output[KSP_ENGINE_DESCRIPTOR_SIZE]) {
  memset(output, 0, KSP_ENGINE_DESCRIPTOR_SIZE);
  memcpy(output, "KED1", 4);
  put16(output + 4, KSP_ENGINE_DESCRIPTOR_SIZE);
  put16(output + 6, d->engine_type);
  put16(output + 8, d->load_address);
  put16(output + 10, d->init_address);
  put16(output + 12, d->play_address);
  put16(output + 14, d->stop_address);
  put16(output + 16, d->capability_address);
  put16(output + 18, d->song_window_address);
  put16(output + 20, d->work_address);
  put16(output + 22, d->work_size);
  put16(output + 24, d->tick_rate_num);
  put16(output + 26, d->tick_rate_den);
  put32(output + 28, d->minimum_mapper_ram);
  put32(output + 32, d->flags);
}

int ksp_validate_engine_descriptor(const uint8_t *data, uint32_t size,
                                   char *error, size_t error_size) {
  uint32_t load_end;
  if (size < KSP_ENGINE_DESCRIPTOR_SIZE || memcmp(data, "KED1", 4) != 0 ||
      get16(data + 4) != KSP_ENGINE_DESCRIPTOR_SIZE || get16(data + 6) != 1) {
    set_error(error, error_size, "invalid KED1 engine descriptor");
    return 0;
  }
  load_end = (uint32_t)get16(data + 8) + get16(data + 22);
  if (load_end > 0x10000u || !get16(data + 10) || !get16(data + 12) ||
      !get16(data + 24) || !get16(data + 26)) {
    set_error(error, error_size, "engine descriptor has invalid address or timing");
    return 0;
  }
  return 1;
}
