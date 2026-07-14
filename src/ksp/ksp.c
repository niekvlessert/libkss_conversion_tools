#include "ksp/ksp.h"
#include "ksp/engine.h"
#include "ksp/mbwave.h"
#include "zx_compressor.h"
#include "zx_decompressor.h"

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

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fail(char *error, size_t error_size, const char *message) {
  if (error && error_size) {
    snprintf(error, error_size, "%s", message);
  }
}

static int read_file(const char *path, uint8_t **data, uint32_t *size,
                     char *error, size_t error_size) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *buffer;

  if (!file) {
    snprintf(error, error_size, "could not open %s", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    fail(error, error_size, "could not determine input size");
    return 0;
  }
  buffer = (uint8_t *)malloc(length ? (size_t)length : 1);
  if (!buffer || (length && fread(buffer, 1, (size_t)length, file) != (size_t)length)) {
    free(buffer);
    fclose(file);
    fail(error, error_size, "could not read input");
    return 0;
  }
  fclose(file);
  *data = buffer;
  *size = (uint32_t)length;
  return 1;
}

static int write_all(FILE *file, const void *data, size_t size) {
  return size == 0 || fwrite(data, 1, size, file) == size;
}

static int type_allows_zx0(const char type[4]) {
  return !memcmp(type, "ENGN", 4) || !memcmp(type, "SONG", 4);
}

typedef struct {
  const uint8_t *data;
  uint8_t *owned_data;
  uint32_t packed_size;
  uint32_t unpacked_size;
  uint32_t crc32;
  uint16_t compression;
} KSP_LAYOUT;

uint32_t ksp_crc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  size_t i;
  for (i = 0; i < size; i++) {
    unsigned bit;
    crc ^= data[i];
    for (bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1));
  }
  return ~crc;
}

