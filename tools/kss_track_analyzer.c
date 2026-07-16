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

typedef struct {
  int id;
  int seconds;
  int fade_seconds;
  int loop;
  char title[256];
} TRACK;

typedef struct {
  KSS *kss;
  int current_page4;
  int current_page5;
  int page4[MAX_COMBINATIONS];
  int page5[MAX_COMBINATIONS];
  int combination_count;
  int bank16[MAX_COMBINATIONS];
  int bank16_count;
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
  if (trace->bank16_count < MAX_COMBINATIONS)
    trace->bank16[trace->bank16_count++] = bank;
}

static void memory_write(void *context, uint32_t address, uint32_t value) {
  TRACE *trace = (TRACE *)context;
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
  if (trace->kss && trace->kss->bank_mode == KSS_16K &&
      (address & 0xff) == 0xfe)
    add_16k_bank(trace, (int)value);
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
    TRACE trace;
    int samples;
    int j;
    if (!player) {
      fclose(out); KSS_delete(kss); return 1;
    }
    memset(&trace, 0, sizeof(trace));
    trace.kss = kss;
    KSSPLAY_set_data(player, kss);
    KSSPLAY_set_memwrite_handler(player, &trace, memory_write);
    KSSPLAY_set_iowrite_handler(player, &trace, io_write);
    KSSPLAY_reset(player, (uint32_t)tracks[i].id, 0);
    seed_current_banks(&trace, player);
    samples = tracks[i].seconds * 44100;
    KSSPLAY_calc_silent(player, (uint32_t)samples);
    fprintf(out, "track=%d\ntitle=%s\nseconds_analyzed=%d\nfade_seconds=%d\nloop=%s\n",
            tracks[i].id, tracks[i].title, tracks[i].seconds,
            tracks[i].fade_seconds, tracks[i].loop ? "yes" : "no");
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
      fprintf(out, "bank16_mapped_page4_page5=");
      for (j = 0; j < trace.bank16_count; j++) {
        if (j) fputc(',', out);
        fprintf(out, "(%d,%d)", trace.bank16[j] - kss->bank_offset,
                trace.bank16[j] - kss->bank_offset);
      }
      if (!trace.bank16_count) fprintf(out, "none");
      fputc('\n', out);
    }
    fputc('\n', out);
    KSSPLAY_delete(player);
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
  if (argc > 3) {
    fprintf(stderr, "usage: %s [DIRECTORY] [GAME]\n", argv[0]);
    return 2;
  }
  if (argc == 3) return analyze_game(directory, argv[2]);
  return analyze_directory(directory);
}
