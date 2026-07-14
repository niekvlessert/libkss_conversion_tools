#include "moonsound_opl4.h"

#include "kss/kss.h"
#include "kssplay.h"

#include <errno.h>
#include <stdint.h>
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

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s --rom YRW801.ROM [--mwk KIT.MWK] [--song N] [--seconds N] [--trace-io FILE] INPUT.KSS OUTPUT.WAV\n",
          program);
}

static int number(const char *text, int minimum, int maximum, int *result) {
  char *end;
  long value;
  errno = 0;
  value = strtol(text, &end, 10);
  if (errno || *text == '\0' || *end != '\0' || value < minimum || value > maximum)
    return 0;
  *result = (int)value;
  return 1;
}

static int write_wav_header(FILE *file, uint32_t frames) {
  uint8_t header[44];
  uint32_t data_size = frames * 4u;
  memset(header, 0, sizeof(header));
  memcpy(header, "RIFF", 4);
  put32(header + 4, 36u + data_size);
  memcpy(header + 8, "WAVEfmt ", 8);
  put32(header + 16, 16);
  put16(header + 20, 1);
  put16(header + 22, 2);
  put32(header + 24, 44100);
  put32(header + 28, 44100u * 4u);
  put16(header + 32, 4);
  put16(header + 34, 16);
  memcpy(header + 36, "data", 4);
  put32(header + 40, data_size);
  return fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

static uint32_t io_counts[256];
static FILE *io_trace;
static uint32_t io_sequence;
static uint8_t io_wave_register;

static void trace_memory(void *context, uint32_t address, uint32_t data) {
  KSSPLAY *player = (KSSPLAY *)context;
  if (io_trace && address >= 0x4cb5u && address < 0x4cc9u)
    fprintf(io_trace, "MEM PC=%04X ADDR=%04X VALUE=%02X\n",
            (unsigned)(player->vm->context.pc & 0xffffu),
            (unsigned)(address & 0xffffu), (unsigned)(data & 0xffu));
}

static void trace_io(void *context, uint32_t port, uint32_t data) {
  KSSPLAY *player = (KSSPLAY *)context;
  uint32_t iy;
  unsigned i;
  io_counts[port & 0xffu]++;
  if ((port & 0xffu) == 0x7e)
    io_wave_register = (uint8_t)data;
  if (io_trace) {
    fprintf(io_trace, "%08u %02X %02X\n", io_sequence++,
            (unsigned)(port & 0xffu), (unsigned)(data & 0xffu));
    if ((port & 0xffu) == 0x7f && io_wave_register == 0x7b && player) {
      iy = (uint32_t)player->vm->context.regs8[REGID_IYL] |
           ((uint32_t)player->vm->context.regs8[REGID_IYH] << 8);
      fprintf(io_trace, "STATE PC=%04X IY=%04X DATA=",
              (unsigned)(player->vm->context.pc & 0xffffu), (unsigned)iy);
      for (i = 0; i < 20; i++)
        fprintf(io_trace, "%02X", (unsigned)MMAP_read_memory(player->vm->mmap,
                                                               (iy + i) & 0xffffu));
      fputc('\n', io_trace);
    }
  }
}

int main(int argc, char **argv) {
  const char *rom_path = NULL;
  const char *mwk_path = NULL;
  const char *input_path = NULL;
  const char *output_path = NULL;
  const char *trace_path = NULL;
  int song = 0;
  int seconds = 10;
  int i;
  KSS *kss = NULL;
  KSSPLAY *player = NULL;
  KSS_MOONSOUND *moonsound = NULL;
  FILE *output = NULL;
  int16_t samples[4096 * 2];
  uint32_t remaining;
  uint32_t total_frames;
  char error[256];

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
      rom_path = argv[++i];
    } else if (strncmp(argv[i], "--rom=", 6) == 0) {
      rom_path = argv[i] + 6;
    } else if (strcmp(argv[i], "--mwk") == 0 && i + 1 < argc) {
      mwk_path = argv[++i];
    } else if (strncmp(argv[i], "--mwk=", 6) == 0) {
      mwk_path = argv[i] + 6;
    } else if (strcmp(argv[i], "--song") == 0 && i + 1 < argc) {
      if (!number(argv[++i], 0, 255, &song)) {
        fprintf(stderr, "error: invalid song number\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      if (!number(argv[++i], 1, 3600, &seconds)) {
        fprintf(stderr, "error: invalid duration\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--trace-io") == 0 && i + 1 < argc) {
      trace_path = argv[++i];
    } else if (strncmp(argv[i], "--trace-io=", 11) == 0) {
      trace_path = argv[i] + 11;
    } else if (argv[i][0] != '-' && !input_path) {
      input_path = argv[i];
    } else if (argv[i][0] != '-' && !output_path) {
      output_path = argv[i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (!rom_path || !input_path || !output_path) {
    usage(argv[0]);
    return 2;
  }

  kss = KSS_load_file((char *)input_path);
  if (!kss) {
    fprintf(stderr, "error: could not load %s\n", input_path);
    return 1;
  }
  if (song < kss->trk_min || song > kss->trk_max) {
    fprintf(stderr, "error: song %d is outside range %u-%u\n", song,
            kss->trk_min, kss->trk_max);
    KSS_delete(kss);
    return 1;
  }
  player = KSSPLAY_new(44100, 2, 16);
  if (!player || KSSPLAY_set_data(player, kss) != 0) {
    fprintf(stderr, "error: could not create KSS player\n");
    KSSPLAY_delete(player);
    KSS_delete(kss);
    return 1;
  }
  moonsound = kss_moonsound_create(rom_path, 44100, error, sizeof(error));
  if (!moonsound) {
    fprintf(stderr, "error: %s\n", error);
    KSSPLAY_delete(player);
    KSS_delete(kss);
    return 1;
  }
  if (mwk_path && !kss_moonsound_load_mwk(moonsound, mwk_path, error,
                                          sizeof(error))) {
    fprintf(stderr, "error: %s\n", error);
    kss_moonsound_delete(moonsound);
    KSSPLAY_delete(player);
    KSS_delete(kss);
    return 1;
  }
  KSSPLAY_set_moonsound(player, moonsound);
  memset(io_counts, 0, sizeof(io_counts));
  io_sequence = 0;
  io_wave_register = 0;
  if (trace_path) {
    io_trace = fopen(trace_path, "w");
    if (!io_trace) {
      fprintf(stderr, "error: could not create I/O trace %s\n", trace_path);
      kss_moonsound_delete(moonsound);
      KSSPLAY_delete(player);
      KSS_delete(kss);
      return 1;
    }
  }
  KSSPLAY_set_iowrite_handler(player, player, trace_io);
  KSSPLAY_set_memwrite_handler(player, player, trace_memory);
  KSSPLAY_reset(player, (uint32_t)song, 0);
  total_frames = (uint32_t)seconds * 44100u;

  output = fopen(output_path, "wb");
  if (!output || !write_wav_header(output, total_frames)) {
    fprintf(stderr, "error: could not create %s\n", output_path);
    if (output) fclose(output);
    kss_moonsound_delete(moonsound);
    KSSPLAY_delete(player);
    KSS_delete(kss);
    return 1;
  }
  remaining = total_frames;
  while (remaining) {
    uint32_t frames = remaining > 4096 ? 4096 : remaining;
    KSSPLAY_calc(player, samples, frames);
    if (fwrite(samples, sizeof(samples[0]), frames * 2u, output) != frames * 2u) {
      fprintf(stderr, "error: could not write %s\n", output_path);
      fclose(output);
      kss_moonsound_delete(moonsound);
      KSSPLAY_delete(player);
      KSS_delete(kss);
      return 1;
    }
    remaining -= frames;
  }
  fclose(output);
  if (io_trace) {
    fclose(io_trace);
    io_trace = NULL;
  }
  fprintf(stderr, "rendered %u stereo frames to %s; MoonSound writes: "
                  "7E=%u 7F=%u C4=%u C5=%u C6=%u C7=%u\n",
          total_frames, output_path, io_counts[0x7e], io_counts[0x7f],
          io_counts[0xc4], io_counts[0xc5], io_counts[0xc6], io_counts[0xc7]);
  kss_moonsound_delete(moonsound);
  KSSPLAY_delete(player);
  KSS_delete(kss);
  return 0;
}
