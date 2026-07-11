#include <stdlib.h>
#include <string.h>
#include "sng2kss_converter.h"

#define SNG_HEADER_OFFSET 0x780
#define SNG_PATTERNS_OFFSET 0x781
#define SNG_PATTERN_DATA_OFFSET 0x7E5
#define SNG_PATTERN_SIZE 0x600
#define SNG_ROW_SIZE 24
#define SNG_ROWS 64
#define SNG_CHANNELS 5
#define SNG_MODULATE_COUNT 3

#define SNG_LOAD_ADDR 0x0200
#define SNG_DRIVER_ADDR 0x4000
#define SNG_DRIVER_PLAY_OFFSET 0x30
#define SNG_DRIVER_SIZE 143
#define SNG_MAX_LOAD_SIZE 0xFE00
#define SNG_MAX_FRAMES (50 * 60 * 10)
#define SNG_BANK_ADDR 0x8000
#define SNG_BANK_SIZE 0x2000
#define SNG_BANK_SKIP_START 0x1800
#define SNG_BANK_SKIP_END 0x1900
#define SNG_BANK_OFFSET 1
#define SNG_MAX_BANKS 127
#define SNG_STREAM_JUMP 0xFB
#define SNG_STREAM_WAIT 0xFC
#define SNG_STREAM_CONTINUE 0xFD
#define SNG_KSS_HEADER_SIZE 0x20

enum { SNG_DIR_DOWN = 0, SNG_DIR_UP = 1 };

typedef struct {
  uint8_t direction;
  uint8_t value;
} SNG_EFFECT;

typedef struct {
  uint8_t direction;
  uint8_t value;
  uint8_t rest;
} SNG_SLIDE_STATE;

typedef struct {
  uint8_t instrument;
  uint8_t volume;
  uint16_t frequency_work;
  uint16_t frequency_original;
  SNG_EFFECT modulate_work;
  SNG_EFFECT modulate_original;
  SNG_SLIDE_STATE volume_slide;
  SNG_SLIDE_STATE frequency_slide;
  SNG_EFFECT transpose;
  uint8_t command;
  uint8_t value;
} SNG_CHANNEL_STATE;

typedef struct {
  const uint8_t *data;
  uint32_t size;
  uint8_t speed;
  uint8_t speed_count;
  uint8_t pattern_line;
  uint8_t position;
  uint8_t pattern_number;
  uint8_t modulate_count;
  int looped;
  SNG_CHANNEL_STATE ch[SNG_CHANNELS];
} SNG_STATE;

typedef struct {
  uint8_t *data;
  uint32_t size;
  uint32_t capacity;
  uint8_t shadow[0xE2];
} SNG_STREAM;

typedef struct {
  uint8_t *data;
  uint32_t bank_count;
  uint32_t offset;
} SNG_BANKS;

typedef struct {
  uint8_t bank;
  uint16_t addr;
} SNG_TRACK_REF;

static uint16_t sng_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const uint8_t *sng_cell_ptr(const uint8_t *row, int ch) { return ch < 4 ? row + ch * 5 : row + 20; }

static uint16_t sng_cell_frequency(const uint8_t *row, int ch) { return sng_u16(sng_cell_ptr(row, ch)); }
static uint8_t sng_cell_instrument(const uint8_t *row, int ch) { return ch < 4 ? sng_cell_ptr(row, ch)[2] : 0; }
static uint8_t sng_cell_volcmd(const uint8_t *row, int ch) { return ch < 4 ? sng_cell_ptr(row, ch)[3] : sng_cell_ptr(row, ch)[2]; }
static uint8_t sng_cell_value(const uint8_t *row, int ch) { return ch < 4 ? sng_cell_ptr(row, ch)[4] : sng_cell_ptr(row, ch)[3]; }

static int stream_reserve(SNG_STREAM *stream, uint32_t extra) {
  uint8_t *new_data;
  uint32_t new_capacity;

  if (stream->size + extra <= stream->capacity)
    return 0;

  new_capacity = stream->capacity ? stream->capacity : 4096;
  while (new_capacity < stream->size + extra)
    new_capacity *= 2;

  new_data = realloc(stream->data, new_capacity);
  if (!new_data)
    return 1;

  stream->data = new_data;
  stream->capacity = new_capacity;
  return 0;
}

static int stream_put(SNG_STREAM *stream, uint8_t value) {
  if (stream_reserve(stream, 1))
    return 1;
  stream->data[stream->size++] = value;
  return 0;
}

