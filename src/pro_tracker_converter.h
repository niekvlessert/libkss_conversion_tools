#ifndef PRO_TRACKER_CONVERTER_H
#define PRO_TRACKER_CONVERTER_H

#include <stdint.h>

typedef struct {
  uint8_t *data;
  uint32_t size;
} PRO_KSS;

int pro_tracker_is_valid(const uint8_t *data, uint32_t size);
PRO_KSS *pro_tracker_to_kss(const uint8_t *data, uint32_t size, const char *title);
void pro_kss_delete(PRO_KSS *kss);

#endif
