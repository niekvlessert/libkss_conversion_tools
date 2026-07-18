#include "kssplay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100u
#define MAX_EVENTS 4000000u

static int capture_reads;
static int capture_all_reads;

typedef struct {
  uint8_t kind;             /* I = PSG port, S = SCC memory. */
  uint16_t address;
  uint8_t value;
  uint16_t pc;
  uint16_t hl;
  uint16_t de;
} EVENT;

typedef struct {
  KSSPLAY *player;
  EVENT *events;
  size_t count;
  int overflow;
  uint16_t work_base;
  uint16_t work_size;
  int work_snapshot;
} TRACE;

static void append(TRACE *trace, uint8_t kind, uint32_t address, uint32_t value) {
  EVENT *event;
  if (trace->count == MAX_EVENTS) {
    trace->overflow = 1;
    return;
  }
  event = &trace->events[trace->count++];
  event->kind = kind;
  event->address = (uint16_t)address;
  event->value = (uint8_t)value;
  event->pc = trace->player
      ? (uint16_t)(trace->player->vm->context.t_pc & 0xffff) : 0;
  event->hl = trace->player
      ? (uint16_t)((trace->player->vm->context.regs8[REGID_H] << 8) |
                   trace->player->vm->context.regs8[REGID_L]) : 0;
  event->de = trace->player
      ? (uint16_t)((trace->player->vm->context.regs8[REGID_D] << 8) |
                   trace->player->vm->context.regs8[REGID_E]) : 0;
}

static void io_write(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
  address &= 0xff;
  if (address == 0xa0 || address == 0xa1)
    append(trace, 'I', address, value);
}

static void memory_write(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
  uint16_t offset;
  address &= 0xffff;
  if (address == 0x9880 && trace->work_size && !trace->work_snapshot) {
    trace->work_snapshot = 1;
    for (offset = 0; offset < trace->work_size; offset++)
      append(trace, 'M', offset,
             KSSPLAY_read_memory(trace->player, trace->work_base + offset));
  }
  if (0x9800 <= address && address <= 0x98ff)
    append(trace, 'S', address, value);
  else if (trace->work_size && trace->work_base <= address &&
           address < (uint32_t)trace->work_base + trace->work_size)
    append(trace, 'W', address - trace->work_base, value);
}

static void memory_read(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
  uint16_t pc = trace->player
      ? (uint16_t)(trace->player->vm->context.t_pc & 0xffff) : 0;
  if (!capture_reads) return;
  address &= 0xffff;
  if (capture_all_reads) {
    /* Compare data consumed by the common resident engine, excluding its
     * instruction/operand fetches. Relocated wrappers intentionally execute
     * at different addresses and are not useful in this diagnostic. */
    if (0x43e0 <= pc && pc < 0x5739 &&
        !((uint16_t)(address - pc) <= 3))
      append(trace, 'R', address, value);
    return;
  }
  /* Contra's sequence fetch is LD A,(HL) at 6367H.  Ignore the instruction
   * fetch itself and compare the resulting byte stream independent of where
   * each per-song page relocated it. */
  if ((pc == 0x6368 && address != 0x6368) ||
      (pc == 0x2b9b && address != 0x2b9b) ||
      (pc == 0x4b9b && address != 0x4b9b) ||
      (pc == 0x0403 && address != 0x0403) ||
      (pc == 0x46f7 && address != 0x46f7) ||
      (pc == 0x52b5 && address != 0x52b5) ||
      (pc == 0x6393 && address != 0x6393) ||
      (pc == 0x081a && address != 0x081a) ||
      (pc == 0x081b && address != 0x081b) ||
      (pc == 0x081c && address != 0x081c) ||
      (pc == 0x67aa && address != 0x67aa) ||
      (pc == 0x67ab && address != 0x67ab) ||
      (pc == 0x67ac && address != 0x67ac))
    append(trace, 'R', address, value);
}