int ksp_write_file(const char *output_path, const uint8_t *kss_prefix,
                   uint32_t kss_prefix_size, const KSP_CHUNK *chunks,
                   uint32_t chunk_count, char *error, size_t error_size) {
  uint64_t directory_size64 = KSP_DIRECTORY_HEADER_SIZE +
                               (uint64_t)chunk_count * KSP_DIRECTORY_ENTRY_SIZE;
  uint32_t *offsets = NULL;
  KSP_LAYOUT *layouts = NULL;
  uint8_t *directory = NULL;
  uint32_t i;
  uint32_t cursor;
  FILE *file = NULL;
  uint8_t trailer[KSP_TRAILER_SIZE];
  int result = 0;

  if (!kss_prefix || kss_prefix_size < 0x20 ||
      memcmp(kss_prefix, "KSSX", 4) != 0) {
    fail(error, error_size, "KSSX header is missing");
    return 0;
  }
  if (chunk_count == 0 || directory_size64 > UINT32_MAX) {
    fail(error, error_size, "KSP directory is too large");
    return 0;
  }

  offsets = (uint32_t *)calloc(chunk_count, sizeof(*offsets));
  layouts = (KSP_LAYOUT *)calloc(chunk_count, sizeof(*layouts));
  if (!offsets || !layouts) {
    fail(error, error_size, "out of memory");
    goto cleanup;
  }

  for (i = 0; i < chunk_count; i++) {
    uint8_t *compressed = NULL;
    uint32_t compressed_size = 0;
    uint32_t delta = 0;

    if (chunks[i].size && !chunks[i].data) {
      fail(error, error_size, "KSP chunk has no data");
      goto cleanup;
    }
    if (chunks[i].compression != KSP_COMPRESSION_NONE &&
        chunks[i].compression != KSP_COMPRESSION_ZX0) {
      fail(error, error_size, "unsupported KSP chunk compression");
      goto cleanup;
    }
    if (chunks[i].compression == KSP_COMPRESSION_ZX0 &&
        !type_allows_zx0(chunks[i].type)) {
      fail(error, error_size, "ZX0 is supported for ENGN and SONG chunks only");
      goto cleanup;
    }

    layouts[i].data = chunks[i].data;
    layouts[i].packed_size = chunks[i].size;
    layouts[i].unpacked_size = chunks[i].size;
    layouts[i].crc32 = ksp_crc32(chunks[i].data, chunks[i].size);
    layouts[i].compression = KSP_COMPRESSION_NONE;

    if (chunks[i].compression == KSP_COMPRESSION_ZX0 && chunks[i].size >= 3u &&
        zx0_compress_data(chunks[i].data, chunks[i].size, &compressed,
                          &compressed_size, &delta) &&
        compressed_size < chunks[i].size) {
      layouts[i].owned_data = compressed;
      layouts[i].data = compressed;
      layouts[i].packed_size = compressed_size;
      layouts[i].compression = KSP_COMPRESSION_ZX0;
    } else {
      free(compressed);
    }
  }

  cursor = KSP_KSS_HEADER_SIZE;
  for (i = 0; i < chunk_count; i++) {
    if (layouts[i].packed_size > UINT32_MAX - cursor) {
      fail(error, error_size, "KSP chunk layout overflows");
      goto cleanup;
    }
    offsets[i] = cursor;
    cursor += layouts[i].packed_size;
  }
  if ((uint64_t)cursor + directory_size64 + KSP_TRAILER_SIZE > UINT32_MAX) {
    fail(error, error_size, "KSP directory is too large");
    goto cleanup;
  }

  directory = (uint8_t *)calloc(1, (size_t)directory_size64);
  if (!directory) {
    fail(error, error_size, "out of memory");
    goto cleanup;
  }
  memcpy(directory, "KDIR", 4);
  put16(directory + 4, KSP_DIRECTORY_HEADER_SIZE);
  put16(directory + 6, KSP_DIRECTORY_ENTRY_SIZE);
  put32(directory + 8, chunk_count);
  put32(directory + 12, 0);
  for (i = 0; i < chunk_count; i++) {
    uint8_t *entry = directory + KSP_DIRECTORY_HEADER_SIZE +
                     i * KSP_DIRECTORY_ENTRY_SIZE;
    memcpy(entry, chunks[i].type, 4);
    put32(entry + 4, chunks[i].id);
    put32(entry + 8, offsets[i]);
    put32(entry + 12, layouts[i].packed_size);
    put32(entry + 16, layouts[i].unpacked_size);
    put32(entry + 20, layouts[i].crc32);
    put16(entry + 24, layouts[i].compression);
    put16(entry + 26, 0);
    put32(entry + 28, chunks[i].aux);
  }

  file = fopen(output_path, "wb");
  if (!file) {
    snprintf(error, error_size, "could not create %s", output_path);
    goto cleanup;
  }
  if (!write_all(file, kss_prefix, KSP_KSS_HEADER_SIZE))
    goto write_error;
  for (i = 0; i < chunk_count; i++)
    if (!write_all(file, layouts[i].data, layouts[i].packed_size))
      goto write_error;
  if (!write_all(file, directory, (size_t)directory_size64))
    goto write_error;

  memcpy(trailer, "KSP1", 4);
  put16(trailer + 4, KSP_TRAILER_SIZE);
  put16(trailer + 6, 0);
  put32(trailer + 8, cursor);
  put32(trailer + 12, (uint32_t)directory_size64);
  put32(trailer + 16, ksp_crc32(directory, (size_t)directory_size64));
  put32(trailer + 20, 0);
  if (!write_all(file, trailer, sizeof(trailer)))
    goto write_error;
  if (fclose(file) != 0) {
    file = NULL;
    goto write_error;
  }
  file = NULL;
  result = 1;
  goto cleanup;

write_error:
  fail(error, error_size, "could not write KSP file");

cleanup:
  if (file) fclose(file);
  if (layouts) {
    for (i = 0; i < chunk_count; i++)
      free(layouts[i].owned_data);
  }
  free(directory);
  free(layouts);
  free(offsets);
  return result;
}

static int ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b,
                          uint32_t b_size) {
  uint64_t a_end = (uint64_t)a + a_size;
  uint64_t b_end = (uint64_t)b + b_size;
  return a < b_end && b < a_end;
}

