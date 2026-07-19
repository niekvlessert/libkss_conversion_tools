#include "kssplay.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_TRACKS 256
#define MAX_COMBINATIONS 512
#define DEFAULT_SECONDS 5

static int analysis_seconds_cap;
static int probe_address = -1;
static int probe_length;

typedef struct {
  int id;
  int seconds;
  int fade_seconds;
  int loop;
  char title[256];
} TRACK;

typedef struct {
  KSS *kss;
  KSSPLAY *player;
  int current_page4;
  int current_page5;
  int page4[MAX_COMBINATIONS];
  int page5[MAX_COMBINATIONS];
  int combination_count;
  int bank16[MAX_COMBINATIONS];
  int bank16_pc[MAX_COMBINATIONS];
  int bank16_count;
  unsigned char executed[0x10000];
  unsigned char data_reads[0x10000];
  unsigned char memory_writes[0x10000];
  unsigned char loaded_reads[0x10000];
  unsigned char music_reads[0x10000];
  unsigned char stream_commands[0x10000];
  unsigned char *bank_reads;
  unsigned int bank_read_size;
  unsigned long scc_writes;
  unsigned long psg_writes;
  unsigned char scc_write_pcs[0x10000];
} TRACE;

static char *trim(char *text) {
  char *end;
  while (*text && isspace((unsigned char)*text)) text++;
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
  return text;
}

static void add_8k_combination(TRACE *trace, int page4, int page5) {
  int i;
  if (!trace->kss || trace->kss->bank_mode != KSS_8K ||
      page4 < trace->kss->bank_offset ||
      page4 >= trace->kss->bank_offset + trace->kss->bank_num ||
      page5 < trace->kss->bank_offset ||
      page5 >= trace->kss->bank_offset + trace->kss->bank_num)
    return;
  for (i = 0; i < trace->combination_count; i++)
    if (trace->page4[i] == page4 && trace->page5[i] == page5) return;
  if (trace->combination_count < MAX_COMBINATIONS) {
    i = trace->combination_count++;
    trace->page4[i] = page4;
    trace->page5[i] = page5;
  }
}

static void add_16k_bank(TRACE *trace, int bank) {
  int i;
  if (!trace->kss || trace->kss->bank_mode != KSS_16K ||
      bank < trace->kss->bank_offset ||
      bank >= trace->kss->bank_offset + trace->kss->bank_num)
    return;
  for (i = 0; i < trace->bank16_count; i++)
    if (trace->bank16[i] == bank) return;
  if (trace->bank16_count < MAX_COMBINATIONS) {
    trace->bank16[trace->bank16_count] = bank;
    trace->bank16_pc[trace->bank16_count] = trace->player
        ? (int)(trace->player->vm->context.t_pc & 0xffff) : -1;
    trace->bank16_count++;
  }
}

static void memory_write(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
  trace->memory_writes[address & 0xffff] = 1;
  if (0x9800 <= address && address <= 0x98ff) {
    trace->scc_writes++;
    if (trace->player)
      trace->scc_write_pcs[trace->player->vm->context.t_pc & 0xffff] = 1;
  }
  if (!trace->kss || trace->kss->bank_mode != KSS_8K) return;
  if (address == 0x9000) {
    trace->current_page4 = (int)value;
    add_8k_combination(trace, trace->current_page4, trace->current_page5);
  } else if (address == 0xB000) {
    trace->current_page5 = (int)value;
    add_8k_combination(trace, trace->current_page4, trace->current_page5);
  }
}

static void io_write(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
  if ((address & 0xff) == 0xa0 || (address & 0xff) == 0xa1)
    trace->psg_writes++;
  if (trace->kss && trace->kss->bank_mode == KSS_16K &&
      (address & 0xff) == 0xfe)
    add_16k_bank(trace, (int)value);
}

