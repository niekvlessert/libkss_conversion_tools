#ifndef PRO_TRACKER_CONVERTER_H
#define PRO_TRACKER_CONVERTER_H

#include <stdint.h>

typedef struct {
  uint8_t *data;
  uint32_t size;
} PRO_KSS;

typedef struct {
  const uint8_t *data;
  uint32_t size;
  const char *title;
} PRO_TRACKER_INPUT;

int pro_tracker_is_valid(const uint8_t *data, uint32_t size);
PRO_KSS *pro_tracker_to_kss(const uint8_t *data, uint32_t size, const char *title);
PRO_KSS *pro_trackers_to_kss(const PRO_TRACKER_INPUT *tracks, uint32_t count);
void pro_kss_delete(PRO_KSS *kss);

#endif
