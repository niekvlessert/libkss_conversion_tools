/*
 * Build a 1 MB Konami-style SCC cartridge ROM around a merged 8K KSSX file.
 *
 * The cartridge boot code copies the KSSX main image into MSX RAM, keeps the
 * KSS bank area in the cartridge at selectors 6.., and then maps the RAM slot
 * over the cartridge's 4000H-7FFFH window.  This lets the existing KSSX engine
 * code run at its original addresses while the cartridge supplies the banked
 * music data and SCC mapper.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE 0x100000
#define ROM_BANK_SIZE 0x2000
#define ROM_BANK_COUNT (ROM_SIZE / ROM_BANK_SIZE)
#define KSS_HEADER_SIZE 0x20
#define KSS_BANK_SIZE 0x2000
#define KSS_BANK_OFFSET 6
#define KSS_MAX_TRACKS 256
#define KSS_LOAD_ADDRESS 0x0200
#define KSS_LOAD_SIZE 0xFE00
#define KSS_MAIN_BANK 85
#define CARTRIDGE_HEADER_SIZE 0x10
#define CARTRIDGE_INIT_ADDRESS 0x4010
#define TITLE_BANK 1
#define TITLE_SIZE 24
#define TITLE_TABLE_SIZE (KSS_MAX_TRACKS * TITLE_SIZE)
#define TITLE_STRING_OFFSET 0x1900
#define MENU_ADDRESS 0xFE00
#define MENU_STACK 0xFFFE
#define MENU_TRACK 0xFF80
#define MENU_DIGITS 0xFF81
#define MENU_VALUE 0xFF82
#define MENU_STATE 0xFF83
#define MENU_TEMP 0xFF84
#define BIOS_ENASLT 0x0024
#define BIOS_CHSNS 0x009C
#define BIOS_CHGET 0x009F
#define BIOS_CHPUT 0x00A2
#define BIOS_CLS 0x00C3

enum {
  LABEL_MENU_ENTRY,
  LABEL_MENU_SHOW,
  LABEL_MENU_INPUT,
  LABEL_START_TRACK,
  LABEL_ACCEPT_DIGITS,
  LABEL_PLAY_LOOP,
  LABEL_PLAY_WAIT,
  LABEL_PRINT_Z,
  LABEL_PRINT_3,
  LABEL_PRINT_3_HUNDREDS,
  LABEL_PRINT_3_HUNDREDS_DONE,
  LABEL_PRINT_3_TENS,
  LABEL_PRINT_3_TENS_DONE,
  LABEL_SHOW_TITLE,
  LABEL_TITLE_OFFSET,
  LABEL_TITLE_OFFSET_DONE,
  LABEL_TITLE_CHARS,
  LABEL_STORE_DIGIT,
  LABEL_COUNT
};

typedef struct {
  uint8_t data[1024];
  uint32_t length;
  uint16_t origin;
  uint16_t labels[LABEL_COUNT];
  uint8_t label_set[LABEL_COUNT];
} CODE;

typedef struct {
  uint8_t type;
  uint32_t offset;
  uint8_t label;
} PATCH;

static PATCH patches[128];
static uint32_t patch_count;

static uint16_t get_word(const uint8_t *data, uint32_t offset) {
  return (uint16_t)(data[offset] | ((uint16_t)data[offset + 1] << 8));
}

static uint32_t get_dword(const uint8_t *data, uint32_t offset) {
  return (uint32_t)data[offset] |
         ((uint32_t)data[offset + 1] << 8) |
         ((uint32_t)data[offset + 2] << 16) |
         ((uint32_t)data[offset + 3] << 24);
}

static void set_word(uint8_t *data, uint32_t offset, uint16_t value) {
  data[offset] = (uint8_t)value;
  data[offset + 1] = (uint8_t)(value >> 8);
}

static uint8_t *read_file(const char *path, uint32_t *size_out) {
  FILE *file;
  long size;
  uint8_t *data;

  file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  data = (uint8_t *)malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size_out = (uint32_t)size;
  return data;
}

static void emit(CODE *code, uint8_t value) {
  if (code->length >= sizeof(code->data)) {
    fprintf(stderr, "menu code exceeds builder buffer\n");
    exit(1);
  }
  code->data[code->length++] = value;
}

static void emit_word(CODE *code, uint16_t value) {
  emit(code, (uint8_t)value);
  emit(code, (uint8_t)(value >> 8));
}

static void label(CODE *code, uint8_t number) {
  code->labels[number] = (uint16_t)(code->origin + code->length);
  code->label_set[number] = 1;
}

static void patch_label(CODE *code, uint8_t type, uint8_t number) {
  if (patch_count >= sizeof(patches) / sizeof(patches[0])) {
    fprintf(stderr, "too many menu label patches\n");
    exit(1);
  }
  patches[patch_count].type = type;
  patches[patch_count].offset = code->length;
  patches[patch_count].label = number;
  patch_count++;
  emit(code, 0);
  emit(code, 0);
}

static void emit_jp(CODE *code, uint8_t opcode, uint8_t number) {
  emit(code, opcode);
  patch_label(code, 0, number);
}

static void emit_call(CODE *code, uint16_t address) {
  emit(code, 0xCD);
  emit_word(code, address);
}

static void emit_call_label(CODE *code, uint8_t number) {
  emit_jp(code, 0xCD, number);
}

static void emit_djnz(CODE *code, uint8_t number) {
  emit(code, 0x10);
  if (patch_count >= sizeof(patches) / sizeof(patches[0])) {
    fprintf(stderr, "too many menu label patches\n");
    exit(1);
  }
  patches[patch_count].type = 1;
  patches[patch_count].offset = code->length;
  patches[patch_count].label = number;
  patch_count++;
  emit(code, 0);
}

static void emit_ld_a_mem(CODE *code, uint16_t address) {
  emit(code, 0x3A);
  emit_word(code, address);
}

static void emit_ld_mem_a(CODE *code, uint16_t address) {
  emit(code, 0x32);
  emit_word(code, address);
}

static void emit_ld_hl(CODE *code, uint16_t value) {
  emit(code, 0x21);
  emit_word(code, value);
}

static void emit_ld_de(CODE *code, uint16_t value) {
  emit(code, 0x11);
  emit_word(code, value);
}

static void emit_ld_sp(CODE *code, uint16_t value) {
  emit(code, 0x31);
  emit_word(code, value);
}

static void finalize_code(CODE *code) {
  uint32_t i;
  for (i = 0; i < patch_count; i++) {
    PATCH *patch = &patches[i];
    uint16_t target;
    int32_t displacement;

    if (!code->label_set[patch->label]) {
      fprintf(stderr, "unresolved menu label %u\n", patch->label);
      exit(1);
    }
    target = code->labels[patch->label];
    if (patch->type == 0) {
      code->data[patch->offset] = (uint8_t)target;
      code->data[patch->offset + 1] = (uint8_t)(target >> 8);
    } else {
      displacement = (int32_t)target -
                    (int32_t)(code->origin + patch->offset + 1);
      if (displacement < -128 || displacement > 127) {
        fprintf(stderr, "menu DJNZ target is out of range\n");
        exit(1);
      }
      code->data[patch->offset] = (uint8_t)displacement;
      /* The second byte is not part of a relative patch. */
    }
  }
}