static int run_trace(const char *path, unsigned song, unsigned seconds,
                     TRACE *trace) {
  KSS *kss = KSS_load_file((char *)path);
  if (!kss) {
    fprintf(stderr, "%s: could not load KSS/KSP\n", path);
    return 0;
  }
  trace->player = KSSPLAY_new(RATE, 1, 16);
  trace->events = (EVENT *)calloc(MAX_EVENTS, sizeof(*trace->events));
  if (!trace->player || !trace->events) {
    fprintf(stderr, "out of memory\n");
    if (trace->player) KSSPLAY_delete(trace->player);
    free(trace->events);
    KSS_delete(kss);
    return 0;
  }
  KSSPLAY_set_data(trace->player, kss);
  KSSPLAY_set_iowrite_handler(trace->player, trace, io_write);
  KSSPLAY_set_memwrite_handler(trace->player, trace, memory_write);
  KSSPLAY_set_memread_handler(trace->player, trace, memory_read);
  KSSPLAY_reset(trace->player, song, 0);
  {
    const char *request = getenv("KSS_COMPARE_DUMP");
    if (request) {
      char *end;
      unsigned address = (unsigned)strtoul(request, &end, 0);
      unsigned length = (*end == ',') ? (unsigned)strtoul(end + 1, NULL, 0) : 32;
      unsigned index;
      fprintf(stderr, "%s %04X:", path, address & 0xffff);
      for (index = 0; index < length; index++)
        fprintf(stderr, " %02X", KSSPLAY_read_memory(
            trace->player, (address + index) & 0xffff));
      fputc('\n', stderr);
    }
  }
  KSSPLAY_calc_silent(trace->player, seconds * RATE);
  KSSPLAY_delete(trace->player);
  trace->player = NULL;
  KSS_delete(kss);
  if (trace->overflow) {
    fprintf(stderr, "%s: event buffer overflow\n", path);
    return 0;
  }
  return 1;
}

static void print_event(const char *label, size_t index, const EVENT *event) {
  fprintf(stderr, "%s[%zu] %c %04X=%02X pc=%04X hl=%04X de=%04X\n",
          label, index, event->kind, event->address, event->value, event->pc,
          event->hl, event->de);
}

int main(int argc, char **argv) {
  TRACE original = {0}, converted = {0};
  unsigned original_song, converted_song, seconds;
  size_t common, index;
  capture_reads = getenv("KSS_COMPARE_READS") != NULL;
  capture_all_reads = getenv("KSS_COMPARE_ALL_READS") != NULL;
  if (capture_all_reads) capture_reads = 1;
  if (argc != 6 && argc != 8 && argc != 9) {
    fprintf(stderr,
            "usage: %s ORIGINAL SONG CONVERTED SONG SECONDS "
            "[ORIGINAL_WORK_BASE CONVERTED_WORK_BASE [WORK_OFFSET]]\n",
            argv[0]);
    return 2;
  }
  original_song = (unsigned)strtoul(argv[2], NULL, 0);
  converted_song = (unsigned)strtoul(argv[4], NULL, 0);
  seconds = (unsigned)strtoul(argv[5], NULL, 0);
  if (argc >= 8) {
    original.work_base = (uint16_t)strtoul(argv[6], NULL, 0);
    converted.work_base = (uint16_t)strtoul(argv[7], NULL, 0);
    /* The largest Konami work area currently analysed is 0x299 bytes. */
    original.work_size = converted.work_size = 0x0299;
  }
  if (!seconds || !run_trace(argv[1], original_song, seconds, &original) ||
      !run_trace(argv[3], converted_song, seconds, &converted)) {
    free(original.events);
    free(converted.events);
    return 1;
  }
  if (argc == 9) {
    uint16_t work_offset = (uint16_t)strtoul(argv[8], NULL, 0);
    size_t cursor;
    for (cursor = 0; cursor < original.count; cursor++)
      if ((original.events[cursor].kind == 'W' ||
           original.events[cursor].kind == 'M') &&
          original.events[cursor].address == work_offset)
        print_event("original", cursor, &original.events[cursor]);
    for (cursor = 0; cursor < converted.count; cursor++)
      if ((converted.events[cursor].kind == 'W' ||
           converted.events[cursor].kind == 'M') &&
          converted.events[cursor].address == work_offset)
        print_event("converted", cursor, &converted.events[cursor]);
    free(original.events);
    free(converted.events);
    return 0;
  }
  common = original.count < converted.count ? original.count : converted.count;
  for (index = 0; index < common; index++) {
    const EVENT *a = &original.events[index];
    const EVENT *b = &converted.events[index];
    if (a->kind != b->kind ||
        (a->kind != 'R' && a->address != b->address) ||
        a->value != b->value)
      break;
  }
  if (index != common || original.count != converted.count) {
    size_t begin = index > 24 ? index - 24 : 0;
    size_t end = index + 25;
    size_t cursor;
    fprintf(stderr, "DIFF at %zu; original=%zu converted=%zu\n", index,
            original.count, converted.count);
    for (cursor = begin; cursor < end; cursor++) {
      if (cursor < original.count)
        print_event("original", cursor, &original.events[cursor]);
      if (cursor < converted.count)
        print_event("converted", cursor, &converted.events[cursor]);
    }
    free(original.events);
    free(converted.events);
    return 1;
  }
  printf("exact: %zu PSG/SCC%s writes over %u seconds\n", common,
         argc >= 8 ? "/work-state" : "", seconds);
  free(original.events);
  free(converted.events);
  return 0;
}