static int decode_chunk_data(const uint8_t *packed, uint32_t packed_size,
                             uint32_t unpacked_size, uint16_t compression,
                             uint8_t **decoded, char *error,
                             size_t error_size) {
  *decoded = NULL;
  if (compression == KSP_COMPRESSION_NONE) {
    if (packed_size != unpacked_size) {
      fail(error, error_size, "uncompressed KSP chunk has mismatched sizes");
      return 0;
    }
    return 1;
  }
  if (compression != KSP_COMPRESSION_ZX0) {
    fail(error, error_size, "unsupported KSP chunk compression");
    return 0;
  }
  *decoded = (uint8_t *)malloc(unpacked_size ? unpacked_size : 1u);
  if (!*decoded) {
    fail(error, error_size, "out of memory for KSP decompression");
    return 0;
  }
  if (!zx0_decompress_data(packed, packed_size, *decoded, unpacked_size)) {
    free(*decoded);
    *decoded = NULL;
    fail(error, error_size, "invalid ZX0 KSP chunk");
    return 0;
  }
  return 1;
}

int ksp_validate_file(const char *path, int verify_crc, KSP_INDEX *index,
                      char *error, size_t error_size) {
  uint8_t *file = NULL;
  uint32_t file_size = 0;
  const uint8_t *trailer;
  const uint8_t *directory;
  uint32_t directory_offset;
  uint32_t directory_size;
  uint32_t entry_count;
  uint32_t i;
  int has_engine = 0, has_song = 0, has_descriptor = 0;

  if (index) memset(index, 0, sizeof(*index));
  if (!read_file(path, &file, &file_size, error, error_size)) return 0;
  if (file_size < 0x20 + KSP_TRAILER_SIZE || memcmp(file, "KSSX", 4) != 0) {
    fail(error, error_size, "file does not begin with a KSSX image");
    free(file);
    return 0;
  }
  trailer = file + file_size - KSP_TRAILER_SIZE;
  if (memcmp(trailer, "KSP1", 4) != 0 || get16(trailer + 4) != KSP_TRAILER_SIZE ||
      get16(trailer + 6) != 0 || get32(trailer + 20) != 0) {
    fail(error, error_size, "invalid KSP trailer");
      free(file);
    return 0;
  }
  directory_offset = get32(trailer + 8);
  directory_size = get32(trailer + 12);
  if (directory_offset < 0x20 || directory_offset > file_size - KSP_TRAILER_SIZE ||
      directory_size < KSP_DIRECTORY_HEADER_SIZE ||
      directory_size > file_size - KSP_TRAILER_SIZE - directory_offset) {
    fail(error, error_size, "KSP directory is outside the file");
    free(file);
    return 0;
  }
  directory = file + directory_offset;
  if (get32(trailer + 16) != ksp_crc32(directory, directory_size)) {
    fail(error, error_size, "KSP directory CRC mismatch");
    free(file);
    return 0;
  }
  if (memcmp(directory, "KDIR", 4) != 0 ||
      get16(directory + 4) != KSP_DIRECTORY_HEADER_SIZE ||
      get16(directory + 6) != KSP_DIRECTORY_ENTRY_SIZE ||
      get32(directory + 12) != 0) {
    fail(error, error_size, "invalid KSP directory header");
    free(file);
    return 0;
  }
  entry_count = get32(directory + 8);
  if ((uint64_t)KSP_DIRECTORY_HEADER_SIZE +
          (uint64_t)entry_count * KSP_DIRECTORY_ENTRY_SIZE != directory_size) {
    fail(error, error_size, "KSP directory size does not match entry count");
    free(file);
    return 0;
  }
  if (index) {
    index->entries = (KSP_ENTRY *)calloc(entry_count, sizeof(*index->entries));
    if (!index->entries) {
      fail(error, error_size, "out of memory");
      free(file);
      return 0;
    }
    index->file_size = file_size;
    index->directory_offset = directory_offset;
    index->directory_size = directory_size;
    index->entry_count = entry_count;
  }
  for (i = 0; i < entry_count; i++) {
    const uint8_t *p = directory + KSP_DIRECTORY_HEADER_SIZE +
                       i * KSP_DIRECTORY_ENTRY_SIZE;
    const uint8_t *chunk_data;
    uint8_t *decoded = NULL;
    uint32_t offset = get32(p + 8);
    uint32_t packed_size = get32(p + 12);
    uint32_t unpacked_size = get32(p + 16);
    uint16_t compression = get16(p + 24);
    uint32_t crc = get32(p + 20);
    uint32_t j;
    if ((compression != KSP_COMPRESSION_NONE &&
         compression != KSP_COMPRESSION_ZX0) ||
        (compression == KSP_COMPRESSION_ZX0 &&
         !type_allows_zx0((const char *)p)) ||
        (compression == KSP_COMPRESSION_NONE &&
         packed_size != unpacked_size) ||
        offset < 0x20 || offset > directory_offset ||
        packed_size > directory_offset - offset) {
      fail(error, error_size, "invalid KSP chunk bounds or compression");
      ksp_free_index(index);
      free(file);
      return 0;
    }
    for (j = 0; j < i; j++) {
      const uint8_t *q = directory + KSP_DIRECTORY_HEADER_SIZE +
                         j * KSP_DIRECTORY_ENTRY_SIZE;
      if (ranges_overlap(offset, packed_size, get32(q + 8), get32(q + 12))) {
        fail(error, error_size, "KSP chunks overlap");
        ksp_free_index(index);
        free(file);
        return 0;
      }
    }
    if (!decode_chunk_data(file + offset, packed_size, unpacked_size,
                           compression, &decoded, error, error_size)) {
      ksp_free_index(index);
      free(file);
      return 0;
    }
    chunk_data = decoded ? decoded : file + offset;
    if (verify_crc && ksp_crc32(chunk_data, unpacked_size) != crc) {
      free(decoded);
      fail(error, error_size, "KSP chunk CRC mismatch");
      ksp_free_index(index);
      free(file);
      return 0;
    }
    if (!memcmp(p, "ENGN", 4) && get32(p + 4) == 0) has_engine = 1;
    if (!memcmp(p, "SONG", 4)) has_song = 1;
    if (!memcmp(p, "EDES", 4) && get32(p + 4) == 0) {
      if (!ksp_validate_engine_descriptor(chunk_data, unpacked_size,
                                          error, error_size)) {
        free(decoded);
        ksp_free_index(index);
        free(file);
        return 0;
      }
      has_descriptor = 1;
    }
    free(decoded);
    for (j = 0; j < i; j++) {
      const uint8_t *q = directory + KSP_DIRECTORY_HEADER_SIZE +
                         j * KSP_DIRECTORY_ENTRY_SIZE;
      if (!memcmp(p, q, 4) && get32(p + 4) == get32(q + 4)) {
        fail(error, error_size, "duplicate KSP chunk type and ID");
        ksp_free_index(index);
        free(file);
        return 0;
      }
    }
    if (index) {
      memcpy(index->entries[i].type, p, 4);
      index->entries[i].type[4] = 0;
      index->entries[i].id = get32(p + 4);
      index->entries[i].offset = offset;
      index->entries[i].packed_size = packed_size;
      index->entries[i].unpacked_size = unpacked_size;
      index->entries[i].crc32 = crc;
      index->entries[i].compression = compression;
      index->entries[i].flags = get16(p + 26);
      index->entries[i].aux = get32(p + 28);
    }
  }
  if (!has_engine || !has_song || !has_descriptor) {
    fail(error, error_size, "KSP is missing ENGN, SONG, or EDES");
    ksp_free_index(index);
    free(file);
    return 0;
  }
  free(file);
  return 1;
}