static void build_menu(CODE *menu, uint16_t origin, uint16_t play_address) {
  memset(menu, 0, sizeof(*menu));
  menu->origin = origin;
  patch_count = 0;

  label(menu, LABEL_MENU_ENTRY);
  emit(menu, 0xF3); /* DI */
  emit(menu, 0xED); /* IM 1 */
  emit(menu, 0x56);
  emit_ld_sp(menu, MENU_STACK);
  emit_ld_a_mem(menu, MENU_STATE);
  emit(menu, 0xB7);
  emit_jp(menu, 0xC2, LABEL_PLAY_LOOP);
  emit_jp(menu, 0xC3, LABEL_MENU_SHOW);

  label(menu, LABEL_MENU_SHOW);
  emit(menu, 0xF3);
  emit_call(menu, BIOS_CLS);
  emit(menu, 0x3E);
  emit(menu, 1);
  emit_ld_mem_a(menu, 0x9000); /* title bank */
  emit_ld_hl(menu, (uint16_t)(0x8000 + TITLE_STRING_OFFSET));
  emit_call_label(menu, LABEL_PRINT_Z);
  emit_ld_hl(menu, (uint16_t)(0x8000 + TITLE_STRING_OFFSET + 32));
  emit_call_label(menu, LABEL_PRINT_Z);
  emit_ld_a_mem(menu, MENU_TRACK);
  emit_call_label(menu, LABEL_PRINT_3);
  emit_ld_hl(menu, (uint16_t)(0x8000 + TITLE_STRING_OFFSET + 64));
  emit_call_label(menu, LABEL_PRINT_Z);
  emit_call_label(menu, LABEL_SHOW_TITLE);
  emit(menu, 0xAF);
  emit_ld_mem_a(menu, MENU_DIGITS);
  emit_ld_mem_a(menu, MENU_VALUE);
  emit(menu, 0xFB); /* EI */

  label(menu, LABEL_MENU_INPUT);
  emit_call(menu, BIOS_CHGET);
  emit(menu, 0xFE);
  emit(menu, 0x1B); /* Escape */
  emit_jp(menu, 0xCA, LABEL_MENU_SHOW);
  emit(menu, 0xFE);
  emit(menu, 0x20); /* Space */
  emit_jp(menu, 0xCA, LABEL_START_TRACK);
  emit(menu, 0xFE);
  emit(menu, 0x0D); /* Return */
  emit_jp(menu, 0xCA, LABEL_ACCEPT_DIGITS);
  emit(menu, 0xFE);
  emit(menu, '0');
  emit_jp(menu, 0xDA, LABEL_MENU_INPUT);
  emit(menu, 0xFE);
  emit(menu, ':');
  emit_jp(menu, 0xD2, LABEL_MENU_INPUT);
  emit(menu, 0xD6); /* digit = A - '0' */
  emit(menu, '0');
  emit(menu, 0x5F); /* E = digit */
  emit_ld_a_mem(menu, MENU_DIGITS);
  emit(menu, 0x3C);
  emit(menu, 0xFE);
  emit(menu, 4);
  emit_jp(menu, 0xDA, LABEL_MENU_INPUT);
  emit_ld_mem_a(menu, MENU_DIGITS);
  emit_ld_a_mem(menu, MENU_VALUE);
  emit(menu, 0x47); /* B = old value */
  emit(menu, 0x87); /* 2x */
  emit(menu, 0x87); /* 4x */
  emit(menu, 0x80); /* 5x */
  emit(menu, 0x87); /* 10x */
  emit(menu, 0x83); /* + digit */
  emit(menu, 0xFE);
  emit(menu, 252);
  emit_jp(menu, 0xDA, LABEL_STORE_DIGIT);
  emit(menu, 0x3E);
  emit(menu, 251);
  label(menu, LABEL_STORE_DIGIT);
  emit_ld_mem_a(menu, MENU_VALUE);
  emit_jp(menu, 0xC3, LABEL_MENU_INPUT);

  label(menu, LABEL_ACCEPT_DIGITS);
  emit_ld_a_mem(menu, MENU_DIGITS);
  emit(menu, 0xB7);
  emit_jp(menu, 0xCA, LABEL_MENU_INPUT);
  emit_ld_a_mem(menu, MENU_VALUE);
  emit_ld_mem_a(menu, MENU_TRACK);
  emit_jp(menu, 0xC3, LABEL_START_TRACK);

  label(menu, LABEL_START_TRACK);
  emit(menu, 0x3E);
  emit(menu, 1);
  emit_ld_mem_a(menu, MENU_STATE);
  emit_ld_a_mem(menu, MENU_TRACK);
  emit(menu, 0x11);
  emit_word(menu, 0);
  emit_ld_sp(menu, 0xF37A);
  emit(menu, 0xC3);
  emit_word(menu, 0xF000);

  label(menu, LABEL_PLAY_LOOP);
  emit_ld_sp(menu, MENU_STACK);
  emit(menu, 0xFB);
  label(menu, LABEL_PLAY_WAIT);
  emit(menu, 0x76); /* HALT until the next VBlank */
  emit(menu, 0xF3);
  emit_call(menu, play_address);
  emit_call(menu, BIOS_CHSNS);
  emit_jp(menu, 0xCA, LABEL_PLAY_WAIT);
  emit_call(menu, BIOS_CHGET);
  emit(menu, 0xAF);
  emit_ld_mem_a(menu, MENU_STATE);
  emit_jp(menu, 0xC3, LABEL_MENU_SHOW);

  label(menu, LABEL_PRINT_Z);
  emit(menu, 0x7E);
  emit(menu, 0xB7);
  emit(menu, 0xC8); /* RET Z */
  emit(menu, 0x23);
  emit(menu, 0xE5);
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0xE1);
  emit_jp(menu, 0xC3, LABEL_PRINT_Z);

  label(menu, LABEL_PRINT_3);
  emit(menu, 0x47); /* B = remaining value */
  emit(menu, 0xAF);
  emit(menu, 0x4F); /* C = hundreds */
  label(menu, LABEL_PRINT_3_HUNDREDS);
  emit(menu, 0x78);
  emit(menu, 0xFE);
  emit(menu, 100);
  emit_jp(menu, 0xDA, LABEL_PRINT_3_HUNDREDS_DONE);
  emit(menu, 0xD6);
  emit(menu, 100);
  emit(menu, 0x47); /* B = new remainder */
  emit(menu, 0x0C);
  emit_jp(menu, 0xC3, LABEL_PRINT_3_HUNDREDS);
  label(menu, LABEL_PRINT_3_HUNDREDS_DONE);
  emit(menu, 0x79); /* A = hundreds */
  emit(menu, 0x3E);
  emit(menu, '0');
  emit(menu, 0x81); /* A = hundreds + '0' */
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0x78);
  emit(menu, 0x47); /* B = remainder */
  emit(menu, 0xAF);
  emit(menu, 0x4F); /* C = tens */
  label(menu, LABEL_PRINT_3_TENS);
  emit(menu, 0x78);
  emit(menu, 0xFE);
  emit(menu, 10);
  emit_jp(menu, 0xDA, LABEL_PRINT_3_TENS_DONE);
  emit(menu, 0xD6);
  emit(menu, 10);
  emit(menu, 0x47); /* B = new remainder */
  emit(menu, 0x0C);
  emit_jp(menu, 0xC3, LABEL_PRINT_3_TENS);
  label(menu, LABEL_PRINT_3_TENS_DONE);
  emit(menu, 0x79); /* A = tens */
  emit(menu, 0x3E);
  emit(menu, '0');
  emit(menu, 0x81); /* A = tens + '0' */
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0x78); /* A = units */
  emit(menu, 0x3E);
  emit(menu, '0');
  emit(menu, 0x80); /* A = units + '0' */
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0xC9);

  label(menu, LABEL_SHOW_TITLE);
  emit_ld_hl(menu, 0x8000);
  emit_ld_a_mem(menu, MENU_TRACK);
  emit(menu, 0x47); /* B = track */
  emit_ld_de(menu, TITLE_SIZE);
  label(menu, LABEL_TITLE_OFFSET);
  emit(menu, 0x78);
  emit(menu, 0xB7);
  emit_jp(menu, 0xCA, LABEL_TITLE_OFFSET_DONE);
  emit(menu, 0x19);
  emit(menu, 0x05);
  emit_jp(menu, 0xC3, LABEL_TITLE_OFFSET);
  label(menu, LABEL_TITLE_OFFSET_DONE);
  emit(menu, 0x06);
  emit(menu, TITLE_SIZE);
  label(menu, LABEL_TITLE_CHARS);
  emit(menu, 0x7E);
  emit(menu, 0x23);
  emit(menu, 0xE5);
  emit(menu, 0xC5);
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0xC1);
  emit(menu, 0xE1);
  emit_djnz(menu, LABEL_TITLE_CHARS);
  emit(menu, 0x3E);
  emit(menu, 13);
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0x3E);
  emit(menu, 10);
  emit_call(menu, BIOS_CHPUT);
  emit(menu, 0xC9);

  finalize_code(menu);
  if (menu->length > 0x180) {
    fprintf(stderr, "menu code/data is %u bytes; high RAM budget is 384 bytes\n",
            (unsigned)menu->length);
    exit(1);
  }
}