static int stream_reg(SNG_STREAM *stream, uint8_t reg, uint8_t value) {
  if (reg >= sizeof(stream->shadow))
    return 0;
  if (stream->shadow[reg] == value)
    return 0;
  if (stream_reserve(stream, 2))
    return 1;
  stream->shadow[reg] = value;
  stream->data[stream->size++] = reg;
  stream->data[stream->size++] = value;
  return 0;
}

static int stream_frame_end(SNG_STREAM *stream) { return stream_put(stream, 0xFF); }

static int stream_loop(SNG_STREAM *stream) { return stream_put(stream, 0xFE); }

static int stream_insert(SNG_STREAM *stream, uint32_t pos, const uint8_t *data, uint32_t size) {
  if (pos > stream->size || stream_reserve(stream, size))
    return 1;
  memmove(stream->data + pos + size, stream->data + pos, stream->size - pos);
  memcpy(stream->data + pos, data, size);
  stream->size += size;
  return 0;
}

static int stream_insert_wait(SNG_STREAM *stream, uint32_t pos, uint32_t frames) {
  while (frames) {
    uint32_t chunk = frames > 256 ? 256 : frames;
    uint8_t wait[2];
    wait[0] = SNG_STREAM_WAIT;
    wait[1] = (uint8_t)(chunk - 1);
    if (stream_insert(stream, pos, wait, sizeof(wait)))
      return 1;
    pos += sizeof(wait);
    frames -= chunk;
  }
  return 0;
}

static int banks_new_bank(SNG_BANKS *banks) {
  uint8_t *new_data;
  uint32_t new_count = banks->bank_count + 1;

  if (new_count > SNG_MAX_BANKS)
    return 1;

  new_data = realloc(banks->data, new_count * SNG_BANK_SIZE);
  if (!new_data)
    return 1;

  banks->data = new_data;
  memset(banks->data + banks->bank_count * SNG_BANK_SIZE, 0xFF, SNG_BANK_SIZE);
  banks->bank_count = new_count;
  banks->offset = 0;
  return 0;
}

static int banks_continue(SNG_BANKS *banks) {
  uint32_t bank = banks->bank_count;
  if (bank >= SNG_MAX_BANKS || banks->offset > SNG_BANK_SIZE - 2)
    return 1;

  banks->data[(banks->bank_count - 1) * SNG_BANK_SIZE + banks->offset++] = SNG_STREAM_CONTINUE;
  banks->data[(banks->bank_count - 1) * SNG_BANK_SIZE + banks->offset++] = (uint8_t)(bank + SNG_BANK_OFFSET);
  return banks_new_bank(banks);
}

static int banks_jump_scc_gap(SNG_BANKS *banks) {
  if (banks->offset > SNG_BANK_SKIP_START - 3)
    return 1;

  banks->data[(banks->bank_count - 1) * SNG_BANK_SIZE + banks->offset++] = SNG_STREAM_JUMP;
  banks->data[(banks->bank_count - 1) * SNG_BANK_SIZE + banks->offset++] =
      (uint8_t)((SNG_BANK_ADDR + SNG_BANK_SKIP_END) & 0xFF);
  banks->data[(banks->bank_count - 1) * SNG_BANK_SIZE + banks->offset++] =
      (uint8_t)((SNG_BANK_ADDR + SNG_BANK_SKIP_END) >> 8);
  banks->offset = SNG_BANK_SKIP_END;
  return 0;
}

static int banks_ensure_event_space(SNG_BANKS *banks, uint32_t size) {
  if (banks->bank_count == 0 && banks_new_bank(banks))
    return 1;

  for (;;) {
    if (banks->offset >= SNG_BANK_SKIP_START && banks->offset < SNG_BANK_SKIP_END) {
      banks->offset = SNG_BANK_SKIP_END;
      continue;
    }

    if (banks->offset < SNG_BANK_SKIP_START && banks->offset + size > SNG_BANK_SKIP_START - 3) {
      if (banks_jump_scc_gap(banks))
        return 1;
      continue;
    }

    if (banks->offset + size > SNG_BANK_SIZE - 2) {
      if (banks_continue(banks))
        return 1;
      continue;
    }

    return 0;
  }
}