int ksp_read_chunk(const char *path, const KSP_ENTRY *entry, uint8_t **data,
                   char *error, size_t error_size) {
  FILE *file;
  uint8_t *packed = NULL;
  uint8_t *buffer = NULL;
  uint8_t *decoded = NULL;
  long file_size;

  if (!entry || !data ||
      (entry->compression != KSP_COMPRESSION_NONE &&
       entry->compression != KSP_COMPRESSION_ZX0) ||
      (entry->compression == KSP_COMPRESSION_ZX0 &&
       !type_allows_zx0(entry->type)) ||
      (entry->compression == KSP_COMPRESSION_NONE &&
       entry->packed_size != entry->unpacked_size)) {
    fail(error, error_size, "unsupported KSP chunk compression");
    return 0;
  }
  file = fopen(path, "rb");
  if (!file || fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
      (uint64_t)entry->offset + entry->packed_size > (uint64_t)file_size ||
      fseek(file, (long)entry->offset, SEEK_SET) != 0) {
    if (file) fclose(file);
    fail(error, error_size, "KSP chunk is outside the file");
    return 0;
  }
  packed = (uint8_t *)malloc(entry->packed_size ? entry->packed_size : 1u);
  if (!packed || (entry->packed_size &&
                  fread(packed, 1, entry->packed_size, file) != entry->packed_size)) {
    free(packed);
    fclose(file);
    fail(error, error_size, "could not read KSP chunk");
    return 0;
  }
  fclose(file);
  if (!decode_chunk_data(packed, entry->packed_size, entry->unpacked_size,
                         entry->compression, &decoded, error, error_size)) {
    free(packed);
    return 0;
  }
  if (entry->compression == KSP_COMPRESSION_NONE) {
    buffer = packed;
    packed = NULL;
  } else {
    buffer = decoded;
    decoded = NULL;
  }
  free(packed);
  free(decoded);
  if (ksp_crc32(buffer, entry->unpacked_size) != entry->crc32) {
    free(buffer);
    fail(error, error_size, "KSP chunk CRC mismatch");
    return 0;
  }
  *data = buffer;
  return 1;
}