static void memory_read(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
  uint32_t pc;
  (void)value;
  if (!trace->player) return;
  pc = trace->player->vm->context.t_pc & 0xffff;
  trace->executed[pc] = 1;
  if (!(address >= pc && address <= ((pc + 4) & 0xffff)))
    trace->data_reads[address & 0xffff] = 1;

  if (trace->bank_reads && trace->kss && trace->kss->bank_num) {
    uint32_t page = address >> 13;
    uint32_t bank = trace->player->vm->mmap->current_bank[page];
    uint32_t bank_size = trace->kss->bank_mode == KSS_8K ? 0x2000 : 0x4000;
    if (trace->player->vm->mmap->current_slot[page] == VM_BANK_SLOT &&
        bank >= trace->kss->bank_offset &&
        bank < trace->kss->bank_offset + trace->kss->bank_num) {
      uint32_t index = bank - trace->kss->bank_offset;
      trace->bank_reads[index * bank_size + (address & (bank_size - 1))] = 1;
    }
  }

  /* Record reads from the statically loaded image, excluding the current
   * instruction and its longest possible immediate/prefix tail.  This is a
   * deliberately conservative dynamic hint: builders still verify every
   * resulting boundary against disassembly and all requested tracks. */
  if (trace->kss && address >= trace->kss->load_adr &&
      address < (uint32_t)trace->kss->load_adr + trace->kss->load_len &&
      !(address >= pc && address <= ((pc + 4) & 0xffff)))
    trace->loaded_reads[address] = 1;

  /* Contra and Space Manbow fetch sequence bytes at 6367H and 2B9AH;
   * after the memory read the VM PC points one byte further. Record only
   * bytes entering their command dispatchers. This distinguishes real
   * address operands from identical values in waveform/instrument payloads. */
  if (((pc == 0x6368 || pc == 0x2b9b || pc == 0x0403 || pc == 0x46f7) &&
       value >= 0xd0) || pc == 0x06fe)
    trace->stream_commands[address & 0xffff] = 1;

  if (address < 0x73c8 || address >= 0xc000) return;
  if ((0x6472 <= pc && pc < 0x73c8) || (0xc000 <= pc && pc < 0xc047))
    trace->music_reads[address] = 1;
}

static void print_bitmap_ranges(FILE *out, const char *label,
                                const unsigned char *bitmap,
                                uint32_t start, uint32_t end) {
  uint32_t address = start;
  int first = 1;
  fprintf(out, "%s=", label);
  while (address < end) {
    uint32_t range_start;
    while (address < end && !bitmap[address]) address++;
    if (address == end) break;
    range_start = address++;
    while (address < end && bitmap[address]) address++;
    fprintf(out, "%s%04X-%04X", first ? "" : ",", range_start, address);
    first = 0;
  }
  if (first) fprintf(out, "none");
  fputc('\n', out);
}

static void print_music_read_ranges(FILE *out, const TRACE *trace) {
  uint32_t address = 0x73c8;
  int first = 1;
  fprintf(out, "music_read_ranges=");
  while (address < 0xc000) {
    uint32_t start, end;
    while (address < 0xc000 && !trace->music_reads[address]) address++;
    if (address == 0xc000) break;
    start = address++;
    while (address < 0xc000 && trace->music_reads[address]) address++;
    end = address;
    fprintf(out, "%s%04X-%04X", first ? "" : ",", start, end);
    first = 0;
  }
  if (first) fprintf(out, "none");
  fputc('\n', out);
}

static void seed_current_banks(TRACE *trace, KSSPLAY *player) {
  if (trace->kss->bank_mode == KSS_8K) {
    trace->current_page4 = (int)player->vm->mmap->current_bank[4];
    trace->current_page5 = (int)player->vm->mmap->current_bank[5];
    add_8k_combination(trace, trace->current_page4, trace->current_page5);
  } else {
    add_16k_bank(trace, (int)player->vm->mmap->current_bank[4]);
  }
}

static int read_tracks(const char *path, TRACK *tracks) {
  FILE *file = fopen(path, "rb");
  char line[1024];
  int count = 0;
  if (!file) return 0;
  while (fgets(line, sizeof(line), file) && count < MAX_TRACKS) {
    char *first = trim(line);
    char *comma;
    char *second;
    char *third;
    char *fourth;
    char *fade;
    char *end;
    long id;
    if (!isdigit((unsigned char)*first)) continue;
    errno = 0;
    id = strtol(first, &end, 10);
    if (errno || id < 0 || id > 255 || *end != ',') continue;
    comma = end;
    second = strchr(comma + 1, ',');
    if (!second) continue;
    *second = 0;
    third = strchr(second + 1, ',');
    memset(&tracks[count], 0, sizeof(tracks[count]));
    if (third) {
      *third = 0;
      fade = third + 1;
      fourth = strchr(fade, ',');
      if (fourth) {
        *fourth = 0;
        tracks[count].loop = tolower((unsigned char)trim(fourth + 1)[0]) == 'y';
      }
      tracks[count].fade_seconds = atoi(trim(fade));
      if (tracks[count].fade_seconds < 0) tracks[count].fade_seconds = 0;
    }
    tracks[count].id = (int)id;
    tracks[count].seconds = atoi(trim(second + 1));
    if (tracks[count].seconds <= 0) tracks[count].seconds = DEFAULT_SECONDS;
    snprintf(tracks[count].title, sizeof(tracks[count].title), "%s", trim(comma + 1));
    count++;
  }
  fclose(file);
  return count;
}

