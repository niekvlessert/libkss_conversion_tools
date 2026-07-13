#ifndef KSP_MBWAVE_H
#define KSP_MBWAVE_H

#include <stdint.h>

/* Build the engine-ready MWM image used by the DMV MBWave driver. The raw
 * MWM remains the on-disk SONG resource; this helper is for playback loaders
 * that need to materialize the image at 8000H. */
int ksp_compact_mwm(const uint8_t *input, uint32_t input_size,
                    uint8_t **output, uint32_t *output_size);

uint32_t ksp_mbwave_bootstrap_size(void);
int ksp_copy_mbwave_bootstrap(uint8_t *output, uint32_t output_size);

#endif