static const KSP_ENTRY *find_entry(const KSP_INDEX *index, const char type[4],
                                   uint32_t id) {
  uint32_t i;
  if (!index) return NULL;
  for (i = 0; i < index->entry_count; i++) {
    if (index->entries[i].id == id && !memcmp(index->entries[i].type, type, 4))
      return &index->entries[i];
  }
  return NULL;
}

int ksp_index_is_compact(const KSP_INDEX *index) {
  uint32_t i;
  uint32_t first_offset = UINT32_MAX;
  if (!index || !index->entries || !index->entry_count) return 0;
  for (i = 0; i < index->entry_count; i++) {
    if (index->entries[i].offset < first_offset)
      first_offset = index->entries[i].offset;
  }
  return first_offset == KSP_KSS_HEADER_SIZE;
}

static void decode_descriptor(const uint8_t *data,
                              KSP_ENGINE_DESCRIPTOR *descriptor) {
  memset(descriptor, 0, sizeof(*descriptor));
  descriptor->engine_type = get16(data + 6);
  descriptor->load_address = get16(data + 8);
  descriptor->init_address = get16(data + 10);
  descriptor->play_address = get16(data + 12);
  descriptor->stop_address = get16(data + 14);
  descriptor->capability_address = get16(data + 16);
  descriptor->song_window_address = get16(data + 18);
  descriptor->work_address = get16(data + 20);
  descriptor->work_size = get16(data + 22);
  descriptor->tick_rate_num = get16(data + 24);
  descriptor->tick_rate_den = get16(data + 26);
  descriptor->minimum_mapper_ram = get32(data + 28);
  descriptor->flags = get32(data + 32);
}

