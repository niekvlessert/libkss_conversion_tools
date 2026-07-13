#ifndef KSP_ENGINE_H
#define KSP_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#define KSP_ENGINE_DESCRIPTOR_SIZE 36u

typedef struct {
  uint16_t engine_type;
  uint16_t load_address;
  uint16_t init_address;
  uint16_t play_address;
  uint16_t stop_address;
  uint16_t capability_address;
  uint16_t song_window_address;
  uint16_t work_address;
  uint16_t work_size;
  uint16_t tick_rate_num;
  uint16_t tick_rate_den;
  uint32_t minimum_mapper_ram;
  uint32_t flags;
} KSP_ENGINE_DESCRIPTOR;

int ksp_read_engine_descriptor_text(const char *path,
                                    KSP_ENGINE_DESCRIPTOR *descriptor,
                                    char *error, size_t error_size);
void ksp_encode_engine_descriptor(const KSP_ENGINE_DESCRIPTOR *descriptor,
                                  uint8_t output[KSP_ENGINE_DESCRIPTOR_SIZE]);
int ksp_validate_engine_descriptor(const uint8_t *data, uint32_t size,
                                   char *error, size_t error_size);

#endif