static int banks_put_event(SNG_BANKS *banks, const uint8_t *data, uint32_t size) {
  if (banks_ensure_event_space(banks, size))
    return 1;
  memcpy(banks->data + (banks->bank_count - 1) * SNG_BANK_SIZE + banks->offset, data, size);
  banks->offset += size;
  return 0;
}

static int banks_put_stream(SNG_BANKS *banks, const SNG_STREAM *stream, SNG_TRACK_REF *ref) {
  uint32_t i;

  if (banks->bank_count == 0 && banks_new_bank(banks))
    return 1;

  if (banks_ensure_event_space(banks, 1))
    return 1;

  ref->bank = (uint8_t)(banks->bank_count - 1 + SNG_BANK_OFFSET);
  ref->addr = (uint16_t)(SNG_BANK_ADDR + banks->offset);

  for (i = 0; i < stream->size; i++) {
    uint32_t event_size;
    if (stream->data[i] == SNG_STREAM_WAIT)
      event_size = 2;
    else if (stream->data[i] == 0xFE || stream->data[i] == 0xFF)
      event_size = 1;
    else
      event_size = 2;

    if (i + event_size > stream->size || banks_put_event(banks, stream->data + i, event_size))
      return 1;
    i += event_size - 1;
  }

  return 0;
}

static const uint8_t *sng_row_ptr(SNG_STATE *sng) {
  uint32_t off = SNG_PATTERN_DATA_OFFSET + (uint32_t)sng->pattern_number * SNG_PATTERN_SIZE +
                 (uint32_t)sng->pattern_line * SNG_ROW_SIZE;
  if (!sng->data || off + SNG_ROW_SIZE > sng->size)
    return NULL;
  return sng->data + off;
}

static int sng_next_pattern(SNG_STATE *sng) {
  uint8_t song_length = sng->data[SNG_HEADER_OFFSET];
  sng->position++;
  if (song_length == 0 || sng->position >= song_length) {
    sng->position = 0;
    sng->looped = 1;
  }
  sng->pattern_number = sng->data[SNG_PATTERNS_OFFSET + sng->position];
  sng->pattern_line = 0;
  return sng->looped;
}

static int sng_emit_wave(SNG_STREAM *stream, int ch, const uint8_t *wave) {
  int i;
  int base = ch * 0x20;
  if (ch < 0 || ch >= 4)
    return 0;
  for (i = 0; i < 32; i++) {
    if (stream_reg(stream, (uint8_t)(base + i), wave[i]))
      return 1;
  }
  return 0;
}

static int sng_play_note(SNG_STATE *sng, SNG_STREAM *stream) {
  const uint8_t *row = sng_row_ptr(sng);
  int i;
  if (!row)
    return 0;

  for (i = 0; i < SNG_CHANNELS; i++) {
    uint16_t frequency = sng_cell_frequency(row, i);
    if (frequency) {
      if (sng->ch[i].transpose.direction == SNG_DIR_DOWN)
        frequency = (uint16_t)(frequency + sng->ch[i].transpose.value);
      else
        frequency = (uint16_t)(frequency - sng->ch[i].transpose.value);
      sng->ch[i].frequency_original = frequency;
      sng->ch[i].frequency_work = frequency;
      sng->ch[i].modulate_work = sng->ch[i].modulate_original;
    }
  }

  for (i = 0; i < SNG_CHANNELS; i++)
    sng->ch[i].volume = sng_cell_volcmd(row, i) >> 4;

  for (i = 0; i < SNG_CHANNELS - 1; i++) {
    uint8_t instrument = sng_cell_instrument(row, i);
    if (instrument && instrument != sng->ch[i].instrument) {
      uint32_t off = (uint32_t)(instrument - 1) * 40;
      sng->ch[i].instrument = instrument;
      if (off + 32 <= SNG_HEADER_OFFSET && sng_emit_wave(stream, i, sng->data + off))
        return 1;
    }
  }

  return 0;
}

static void sng_handle_transpose(SNG_STATE *sng, int ch, uint8_t direction, uint8_t value) {
  sng->ch[ch].transpose.direction = direction;
  sng->ch[ch].transpose.value = value;
  if (direction == SNG_DIR_DOWN)
    sng->ch[ch].frequency_work = (uint16_t)(sng->ch[ch].frequency_work + value);
  else
    sng->ch[ch].frequency_work = (uint16_t)(sng->ch[ch].frequency_work - value);
}