static void print_list(FILE *out, const char *label, const int *values,
                       int count, int offset) {
  int i;
  fprintf(out, "%s=", label);
  for (i = 0; i < count; i++) {
    if (i) fputc(',', out);
    fprintf(out, "%d", values[i]);
  }
  if (!count) fprintf(out, "none");
  if (offset >= 0) {
    fprintf(out, " (source=");
    for (i = 0; i < count; i++) {
      if (i) fputc(',', out);
      fprintf(out, "%d", values[i] - offset);
    }
    fprintf(out, ")");
  }
  fputc('\n', out);
}

static int analyze_one(const char *directory, const char *stem,
                       const char *kss_path, const char *track_path,
                       const char *output_path) {
  TRACK tracks[MAX_TRACKS];
  int track_count = read_tracks(track_path, tracks);
  FILE *out;
  KSS *kss;
  int i;

  if (!track_count) {
    fprintf(stderr, "%s: no real tracks found in %s\n", stem, track_path);
    return 1;
  }
  kss = KSS_load_file((char *)kss_path);
  if (!kss) {
    fprintf(stderr, "%s: could not load KSS\n", kss_path);
    return 1;
  }
  out = fopen(output_path, "wb");
  if (!out) {
    fprintf(stderr, "%s: could not create %s\n", stem, output_path);
    KSS_delete(kss);
    return 1;
  }
  fprintf(out, "source=%s\nformat=%s\ntrackinfo=%s\n", kss_path,
          kss->kssx ? "KSSX" : "KSCC", track_path);
  fprintf(out, "bank_mode=%s\nbank_offset=%u\nbank_count=%u\nbank_size=0x%04X\n",
          kss->bank_mode == KSS_8K ? "8K" : "16K", kss->bank_offset,
          kss->bank_num, kss->bank_mode == KSS_8K ? 0x2000 : 0x4000);
  fprintf(out, "analysis_method=libkss_emulation\n");
  fprintf(out, "selector_note=bank selectors are relative to bank_offset\n");
  fprintf(out, "analysis_seconds_default=%d\n", DEFAULT_SECONDS);
  fprintf(out, "track_count=%d\n", track_count);
  fprintf(out, "\n[tracks]\n");

  for (i = 0; i < track_count; i++) {
    KSSPLAY *player = KSSPLAY_new(44100, 1, 16);
    KSS *track_kss = KSS_load_file((char *)kss_path);
    TRACE trace;
    int samples;
    int j;
    unsigned int reset_pc, reset_sp;
    if (!player || !track_kss) {
      if (player) KSSPLAY_delete(player);
      if (track_kss) KSS_delete(track_kss);
      fclose(out); KSS_delete(kss); return 1;
    }
    memset(&trace, 0, sizeof(trace));
    trace.kss = track_kss;
    trace.player = player;
    trace.bank_read_size = (track_kss->bank_mode == KSS_8K ? 0x2000 : 0x4000);
    if (track_kss->bank_num)
      trace.bank_reads = calloc(track_kss->bank_num, trace.bank_read_size);
    if (track_kss->bank_num && !trace.bank_reads) {
      KSSPLAY_delete(player); KSS_delete(track_kss);
      fclose(out); KSS_delete(kss); return 1;
    }
    KSSPLAY_set_data(player, track_kss);
    KSSPLAY_set_memwrite_handler(player, &trace, memory_write);
    KSSPLAY_set_memread_handler(player, &trace, memory_read);
    KSSPLAY_set_iowrite_handler(player, &trace, io_write);
    KSSPLAY_reset(player, (uint32_t)tracks[i].id, 0);
    reset_pc = player->vm->context.pc & 0xffff;
    reset_sp = player->vm->context.sp & 0xffff;
    seed_current_banks(&trace, player);
    if (analysis_seconds_cap > 0 && tracks[i].seconds > analysis_seconds_cap)
      tracks[i].seconds = analysis_seconds_cap;
    samples = tracks[i].seconds * 44100;
    KSSPLAY_calc_silent(player, (uint32_t)samples);
    fprintf(out, "track=%d\ntitle=%s\nseconds_analyzed=%d\nfade_seconds=%d\nloop=%s\n",
            tracks[i].id, tracks[i].title, tracks[i].seconds,
            tracks[i].fade_seconds, tracks[i].loop ? "yes" : "no");
    fprintf(out, "page1_physical_bank=%u\n",
            player->vm->mmap->current_bank[2]);
    fprintf(out, "reset_pc=%04X\nreset_sp=%04X\nfinal_pc=%04X\nfinal_sp=%04X\n",
            reset_pc, reset_sp, player->vm->context.pc & 0xffff,
            player->vm->context.sp & 0xffff);
    if (probe_address >= 0) {
      fprintf(out, "memory_probe=%04X:", probe_address);
      for (j = 0; j < probe_length; j++)
        fprintf(out, "%02X", MMAP_read_memory(
            player->vm->mmap, (probe_address + j) & 0xffff));
      fputc('\n', out);
    }
    fprintf(out, "complete_page_map=%u,%u\n",
            player->quarth_song_page[tracks[i].id & 0xff],
            player->quarth_song_id[tracks[i].id & 0xff]);
    if (kss->bank_mode == KSS_8K) {
      int page4[MAX_COMBINATIONS];
      int page5[MAX_COMBINATIONS];
      int page4_count = 0;
      int page5_count = 0;
      for (j = 0; j < trace.combination_count; j++) {
        int k;
        for (k = 0; k < page4_count && page4[k] != trace.page4[j]; k++) {}
        if (k == page4_count) page4[page4_count++] = trace.page4[j];
        for (k = 0; k < page5_count && page5[k] != trace.page5[j]; k++) {}
        if (k == page5_count) page5[page5_count++] = trace.page5[j];
      }
      print_list(out, "page4_selectors", page4, page4_count, kss->bank_offset);
      print_list(out, "page5_selectors", page5, page5_count, kss->bank_offset);
      fprintf(out, "page_combinations=");
      for (j = 0; j < trace.combination_count; j++) {
        if (j) fputc(',', out);
        fprintf(out, "(%d,%d)", trace.page4[j] - kss->bank_offset,
                trace.page5[j] - kss->bank_offset);
      }
      if (!trace.combination_count) fprintf(out, "none");
      fputc('\n', out);
    } else {
      print_list(out, "bank16_selectors", trace.bank16,
                 trace.bank16_count, kss->bank_offset);
      fprintf(out, "bank16_selector_pcs=");
      for (j = 0; j < trace.bank16_count; j++)
        fprintf(out, "%s%04X", j ? "," : "", trace.bank16_pc[j] & 0xffff);
      if (!trace.bank16_count) fprintf(out, "none");
      fputc('\n', out);
      fprintf(out, "bank16_mapped_page4_page5=");
      for (j = 0; j < trace.bank16_count; j++) {
        if (j) fputc(',', out);
        fprintf(out, "(%d,%d)", trace.bank16[j] - kss->bank_offset,
                trace.bank16[j] - kss->bank_offset);
      }
      if (!trace.bank16_count) fprintf(out, "none");
      fputc('\n', out);
    }
    print_bitmap_ranges(out, "executed_ranges", trace.executed,
                        track_kss->load_adr,
                        (uint32_t)track_kss->load_adr + track_kss->load_len);
    print_bitmap_ranges(out, "executed_all_ranges", trace.executed,
                        0, 0x10000);
    print_bitmap_ranges(out, "data_read_ranges", trace.data_reads,
                        0, 0x10000);
    print_bitmap_ranges(out, "memory_write_ranges", trace.memory_writes,
                        0, 0x10000);
    print_bitmap_ranges(out, "loaded_data_read_ranges", trace.loaded_reads,
                        track_kss->load_adr,
                        (uint32_t)track_kss->load_adr + track_kss->load_len);
    print_bitmap_ranges(out, "stream_command_ranges", trace.stream_commands,
                        track_kss->load_adr,
                        (uint32_t)track_kss->load_adr + track_kss->load_len);
    for (j = 0; j < track_kss->bank_num; j++) {
      char label[64];
      snprintf(label, sizeof(label), "bank_data_read_ranges_%d", j);
      print_bitmap_ranges(out, label,
                          trace.bank_reads + j * trace.bank_read_size,
                          0, trace.bank_read_size);
    }
    print_music_read_ranges(out, &trace);
    fprintf(out, "scc_writes=%lu\npsg_writes=%lu\n",
            trace.scc_writes, trace.psg_writes);
    print_bitmap_ranges(out, "scc_write_pc_ranges", trace.scc_write_pcs,
                        0, 0x10000);
    fputc('\n', out);
    KSSPLAY_delete(player);
    free(trace.bank_reads);
    KSS_delete(track_kss);
  }
  fclose(out);
  KSS_delete(kss);
  printf("analyzed %s (%d tracks)\n", stem, track_count);
  (void)directory;
  return 0;
}