static uint32_t emit_loader(uint8_t *loader, uint16_t menu_target) {
  uint32_t p = 0;
  uint32_t i;

#define B(v) loader[p++] = (uint8_t)(v)
#define W(v) do { uint16_t _v = (uint16_t)(v); B(_v); B(_v >> 8); } while (0)

  B(0xF3); /* DI */
  B(0x31); W(MENU_STACK);

  for (i = 0; i < 8; i++) {
    B(0x3E); B(KSS_MAIN_BANK + i);
    B(0x32); W(0x9000);
    B(0x21); W(0x8000);
    B(0x11); W(KSS_LOAD_ADDRESS + i * 0x2000);
    B(0x01); W(i == 7 ? 0x1E00 : 0x2000);
    B(0xED); B(0xB0);
  }

  B(0x3E); B(0xC3);
  B(0x32); W(0xF37C);
  B(0x21); W(MENU_ADDRESS);
  B(0x7D);
  B(0x32); W(0xF37D);
  B(0x7C);
  B(0x32); W(0xF37E);
  B(0xAF);
  B(0x32); W(MENU_STATE);
  B(0x32); W(MENU_TRACK);
  B(0xC3); W(menu_target);

#undef B
#undef W
  return p;
}

static uint32_t emit_boot(uint8_t *boot, uint16_t menu_source, uint16_t menu_size) {
  uint32_t p = 0;

#define B(v) boot[p++] = (uint8_t)(v)
#define W(v) do { uint16_t _v = (uint16_t)(v); B(_v); B(_v >> 8); } while (0)

  B(0xF3); /* DI */
  B(0x31); W(MENU_STACK);
  B(0x21); W(menu_source);
  B(0x11); W(MENU_ADDRESS);
  B(0x01); W(menu_size);
  B(0xED); B(0xB0); /* copy loader and menu into high RAM */

  /* Replace the cartridge's 4000H-7FFFH mapping with MSX RAM.  Jumping
   * directly into ENASLT lets its RET return to the RAM loader after the
   * mapping has changed, so no instruction is fetched from the cartridge. */
  B(0x3A); W(0xF342); /* RAM_PAGE1: RAM slot for 4000H-7FFFH */
  B(0x21); W(MENU_ADDRESS);
  B(0xE5); /* return address for ENASLT */
  B(0x21); W(0x4000);
  B(0xC3); W(BIOS_ENASLT);

#undef B
#undef W
  return p;
}