static void sng_handle_volume_slide(SNG_STATE *sng, int ch, uint8_t value) {
  sng->ch[ch].volume_slide.direction = (value & 0xF0) ? SNG_DIR_DOWN : SNG_DIR_UP;
  value &= 0x0F;
  sng->ch[ch].volume_slide.value = sng->speed ? (uint8_t)(value / sng->speed) : value;
  sng->ch[ch].volume_slide.rest = sng->speed ? (uint8_t)(value % sng->speed) : 0;
}

static void sng_handle_modulate(SNG_STATE *sng, int ch, uint8_t direction, uint8_t value) {
  sng->ch[ch].modulate_work.direction = direction;
  sng->ch[ch].modulate_work.value = value;
  sng->ch[ch].modulate_original.direction = direction;
  sng->ch[ch].modulate_original.value = value;
  sng->ch[ch].frequency_work = sng->ch[ch].frequency_original;
}

static void sng_handle_slide(SNG_STATE *sng, int ch, uint8_t direction, uint8_t value) {
  sng->ch[ch].frequency_slide.direction = direction;
  sng->ch[ch].frequency_slide.value = sng->speed ? (uint8_t)(value / sng->speed) : value;
  sng->ch[ch].frequency_slide.rest = sng->speed ? (uint8_t)(value % sng->speed) : 0;
}

static int sng_handle_command(SNG_STATE *sng) {
  const uint8_t *row = sng_row_ptr(sng);
  int i;
  int result = 1;
  if (!row)
    return 1;

  for (i = 0; i < SNG_CHANNELS; i++) {
    uint8_t command = sng_cell_volcmd(row, i) & 0x0F;
    uint8_t value = sng_cell_value(row, i);
    sng->ch[i].command = command;
    sng->ch[i].value = value;
    switch (command) {
    case 15:
      if (value)
        sng->speed = (value == 1) ? 2 : value;
      break;
    case 11:
      if (value == 0)
        sng->looped = 1;
      sng->position = (uint8_t)(value - 1);
      sng_next_pattern(sng);
      result = 0;
      break;
    case 10:
      sng_next_pattern(sng);
      result = 0;
      break;
    case 7:
      sng_handle_transpose(sng, i, SNG_DIR_DOWN, value);
      break;
    case 6:
      sng_handle_transpose(sng, i, SNG_DIR_UP, value);
      break;
    case 5:
      sng_handle_volume_slide(sng, i, value);
      break;
    case 4:
      sng_handle_modulate(sng, i, SNG_DIR_DOWN, value);
      break;
    case 3:
      sng_handle_modulate(sng, i, SNG_DIR_UP, value);
      break;
    case 2:
      sng_handle_slide(sng, i, SNG_DIR_DOWN, value);
      break;
    case 1:
      sng_handle_slide(sng, i, SNG_DIR_UP, value);
      break;
    default:
      break;
    }
  }

  return result;
}

static void sng_do_slide(SNG_STATE *sng, int ch) {
  uint8_t value = sng->ch[ch].frequency_slide.value;
  if (sng->ch[ch].frequency_slide.rest) {
    value++;
    sng->ch[ch].frequency_slide.rest--;
  }
  if (sng->ch[ch].command == 2) {
    sng->ch[ch].frequency_work = (uint16_t)(sng->ch[ch].frequency_work + value);
    sng->ch[ch].frequency_original = (uint16_t)(sng->ch[ch].frequency_original + value);
  } else {
    sng->ch[ch].frequency_work = (uint16_t)(sng->ch[ch].frequency_work - value);
    sng->ch[ch].frequency_original = (uint16_t)(sng->ch[ch].frequency_original - value);
  }
}

static void sng_do_volume_slide(SNG_STATE *sng, int ch) {
  uint8_t value = sng->ch[ch].volume_slide.value;
  if (sng->ch[ch].volume_slide.rest) {
    value++;
    sng->ch[ch].volume_slide.rest--;
  }
  if (sng->ch[ch].volume_slide.direction == SNG_DIR_DOWN) {
    sng->ch[ch].volume = (sng->ch[ch].volume > value) ? (uint8_t)(sng->ch[ch].volume - value) : 0;
  } else {
    sng->ch[ch].volume = (uint8_t)(sng->ch[ch].volume + value);
    if (sng->ch[ch].volume > 15)
      sng->ch[ch].volume = 15;
  }
}

