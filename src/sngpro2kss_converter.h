#ifndef SNGPRO2KSS_CONVERTER_H
#define SNGPRO2KSS_CONVERTER_H

#include <stdint.h>

typedef enum {
  SNGPRO_TRACK_SNG = 0,
  SNGPRO_TRACK_PRO = 1
} SNGPRO_TRACK_TYPE;

typedef struct {
  const uint8_t *data;
  uint32_t size;
  const char *title;
  SNGPRO_TRACK_TYPE type;
} SNGPRO_INPUT;

typedef struct {
  uint8_t *data;
  uint32_t size;
} SNGPRO_KSS;

SNGPRO_KSS *sngpro_tracks_to_kss(const SNGPRO_INPUT *tracks, uint32_t count);
void sngpro_kss_delete(SNGPRO_KSS *kss);

#endif