static int has_suffix(const char *name, const char *suffix) {
  size_t name_len = strlen(name);
  size_t suffix_len = strlen(suffix);
  return name_len >= suffix_len &&
         strcasecmp(name + name_len - suffix_len, suffix) == 0;
}

static int ensure_extracted_directory(const char *directory) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/extracted", directory);
  if (mkdir(path, 0777) == 0 || errno == EEXIST) return 0;
  fprintf(stderr, "%s: could not create %s: %s\n", directory, path,
          strerror(errno));
  return 1;
}

static int analyze_game(const char *directory, const char *stem) {
  char kss_path[1024], track_path[1024], output_path[1024];
  if (ensure_extracted_directory(directory)) return 1;
  snprintf(kss_path, sizeof(kss_path), "%s/%s.kss", directory, stem);
  snprintf(track_path, sizeof(track_path), "%s/%s.trackinfo", directory, stem);
  snprintf(output_path, sizeof(output_path), "%s/extracted/%s.track_extract",
           directory, stem);
  return analyze_one(directory, stem, kss_path, track_path, output_path);
}

static int analyze_directory(const char *directory) {
  DIR *dir = opendir(directory);
  struct dirent *entry;
  int found = 0;
  int failures = 0;
  if (!dir) {
    fprintf(stderr, "%s: %s\n", directory, strerror(errno));
    return 1;
  }
  while ((entry = readdir(dir)) != NULL) {
    char stem[256];
    size_t len;
    if (!has_suffix(entry->d_name, ".kss")) continue;
    len = strlen(entry->d_name) - 4;
    if (!len || len >= sizeof(stem)) continue;
    memcpy(stem, entry->d_name, len);
    stem[len] = 0;
    found = 1;
    if (analyze_game(directory, stem)) failures = 1;
  }
  closedir(dir);
  if (!found) {
    fprintf(stderr, "%s: no .kss files found\n", directory);
    return 1;
  }
  return failures;
}

int main(int argc, char **argv) {
  const char *directory = argc > 1 ? argv[1] : "vigamup";
  const char *cap = getenv("KSS_ANALYSIS_SECONDS");
  const char *probe = getenv("KSS_ANALYSIS_PROBE");
  if (cap && *cap) {
    analysis_seconds_cap = atoi(cap);
    if (analysis_seconds_cap < 1) {
      fprintf(stderr, "KSS_ANALYSIS_SECONDS must be a positive integer\n");
      return 2;
    }
  }
  if (probe && *probe &&
      sscanf(probe, "%x:%d", &probe_address, &probe_length) != 2) {
    fprintf(stderr, "KSS_ANALYSIS_PROBE must be HEX_ADDRESS:LENGTH\n");
    return 2;
  }
  if (probe_length < 0 || probe_length > 256) {
    fprintf(stderr, "KSS_ANALYSIS_PROBE length must be 0..256\n");
    return 2;
  }
  if (argc > 3) {
    fprintf(stderr, "usage: %s [DIRECTORY] [GAME]\n", argv[0]);
    return 2;
  }
  if (argc == 3) return analyze_game(directory, argv[2]);
  return analyze_directory(directory);
}