static int sng_emit_registers(SNG_STATE *sng, SNG_STREAM *stream) {
  int i;

  for (i = 0; i < SNG_CHANNELS; i++) {
    uint16_t frequency = sng->ch[i].frequency_work;
    if (stream_reg(stream, (uint8_t)(0x80 + i * 2), (uint8_t)(frequency & 0xFF)))
      return 1;
    if (stream_reg(stream, (uint8_t)(0x81 + i * 2), (uint8_t)((frequency >> 8) & 0x0F)))
      return 1;
    if (stream_reg(stream, (uint8_t)(0x8A + i), (uint8_t)(sng->ch[i].volume & 0x0F)))
      return 1;
  }

  return stream_reg(stream, 0x8F, 0x1F);
}

static int sng_update(SNG_STATE *sng, SNG_STREAM *stream) {
  int i;

  sng->speed_count--;
  if (!sng->speed_count) {
    do {
      if (sng_play_note(sng, stream))
        return 1;
    } while (!sng_handle_command(sng) && !sng->looped);
    sng->speed_count = sng->speed ? sng->speed : 1;
    sng->pattern_line++;
    if (sng->pattern_line == SNG_ROWS)
      sng_next_pattern(sng);
  }

  for (i = 0; i < SNG_CHANNELS; i++) {
    if (sng->ch[i].command == 1 || sng->ch[i].command == 2)
      sng_do_slide(sng, i);
    else if (sng->ch[i].command == 5)
      sng_do_volume_slide(sng, i);
  }

  sng->modulate_count--;
  if (!sng->modulate_count) {
    sng->modulate_count = SNG_MODULATE_COUNT;
    for (i = 0; i < SNG_CHANNELS; i++) {
      if (sng->ch[i].modulate_work.value) {
        if (sng->ch[i].modulate_work.direction == SNG_DIR_DOWN) {
          sng->ch[i].frequency_work = (uint16_t)(sng->ch[i].frequency_work + sng->ch[i].modulate_work.value);
          sng->ch[i].modulate_work.direction = SNG_DIR_UP;
        } else {
          sng->ch[i].frequency_work = (uint16_t)(sng->ch[i].frequency_work - sng->ch[i].modulate_work.value);
          sng->ch[i].modulate_work.direction = SNG_DIR_DOWN;
        }
      }
    }
  }

  return sng_emit_registers(sng, stream);
}

static void sng_reset(SNG_STATE *sng, const uint8_t *data, uint32_t size) {
  int i;
  memset(sng, 0, sizeof(*sng));
  sng->data = data;
  sng->size = size;
  sng->speed = 8;
  sng->speed_count = 1;
  sng->modulate_count = 1;
  sng->position = 0xFF;
  for (i = 0; i < SNG_CHANNELS; i++) {
    sng->ch[i].transpose.direction = SNG_DIR_DOWN;
    sng->ch[i].modulate_work.direction = SNG_DIR_DOWN;
    sng->ch[i].modulate_original.direction = SNG_DIR_DOWN;
    sng->ch[i].volume_slide.direction = SNG_DIR_DOWN;
    sng->ch[i].frequency_slide.direction = SNG_DIR_DOWN;
  }
  sng_next_pattern(sng);
  sng->looped = 0;
}

static int sng_compile_stream(const uint8_t *data, uint32_t size, SNG_STREAM *stream) {
  SNG_STATE sng;
  uint32_t frame, wait_frames = 0;

  memset(stream, 0, sizeof(*stream));
  memset(stream->shadow, 0xFF, sizeof(stream->shadow));
  sng_reset(&sng, data, size);

  for (frame = 0; frame < SNG_MAX_FRAMES && !sng.looped; frame++) {
    uint32_t frame_start = stream->size;
    if (sng_update(&sng, stream))
      return 1;
    if (stream->size == frame_start) {
      wait_frames++;
    } else {
      if (wait_frames && stream_insert_wait(stream, frame_start, wait_frames))
        return 1;
      wait_frames = 0;
      if (stream_frame_end(stream))
        return 1;
    }
  }

  if (wait_frames && stream_insert_wait(stream, stream->size, wait_frames))
    return 1;

  if (stream_loop(stream))
    return 1;

  return 0;
}

static void set_word(uint8_t *data, uint32_t off, uint16_t value) {
  data[off] = (uint8_t)(value & 0xFF);
  data[off + 1] = (uint8_t)(value >> 8);
}

