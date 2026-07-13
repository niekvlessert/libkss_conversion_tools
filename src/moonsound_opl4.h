#ifndef KSS_MOONSOUND_OPL4_H
#define KSS_MOONSOUND_OPL4_H

#include <stddef.h>
#include <stdint.h>

typedef struct KSS_MOONSOUND KSS_MOONSOUND;

KSS_MOONSOUND *kss_moonsound_create(const char *rom_path, uint32_t rate,
                                    char *error, size_t error_size);
void kss_moonsound_delete(KSS_MOONSOUND *device);
void kss_moonsound_reset(KSS_MOONSOUND *device);
int kss_moonsound_load_mwk(KSS_MOONSOUND *device, const char *path,
                           char *error, size_t error_size);
void kss_moonsound_write(KSS_MOONSOUND *device, uint32_t port, uint32_t data);
uint32_t kss_moonsound_read(KSS_MOONSOUND *device, uint32_t port);
void kss_moonsound_calc(KSS_MOONSOUND *device, int32_t *left, int32_t *right);

#endif