static void make_titles(uint8_t *titles, const uint8_t *kss, uint32_t kss_size,
                        uint32_t track_count) {
  uint32_t info;
  uint32_t count;
  uint32_t i;
  uint32_t offset;

  memset(titles, ' ', TITLE_TABLE_SIZE);
  for (i = 0; i < track_count; i++) {
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "Track %03u", (unsigned)i);
    memcpy(titles + i * TITLE_SIZE, fallback, strlen(fallback));
  }

  info = 0x10 + kss[0x0E] + get_dword(kss, 0x10);
  if (info + 10 > kss_size || memcmp(kss + info, "INFO", 4) != 0)
    return;
  count = get_word(kss, info + 8);
  offset = info + 0x10;
  for (i = 0; i < count && offset < kss_size; i++) {
    uint32_t song;
    uint32_t j;
    song = kss[offset++];
    offset++; /* type */
    offset += 8; /* time and fade */
    if (song >= track_count)
      break;
    for (j = 0; j < TITLE_SIZE && offset < kss_size; j++) {
      uint8_t c = kss[offset++];
      if (c == 0)
        break;
      if (c >= 0x20 && c < 0x7F)
        titles[song * TITLE_SIZE + j] = c;
    }
    while (offset < kss_size && kss[offset - 1] != 0)
      offset++;
  }
}