static void set_dword(uint8_t *data, uint32_t off, uint32_t value) {
  data[off] = (uint8_t)(value & 0xFF);
  data[off + 1] = (uint8_t)((value >> 8) & 0xFF);
  data[off + 2] = (uint8_t)((value >> 16) & 0xFF);
  data[off + 3] = (uint8_t)((value >> 24) & 0xFF);
}


static void sng_make_header(uint8_t *header, uint16_t load_adr, uint16_t load_size, uint16_t init_adr,
                            uint16_t play_adr) {
  header[0x00] = 'K';
  header[0x01] = 'S';
  header[0x02] = 'S';
  header[0x03] = 'X';
  set_word(header, 0x04, load_adr);
  set_word(header, 0x06, load_size);
  set_word(header, 0x08, init_adr);
  set_word(header, 0x0A, play_adr);
  header[0x0C] = 0x00;
  header[0x0D] = 0x00;
  header[0x0E] = 0x10;
  header[0x0F] = 0x05;
  header[0x1C] = 0x00;
  header[0x1D] = 0x00;
  header[0x1E] = 0x00;
  header[0x1F] = 0x00;
}

static SNG_KSS *sng_result_new(uint8_t *data, uint32_t size) {
  SNG_KSS *result = (SNG_KSS *)malloc(sizeof(*result));
  if (!result) {
    free(data);
    return NULL;
  }
  result->data = data;
  result->size = size;
  return result;
}

void sng_kss_delete(SNG_KSS *result) {
  if (result) {
    free(result->data);
    free(result);
  }
}

static void sng_make_driver(uint8_t *driver, uint16_t ptr_addr, uint16_t start_addr, uint16_t ptr_bank_addr,
                            uint16_t start_bank_addr, uint16_t wait_addr, uint16_t table_addr, uint8_t track_count) {
  static const uint8_t template_driver[SNG_DRIVER_SIZE] = {
      0x47,             /* LD B,A */
      0x78,             /* LD A,B */
      0xFE, 0x00,       /* CP track_count */
      0x38, 0x01,       /* JR C,use_track */
      0xAF,             /* XOR A */
      0x6F,             /* LD L,A */
      0x26, 0x00,       /* LD H,0 */
      0x29,             /* ADD HL,HL */
      0x5F,             /* LD E,A */
      0x16, 0x00,       /* LD D,0 */
      0x19,             /* ADD HL,DE */
      0x11, 0x00, 0x00, /* LD DE,table */
      0x19,             /* ADD HL,DE */
      0x7E,             /* LD A,(HL) */
      0x23,             /* INC HL */
      0x32, 0x00, 0x00, /* LD (ptr_bank),A */
      0x32, 0x00, 0x00, /* LD (start_bank),A */
      0x32, 0x00, 0x90, /* LD (09000H),A */
      0x5E,             /* LD E,(HL) */
      0x23,             /* INC HL */
      0x56,             /* LD D,(HL) */
      0xED, 0x53, 0x00, 0x00, /* LD (ptr),DE */
      0xED, 0x53, 0x00, 0x00, /* LD (start),DE */
      0xAF,                   /* XOR A */
      0x32, 0x00, 0x00,       /* LD (wait),A */
      0xC9,                   /* RET */
      0xC9, 0xC9,
      0x3A, 0x00, 0x00,       /* play: LD A,(wait) */
      0xB7,                   /* OR A */
      0x28, 0x05,             /* JR Z,read_stream */
      0x3D,                   /* DEC A */
      0x32, 0x00, 0x00,       /* LD (wait),A */
      0xC9,                   /* RET */
      0x3A, 0x00, 0x00,       /* play: LD A,(ptr_bank) */
      0x32, 0x00, 0x90,       /* LD (09000H),A */
      0x2A, 0x00, 0x00,       /* LD HL,(ptr) */
      0x7E,                   /* loop: LD A,(HL) */
      0x23,                   /* INC HL */
      0xFE, 0xFB,             /* CP 0FBH */
      0x28, 0x18,             /* JR Z,jump */
      0xFE, 0xFC,             /* CP 0FCH */
      0x28, 0x1B,             /* JR Z,wait_marker */
      0xFE, 0xFD,             /* CP 0FDH */
      0x28, 0x1E,             /* JR Z,next_bank */
      0xFE, 0xFE,             /* CP 0FEH */
      0x28, 0x27,             /* JR Z,reset */
      0xFE, 0xFF,             /* CP 0FFH */
      0x28, 0x31,             /* JR Z,done */
      0x5F,                   /* LD E,A */
      0x16, 0x98,             /* LD D,98H */
      0x7E,                   /* LD A,(HL) */
      0x23,                   /* INC HL */
      0x12,                   /* LD (DE),A */
      0x18, 0xE2,             /* JR loop */
      0x5E,                   /* jump: LD E,(HL) */
      0x23,                   /* INC HL */
      0x56,                   /* LD D,(HL) */
      0x23,                   /* INC HL */
      0xEB,                   /* EX DE,HL */
      0x18, 0xDB,             /* JR loop */
      0x7E,                   /* wait_marker: LD A,(HL) */
      0x23,                   /* INC HL */
      0x32, 0x00, 0x00,       /* LD (wait),A */
      0x18, 0x1B,             /* JR done */
      0x7E,                   /* next_bank: LD A,(HL) */
      0x23,                   /* INC HL */
      0x32, 0x00, 0x00,       /* LD (ptr_bank),A */
      0x32, 0x00, 0x90,       /* LD (09000H),A */
      0x21, 0x00, 0x80,       /* LD HL,08000H */
      0x18, 0xC7,             /* JR loop */
      0x3A, 0x00, 0x00,       /* reset: LD A,(start_bank) */
      0x32, 0x00, 0x00,       /* LD (ptr_bank),A */
      0x32, 0x00, 0x90,       /* LD (09000H),A */
      0x2A, 0x00, 0x00,       /* reset: LD HL,(start) */
      0x18, 0xB9,             /* JR loop */
      0x22, 0x00, 0x00,       /* done: LD (ptr),HL */
      0xC9                    /* RET */
  };

  memcpy(driver, template_driver, sizeof(template_driver));
  driver[0x03] = track_count;
  set_word(driver, 0x10, table_addr);
  set_word(driver, 0x16, ptr_bank_addr);
  set_word(driver, 0x19, start_bank_addr);
  set_word(driver, 0x23, ptr_addr);
  set_word(driver, 0x27, start_addr);
  set_word(driver, 0x2B, wait_addr);
  set_word(driver, 0x31, wait_addr);
  set_word(driver, 0x38, wait_addr);
  set_word(driver, 0x3C, ptr_bank_addr);
  set_word(driver, 0x42, ptr_addr);
  set_word(driver, 0x6C, wait_addr);
  set_word(driver, 0x73, ptr_bank_addr);
  set_word(driver, 0x7E, start_bank_addr);
  set_word(driver, 0x81, ptr_bank_addr);
  set_word(driver, 0x87, start_addr);
  set_word(driver, 0x8C, ptr_addr);
}

