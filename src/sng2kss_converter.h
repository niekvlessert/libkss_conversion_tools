#ifndef SNG2KSS_CONVERTER_H
#define SNG2KSS_CONVERTER_H

#include <stdint.h>

typedef struct {
  uint8_t *data;
  uint32_t size;
} SNG_KSS;

int sng_is_valid(const uint8_t *data, uint32_t size);
SNG_KSS *sngs_to_kss(const uint8_t **data, const uint32_t *sizes, const char **titles, uint32_t count);
void sng_kss_delete(SNG_KSS *result);

#endif

