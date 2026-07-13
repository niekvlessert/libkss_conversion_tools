#include "moonsound_opl4.h"

#include "SoundDevs.h"
#include "SoundEmu.h"
#include "mwm_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct KSS_MOONSOUND {
  DEV_INFO device;
  DEV_INFO fm_device;
  DEVFUNC_WRITE_A8D8 write8;
  DEVFUNC_WRITE_A8D8 fm_write;
  DEVFUNC_READ_A8D8 read8;
  DEVFUNC_WRITE_MEMSIZE alloc_rom;
  DEVFUNC_WRITE_BLOCK write_rom;
  DEVFUNC_WRITE_MEMSIZE alloc_ram;
  DEVFUNC_WRITE_BLOCK write_ram;
  uint32_t rate;
};

static void set_error(char *error, size_t error_size, const char *message) {
  if (error && error_size) {
    snprintf(error, error_size, "%s", message);
  }
}

static int read_file(const char *path, uint8_t **data, size_t *size,
                     char *error, size_t error_size) {
  FILE *file;
  long length;
  uint8_t *buffer;

  if (!path || !path[0]) {
    set_error(error, error_size, "MoonSound ROM path is empty");
    return 0;
  }
  file = fopen(path, "rb");
  if (!file) {
    snprintf(error, error_size, "could not open MoonSound ROM: %s", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    set_error(error, error_size, "could not determine MoonSound ROM size");
    return 0;
  }
  buffer = (uint8_t *)malloc((size_t)length);
  if (!buffer || fread(buffer, 1, (size_t)length, file) != (size_t)length) {
    free(buffer);
    fclose(file);
    set_error(error, error_size, "could not read MoonSound ROM");
    return 0;
  }
  fclose(file);
  *data = buffer;
  *size = (size_t)length;
  return 1;
}

static int map_port(uint32_t port) {
  switch (port & 0xff) {
  case 0xc4:
    return 0;
  case 0xc5:
    return 1;
  case 0xc6:
    return 2;
  case 0xc7:
    return 3;
  case 0x7e:
    return 4;
  case 0x7f:
    return 5;
  default:
    return -1;
  }
}

KSS_MOONSOUND *kss_moonsound_create(const char *rom_path, uint32_t rate,
                                    char *error, size_t error_size) {
  KSS_MOONSOUND *device;
  DEV_GEN_CFG config;
  uint8_t *rom = NULL;
  size_t rom_size = 0;

  if (!read_file(rom_path, &rom, &rom_size, error, error_size)) {
    return NULL;
  }
  if (rom_size > UINT32_MAX) {
    free(rom);
    set_error(error, error_size, "MoonSound ROM is too large");
    return NULL;
  }

  device = (KSS_MOONSOUND *)calloc(1, sizeof(*device));
  if (!device) {
    free(rom);
    set_error(error, error_size, "out of memory creating MoonSound device");
    return NULL;
  }
  device->rate = rate;
  memset(&config, 0, sizeof(config));
  config.srMode = DEVRI_SRMODE_NATIVE;
  config.clock = 33868800;
  config.smplRate = rate;

  if (SndEmu_Start(DEVID_YMF278B, &config, &device->device) != 0) {
    free(rom);
    free(device);
    set_error(error, error_size, "could not start the YMF278B emulator");
    return NULL;
  }
  {
    DEV_GEN_CFG fm_config = config;
    fm_config.clock = config.clock * 8 / 19;
    if (SndEmu_Start(DEVID_YMF262, &fm_config, &device->fm_device) != 0) {
      SndEmu_Stop(&device->device);
      free(rom);
      free(device);
      set_error(error, error_size, "could not start the MoonSound FM emulator");
      return NULL;
    }
  }
  SndEmu_GetDeviceFunc(device->device.devDef, RWF_REGISTER | RWF_WRITE,
                       DEVRW_A8D8, 0, (void **)&device->write8);
  SndEmu_GetDeviceFunc(device->device.devDef, RWF_REGISTER | RWF_READ,
                       DEVRW_A8D8, 0, (void **)&device->read8);
  SndEmu_GetDeviceFunc(device->device.devDef, RWF_MEMORY | RWF_WRITE,
                       DEVRW_MEMSIZE, 0x524f, (void **)&device->alloc_rom);
  SndEmu_GetDeviceFunc(device->device.devDef, RWF_MEMORY | RWF_WRITE,
                       DEVRW_BLOCK, 0x524f, (void **)&device->write_rom);
  SndEmu_GetDeviceFunc(device->device.devDef, RWF_MEMORY | RWF_WRITE,
                       DEVRW_MEMSIZE, 0x5241, (void **)&device->alloc_ram);
  SndEmu_GetDeviceFunc(device->device.devDef, RWF_MEMORY | RWF_WRITE,
                       DEVRW_BLOCK, 0x5241, (void **)&device->write_ram);
  SndEmu_GetDeviceFunc(device->fm_device.devDef, RWF_REGISTER | RWF_WRITE,
                       DEVRW_A8D8, 0, (void **)&device->fm_write);
  if (!device->write8 || !device->fm_write || !device->alloc_rom || !device->write_rom) {
    SndEmu_Stop(&device->fm_device);
    SndEmu_Stop(&device->device);
    free(rom);
    free(device);
    set_error(error, error_size, "YMF278B emulator has no required functions");
    return NULL;
  }
  device->alloc_rom(device->device.dataPtr, (uint32_t)rom_size);
  device->write_rom(device->device.dataPtr, 0, (uint32_t)rom_size, rom);
  free(rom);

  /* NEW + NEW2 enables the OPL4 wave-table side. The two volume registers
   * are also what the libmoonsound reference player uses. */
  device->write8(device->device.dataPtr, 2, 0x05);
  device->write8(device->device.dataPtr, 3, 0x03);
  device->write8(device->device.dataPtr, 4, 0xf8);
  device->write8(device->device.dataPtr, 5, 0x00);
  device->write8(device->device.dataPtr, 4, 0xf9);
  device->write8(device->device.dataPtr, 5, 0x00);
  return device;
}

void kss_moonsound_delete(KSS_MOONSOUND *device) {
  if (!device)
    return;
  SndEmu_Stop(&device->fm_device);
  SndEmu_Stop(&device->device);
  free(device);
}

void kss_moonsound_reset(KSS_MOONSOUND *device) {
  if (!device)
    return;
  if (device->device.devDef->Reset)
    device->device.devDef->Reset(device->device.dataPtr);
  if (device->fm_device.devDef->Reset)
    device->fm_device.devDef->Reset(device->fm_device.dataPtr);
  device->write8(device->device.dataPtr, 2, 0x05);
  device->write8(device->device.dataPtr, 3, 0x03);
  device->write8(device->device.dataPtr, 4, 0xf8);
  device->write8(device->device.dataPtr, 5, 0x00);
  device->write8(device->device.dataPtr, 4, 0xf9);
  device->write8(device->device.dataPtr, 5, 0x00);
}

int kss_moonsound_load_mwk(KSS_MOONSOUND *device, const char *path,
                           char *error, size_t error_size) {
  MwkKit kit;
  uint8_t *ram;
  if (!device || !path || !device->alloc_ram || !device->write_ram) {
    set_error(error, error_size, "MoonSound RAM upload is unavailable");
    return 0;
  }
  ram = (uint8_t *)calloc(1, 2u * 1024u * 1024u);
  if (!ram) {
    set_error(error, error_size, "out of memory for MWK data");
    return 0;
  }
  if (!load_mwk(path, &kit, ram)) {
    free(ram);
    snprintf(error, error_size, "could not load MWK: %s", path);
    return 0;
  }
  device->alloc_ram(device->device.dataPtr, 2u * 1024u * 1024u);
  device->write_ram(device->device.dataPtr, 0, 2u * 1024u * 1024u, ram);
  free(ram);
  return 1;
}

void kss_moonsound_write(KSS_MOONSOUND *device, uint32_t port,
                         uint32_t data) {
  int offset;
  if (!device || !device->write8)
    return;
  offset = map_port(port);
  if (offset >= 0) {
    device->write8(device->device.dataPtr, (uint8_t)offset, (uint8_t)data);
    if (offset < 4)
      device->fm_write(device->fm_device.dataPtr, (uint8_t)offset,
                       (uint8_t)data);
  }
}

uint32_t kss_moonsound_read(KSS_MOONSOUND *device, uint32_t port) {
  int offset;
  if (!device || !device->read8)
    return 0xff;
  offset = map_port(port);
  return offset >= 0 ? device->read8(device->device.dataPtr, (uint8_t)offset)
                     : 0xff;
}

void kss_moonsound_calc(KSS_MOONSOUND *device, int32_t *left, int32_t *right) {
  DEV_SMPL samples[2] = {0, 0};
  DEV_SMPL fm_samples[2] = {0, 0};
  DEV_SMPL *outputs[2] = {&samples[0], &samples[1]};
  DEV_SMPL *fm_outputs[2] = {&fm_samples[0], &fm_samples[1]};

  if (!device || !left || !right)
    return;
  device->device.devDef->Update(device->device.dataPtr, 1, outputs);
  if (device->fm_device.devDef->Update)
    device->fm_device.devDef->Update(device->fm_device.dataPtr, 1, fm_outputs);
  *left = samples[0] + fm_samples[0];
  *right = samples[1] + fm_samples[1];
}