int sng_is_valid(const uint8_t *data, uint32_t size) {
  uint32_t song_length, pattern_count, i;
  if (!data || size < SNG_PATTERN_DATA_OFFSET + SNG_PATTERN_SIZE)
    return 0;

  song_length = data[SNG_HEADER_OFFSET];
  if (song_length == 0 || song_length > 0x64)
    return 0;

  pattern_count = (size - SNG_PATTERN_DATA_OFFSET) / SNG_PATTERN_SIZE;
  if (pattern_count == 0)
    return 0;

  for (i = 0; i < song_length; i++) {
    if (data[SNG_PATTERNS_OFFSET + i] >= pattern_count)
      return 0;
  }

  return 1;
}

static uint32_t sng_info_size(const char **titles, uint32_t count) {
  uint32_t i;
  uint32_t size = 0x10;

  if (!titles)
    return 0;

  for (i = 0; i < count; i++) {
    if (titles[i])
      size += 10 + (uint32_t)strlen(titles[i]) + 1;
    else
      size += 11;
  }

  return size;
}

static void sng_write_info(uint8_t *buf, const char **titles, uint32_t count) {
  uint32_t i, off = 0x10;

  memcpy(buf, "INFO", 4);
  set_dword(buf, 4, sng_info_size(titles, count) - 0x10);
  set_word(buf, 8, (uint16_t)count);
  memset(buf + 10, 0, 6);

  for (i = 0; i < count; i++) {
    const char *title = titles[i] ? titles[i] : "-";
    buf[off++] = (uint8_t)i;
    buf[off++] = 0;
    set_dword(buf, off, 0);
    off += 4;
    set_dword(buf, off, 0);
    off += 4;
    strcpy((char *)(buf + off), title);
    off += (uint32_t)strlen(title) + 1;
  }
}