static void make_rom(const char *input, const char *output) {
  uint8_t *kss;
  uint8_t *rom;
  uint8_t titles[TITLE_STRING_OFFSET + 256];
  uint32_t kss_size;
  uint32_t bank_count;
  uint32_t bank_data;
  uint32_t i;
  uint32_t track_count;
  uint16_t play_address;
  CODE menu;
  uint8_t loader[256];
  uint8_t boot[1024];
  uint32_t loader_length;
  uint32_t boot_length;
  uint32_t combined_size;
  uint16_t menu_origin;
  uint32_t menu_source;
  FILE *file;

  kss = read_file(input, &kss_size);
  if (!kss) {
    fprintf(stderr, "could not read %s\n", input);
    exit(1);
  }
  if (kss_size < KSS_HEADER_SIZE || memcmp(kss, "KSSX", 4) != 0 ||
      get_word(kss, 4) != KSS_LOAD_ADDRESS || get_word(kss, 6) != KSS_LOAD_SIZE ||
      (kss[0x0D] & 0x80) == 0 || kss[0x0C] != KSS_BANK_OFFSET) {
    fprintf(stderr, "%s is not the expected merged 8K KSSX archive\n", input);
    free(kss);
    exit(1);
  }
  bank_count = kss[0x0D] & 0x7F;
  bank_data = KSS_HEADER_SIZE + KSS_LOAD_SIZE;
  if (bank_count > 79 || bank_data + bank_count * KSS_BANK_SIZE > kss_size) {
    fprintf(stderr, "%s has unsupported KSS bank data\n", input);
    free(kss);
    exit(1);
  }
  play_address = get_word(kss, 0x0A);
  track_count = (uint32_t)get_word(kss, 0x1A) + 1;
  if (track_count > KSS_MAX_TRACKS)
    track_count = KSS_MAX_TRACKS;

  memset(titles, ' ', sizeof(titles));
  make_titles(titles, kss, kss_size, track_count);
  memcpy(titles + TITLE_STRING_OFFSET, "VIGAMUP SCC PLAYER\r\n\r\n",
         strlen("VIGAMUP SCC PLAYER\r\n\r\n"));
  titles[TITLE_STRING_OFFSET + strlen("VIGAMUP SCC PLAYER\r\n\r\n")] = 0;
  memcpy(titles + TITLE_STRING_OFFSET + 32, "Track ", strlen("Track "));
  titles[TITLE_STRING_OFFSET + 32 + strlen("Track ")] = 0;
  memcpy(titles + TITLE_STRING_OFFSET + 64, "\r\nTitle: ", strlen("\r\nTitle: "));
  titles[TITLE_STRING_OFFSET + 64 + strlen("\r\nTitle: ")] = 0;

  /* The loader and menu are copied together into high RAM before the
   * cartridge's 4000H-7FFFH window is replaced by normal MSX RAM.  The menu
   * starts immediately after the loader, so its absolute labels are known
   * before either stage is emitted. */
  loader_length = emit_loader(loader, 0);
  menu_origin = (uint16_t)(MENU_ADDRESS + loader_length);
  build_menu(&menu, menu_origin, play_address);
  loader_length = emit_loader(loader, menu.labels[LABEL_MENU_ENTRY]);
  combined_size = loader_length + menu.length;
  memset(boot, 0xC9, sizeof(boot));
  boot_length = emit_boot(boot, 0, (uint16_t)combined_size);
  menu_source = CARTRIDGE_INIT_ADDRESS + boot_length;
  /* Re-emit with the actual source address now that the boot length is known. */
  boot_length = emit_boot(boot, (uint16_t)menu_source, (uint16_t)combined_size);

  if (CARTRIDGE_HEADER_SIZE + boot_length + combined_size > ROM_BANK_SIZE) {
    fprintf(stderr, "boot/menu do not fit in the first 8K cartridge bank\n");
    free(kss);
    exit(1);
  }

  rom = (uint8_t *)malloc(ROM_SIZE);
  if (!rom) {
    free(kss);
    exit(1);
  }
  memset(rom, 0xFF, ROM_SIZE);
  /* Cartridge header: AB followed by the initialization entry point. */
  rom[0] = 'A';
  rom[1] = 'B';
  set_word(rom, 2, CARTRIDGE_INIT_ADDRESS);
  set_word(rom, 4, 0);
  set_word(rom, 6, 0);
  set_word(rom, 8, 0);
  memcpy(rom + CARTRIDGE_HEADER_SIZE, boot, boot_length);
  memcpy(rom + CARTRIDGE_HEADER_SIZE + boot_length, loader, loader_length);
  memcpy(rom + CARTRIDGE_HEADER_SIZE + boot_length + loader_length,
         menu.data, menu.length);
  memcpy(rom + TITLE_BANK * ROM_BANK_SIZE, titles, TITLE_STRING_OFFSET + 256);
  memcpy(rom + KSS_BANK_OFFSET * ROM_BANK_SIZE,
         kss + bank_data, bank_count * KSS_BANK_SIZE);
  for (i = 0; i < 8; i++) {
    uint32_t source = KSS_HEADER_SIZE + i * ROM_BANK_SIZE;
    uint32_t destination = (KSS_MAIN_BANK + i) * ROM_BANK_SIZE;
    uint32_t size = i == 7 ? 0x1E00 : ROM_BANK_SIZE;
    memcpy(rom + destination, kss + source, size);
  }

  file = fopen(output, "wb");
  if (!file || fwrite(rom, 1, ROM_SIZE, file) != ROM_SIZE) {
    fprintf(stderr, "could not write %s\n", output);
    if (file)
      fclose(file);
    free(rom);
    free(kss);
    exit(1);
  }
  fclose(file);
  printf("wrote %s (1 MB, %u KSS banks, %u tracks)\n", output,
         (unsigned)bank_count, (unsigned)track_count);
  free(rom);
  free(kss);
}

int main(int argc, char **argv) {
  if (argc != 4 || strcmp(argv[1], "-o") != 0) {
    fprintf(stderr, "Usage: %s -o output.rom vigamup-8k.kss\n", argv[0]);
    return 1;
  }
  make_rom(argv[3], argv[2]);
  return 0;
}