int ksp_build_kss_image_for_song(const char *path, const KSP_INDEX *index,
                                 uint32_t song_id, uint8_t **image,
                                 uint32_t *image_size, char *error,
                                 size_t error_size) {
  const KSP_ENTRY *engine_entry;
  const KSP_ENTRY *song_entry;
  const KSP_ENTRY *descriptor_entry;
  uint8_t *file = NULL;
  uint8_t *engine = NULL;
  uint8_t *song = NULL;
  uint8_t *descriptor_data = NULL;
  uint8_t *compact_song = NULL;
  uint8_t *result = NULL;
  uint32_t file_size = 0;
  uint32_t load_address;
  uint32_t load_size;
  uint32_t engine_offset;
  uint32_t song_offset;
  uint32_t compact_song_size = 0;
  uint64_t image_size64;
  KSP_ENGINE_DESCRIPTOR descriptor;
  int ok = 0;

  if (!image || !image_size) {
    fail(error, error_size, "KSP image output is missing");
    return 0;
  }
  *image = NULL;
  *image_size = 0;
  if (!ksp_index_is_compact(index)) {
    fail(error, error_size, "KSP file does not use the compact layout");
    return 0;
  }
  engine_entry = find_entry(index, "ENGN", 0);
  song_entry = find_entry(index, "SONG", song_id);
  descriptor_entry = find_entry(index, "EDES", 0);
  if (!engine_entry || !song_entry || !descriptor_entry) {
    fail(error, error_size, "compact KSP is missing a required chunk");
    return 0;
  }
  if (!read_file(path, &file, &file_size, error, error_size) ||
      file_size < KSP_KSS_HEADER_SIZE || memcmp(file, "KSSX", 4) != 0) {
    free(file);
    fail(error, error_size, "compact KSP has no valid KSSX header");
    return 0;
  }

  load_address = get16(file + 4);
  load_size = get16(file + 6);
  if (!load_size || (file[0x0D] & 0x7Fu)) {
    free(file);
    fail(error, error_size, "compact KSP has unsupported KSS banking");
    return 0;
  }
  image_size64 = (uint64_t)KSP_KSS_HEADER_SIZE + load_size;
  if (image_size64 > UINT32_MAX) {
    free(file);
    fail(error, error_size, "compact KSS image is too large");
    return 0;
  }
  if (!ksp_read_chunk(path, engine_entry, &engine, error, error_size) ||
      !ksp_read_chunk(path, song_entry, &song, error, error_size) ||
      !ksp_read_chunk(path, descriptor_entry, &descriptor_data, error,
                      error_size))
    goto cleanup;
  if (!ksp_validate_engine_descriptor(descriptor_data,
                                      descriptor_entry->unpacked_size, error,
                                      error_size))
    goto cleanup;
  decode_descriptor(descriptor_data, &descriptor);

  if (descriptor.load_address < load_address ||
      descriptor.load_address - load_address > load_size ||
      engine_entry->unpacked_size >
          load_size - (descriptor.load_address - load_address)) {
    fail(error, error_size, "engine does not fit the compact KSS image");
    goto cleanup;
  }
  engine_offset = descriptor.load_address - load_address;

  if (descriptor.engine_type == 1) {
    if (!ksp_compact_mwm(song, song_entry->unpacked_size, &compact_song,
                         &compact_song_size)) {
      fail(error, error_size, "could not compact MBWave MWM");
      goto cleanup;
    }
  } else {
    compact_song = song;
    song = NULL;
    compact_song_size = song_entry->unpacked_size;
  }
  if (descriptor.song_window_address < load_address ||
      descriptor.song_window_address - load_address > load_size ||
      compact_song_size >
          load_size - (descriptor.song_window_address - load_address)) {
    fail(error, error_size, "song does not fit the compact KSS image");
    goto cleanup;
  }
  song_offset = descriptor.song_window_address - load_address;

  result = (uint8_t *)calloc(1, (size_t)image_size64);
  if (!result) {
    fail(error, error_size, "out of memory for compact KSS image");
    goto cleanup;
  }
  memcpy(result, file, KSP_KSS_HEADER_SIZE);
  memcpy(result + KSP_KSS_HEADER_SIZE + engine_offset, engine,
         engine_entry->unpacked_size);
  memcpy(result + KSP_KSS_HEADER_SIZE + song_offset, compact_song,
         compact_song_size);
  if (descriptor.engine_type == 1) {
    uint32_t bootstrap_offset;
    if (descriptor.init_address < load_address ||
        descriptor.init_address - load_address > load_size ||
        ksp_mbwave_bootstrap_size() >
            load_size - (descriptor.init_address - load_address)) {
      fail(error, error_size, "MBWave bootstrap does not fit the KSS image");
      goto cleanup;
    }
    bootstrap_offset = descriptor.init_address - load_address;
    if (!ksp_copy_mbwave_bootstrap(
            result + KSP_KSS_HEADER_SIZE + bootstrap_offset,
            load_size - bootstrap_offset)) {
      fail(error, error_size, "could not build MBWave bootstrap");
      goto cleanup;
    }
  }
  *image = result;
  *image_size = (uint32_t)image_size64;
  result = NULL;
  ok = 1;

cleanup:
  free(result);
  free(file);
  free(engine);
  free(song);
  free(descriptor_data);
  free(compact_song);
  return ok;
}

int ksp_build_kss_image(const char *path, const KSP_INDEX *index,
                        uint8_t **image, uint32_t *image_size,
                        char *error, size_t error_size) {
  return ksp_build_kss_image_for_song(path, index, 0, image, image_size,
                                      error, error_size);
}

void ksp_free_index(KSP_INDEX *index) {
  if (index) {
    free(index->entries);
    memset(index, 0, sizeof(*index));
  }
}