SNG_KSS *sngs_to_kss(const uint8_t **data, const uint32_t *sizes, const char **titles, uint32_t count) {
  SNG_KSS *result;
  SNG_STREAM *streams;
  SNG_BANKS banks;
  SNG_TRACK_REF *track_refs;
  uint8_t *buf;
  uint32_t i, load_size, bank_size, total_size, driver_offset, table_offset, info_size;
  uint16_t ptr_addr, start_addr, ptr_bank_addr, start_bank_addr, wait_addr, table_addr, resident_end;

  if (!data || !sizes || count == 0 || count > 255)
    return NULL;

  streams = calloc(count, sizeof(SNG_STREAM));
  if (!streams)
    return NULL;

  memset(&banks, 0, sizeof(banks));
  track_refs = calloc(count, sizeof(SNG_TRACK_REF));
  if (!track_refs) {
    free(streams);
    return NULL;
  }

  for (i = 0; i < count; i++) {
    if (!sng_is_valid(data[i], sizes[i]) || sng_compile_stream(data[i], sizes[i], &streams[i])) {
      uint32_t j;
      for (j = 0; j <= i; j++)
        free(streams[j].data);
      free(track_refs);
      free(streams);
      return NULL;
    }
    if (banks_put_stream(&banks, &streams[i], &track_refs[i])) {
      uint32_t j;
      for (j = 0; j <= i; j++)
        free(streams[j].data);
      free(banks.data);
      free(track_refs);
      free(streams);
      return NULL;
    }
  }

  ptr_addr = SNG_DRIVER_ADDR - 8;
  start_addr = SNG_DRIVER_ADDR - 6;
  ptr_bank_addr = SNG_DRIVER_ADDR - 4;
  start_bank_addr = SNG_DRIVER_ADDR - 3;
  wait_addr = SNG_DRIVER_ADDR - 2;
  table_addr = SNG_DRIVER_ADDR + SNG_DRIVER_SIZE;
  resident_end = (uint16_t)(table_addr + count * 3);
  load_size = (uint32_t)(resident_end - SNG_LOAD_ADDR);
  if (load_size > SNG_MAX_LOAD_SIZE) {
    for (i = 0; i < count; i++)
      free(streams[i].data);
    free(banks.data);
    free(track_refs);
    free(streams);
    return NULL;
  }

  info_size = sng_info_size(titles, count);
  bank_size = banks.bank_count * SNG_BANK_SIZE;
  total_size = SNG_KSS_HEADER_SIZE + load_size + bank_size + info_size;
  buf = malloc(total_size);
  if (!buf) {
    for (i = 0; i < count; i++)
      free(streams[i].data);
    free(banks.data);
    free(track_refs);
    free(streams);
    return NULL;
  }

  memset(buf, 0xC9, total_size);
  sng_make_header(buf, SNG_LOAD_ADDR, (uint16_t)load_size, SNG_DRIVER_ADDR, SNG_DRIVER_ADDR + SNG_DRIVER_PLAY_OFFSET);
  set_dword(buf, 0x10, info_size ? load_size + bank_size : 0);
  set_word(buf, 0x18, 0);
  set_word(buf, 0x1A, (uint16_t)(count - 1));
  buf[0x0C] = SNG_BANK_OFFSET;
  buf[0x0D] = (uint8_t)(0x80 | banks.bank_count);
  buf[0x0F] = 0x40;

  driver_offset = SNG_KSS_HEADER_SIZE + (SNG_DRIVER_ADDR - SNG_LOAD_ADDR);
  table_offset = driver_offset + SNG_DRIVER_SIZE;
  sng_make_driver(buf + driver_offset, ptr_addr, start_addr, ptr_bank_addr, start_bank_addr, wait_addr, table_addr,
                  (uint8_t)count);

  for (i = 0; i < count; i++) {
    uint32_t off = table_offset + i * 3;
    buf[off] = track_refs[i].bank;
    set_word(buf, off + 1, track_refs[i].addr);
  }

  memcpy(buf + SNG_KSS_HEADER_SIZE + load_size, banks.data, bank_size);

  if (info_size)
    sng_write_info(buf + SNG_KSS_HEADER_SIZE + load_size + bank_size, titles, count);

  result = sng_result_new(buf, total_size);
  for (i = 0; i < count; i++)
    free(streams[i].data);
  free(banks.data);
  free(track_refs);
  free(streams);
  return result;
}
