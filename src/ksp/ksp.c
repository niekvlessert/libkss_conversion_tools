#include "ksp/ksp.h"
#include "ksp/engine.h"

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
  uint64_t directory_offset64 = kss_prefix_size;
  uint32_t *offsets = NULL;
  uint32_t i;
  uint32_t cursor;
  FILE *file = NULL;
  uint8_t header[KSP_DIRECTORY_HEADER_SIZE];
  uint8_t entry[KSP_DIRECTORY_ENTRY_SIZE];
  uint8_t trailer[KSP_TRAILER_SIZE];

  if (!kss_prefix || kss_prefix_size < 0x20 ||
      memcmp(kss_prefix, "KSSX", 4) != 0) {
    fail(error, error_size, "KSS prefix is not a KSSX image");
    return 0;
  }
  if (chunk_count == 0 || directory_size64 > UINT32_MAX ||
      directory_offset64 + directory_size64 + KSP_TRAILER_SIZE > UINT32_MAX) {
    fail(error, error_size, "KSP directory is too large");
    return 0;
  }

  offsets = (uint32_t *)calloc(chunk_count, sizeof(*offsets));
  if (!offsets) {
    fail(error, error_size, "out of memory");
    return 0;
  }
  cursor = kss_prefix_size;
  for (i = 0; i < chunk_count; i++) {
    if (chunks[i].size > UINT32_MAX - cursor) {
      fail(error, error_size, "KSP chunk layout overflows");
      free(offsets);
      return 0;
    }
    if (chunks[i].size && !chunks[i].data) {
      fail(error, error_size, "KSP chunk has no data");
      free(offsets);
      return 0;
    }
    offsets[i] = cursor;
    cursor += chunks[i].size;
  }
  directory_offset64 = cursor;

  file = fopen(output_path, "wb");
  if (!file) {
    snprintf(error, error_size, "could not create %s", output_path);
    free(offsets);
    return 0;
  }
  if (!write_all(file, kss_prefix, kss_prefix_size))
    goto write_error;
  for (i = 0; i < chunk_count; i++)
    if (!write_all(file, chunks[i].data, chunks[i].size))
      goto write_error;

  memcpy(header, "KDIR", 4);
  put16(header + 4, KSP_DIRECTORY_HEADER_SIZE);
  put16(header + 6, KSP_DIRECTORY_ENTRY_SIZE);
  put32(header + 8, chunk_count);
  put32(header + 12, 0);
  if (!write_all(file, header, sizeof(header)))
    goto write_error;

  for (i = 0; i < chunk_count; i++) {
    memset(entry, 0, sizeof(entry));
    memcpy(entry, chunks[i].type, 4);
    put32(entry + 4, chunks[i].id);
    put32(entry + 8, offsets[i]);
    put32(entry + 12, chunks[i].size);
    put32(entry + 16, chunks[i].size);
    put32(entry + 20, ksp_crc32(chunks[i].data, chunks[i].size));
    put16(entry + 24, 0);
    put16(entry + 26, 0);
    put32(entry + 28, 0);
    if (!write_all(file, entry, sizeof(entry)))
      goto write_error;
  }

  memcpy(trailer, "KSP1", 4);
  put16(trailer + 4, KSP_TRAILER_SIZE);
  put16(trailer + 6, 0);
  put32(trailer + 8, (uint32_t)directory_offset64);
  put32(trailer + 12, (uint32_t)directory_size64);
  /* The directory CRC covers the header and all entries. */
  {
    uint8_t *directory = (uint8_t *)malloc((size_t)directory_size64);
    uint32_t crc;
    if (!directory) goto write_error;
    memcpy(directory, header, sizeof(header));
    for (i = 0; i < chunk_count; i++) {
      uint8_t *p = directory + KSP_DIRECTORY_HEADER_SIZE +
                   i * KSP_DIRECTORY_ENTRY_SIZE;
      memset(p, 0, KSP_DIRECTORY_ENTRY_SIZE);
      memcpy(p, chunks[i].type, 4);
      put32(p + 4, chunks[i].id);
      put32(p + 8, offsets[i]);
      put32(p + 12, chunks[i].size);
      put32(p + 16, chunks[i].size);
      put32(p + 20, ksp_crc32(chunks[i].data, chunks[i].size));
    }
    crc = ksp_crc32(directory, (size_t)directory_size64);
    free(directory);
    put32(trailer + 16, crc);
  }
  put32(trailer + 20, 0);
  if (!write_all(file, trailer, sizeof(trailer)) || fclose(file) != 0) {
    file = NULL;
    goto write_error;
  }
  free(offsets);
  return 1;

write_error:
  if (file) fclose(file);
  free(offsets);
  fail(error, error_size, "could not write KSP file");
  return 0;
}

static int ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b,
                          uint32_t b_size) {
  uint64_t a_end = (uint64_t)a + a_size;
  uint64_t b_end = (uint64_t)b + b_size;
  return a < b_end && b < a_end;
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
    uint32_t offset = get32(p + 8);
    uint32_t packed_size = get32(p + 12);
    uint32_t unpacked_size = get32(p + 16);
    uint16_t compression = get16(p + 24);
    uint32_t crc = get32(p + 20);
    uint32_t j;
    if (compression != 0 || packed_size != unpacked_size ||
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
    if (verify_crc && ksp_crc32(file + offset, unpacked_size) != crc) {
      fail(error, error_size, "KSP chunk CRC mismatch");
      ksp_free_index(index);
      free(file);
      return 0;
    }
    if (!memcmp(p, "ENGN", 4) && get32(p + 4) == 0) has_engine = 1;
    if (!memcmp(p, "SONG", 4) && get32(p + 4) == 0) has_song = 1;
    if (!memcmp(p, "EDES", 4) && get32(p + 4) == 0) {
      if (!ksp_validate_engine_descriptor(file + offset, unpacked_size,
                                          error, error_size)) {
        ksp_free_index(index);
        free(file);
        return 0;
      }
      has_descriptor = 1;
    }
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
  uint8_t *buffer;
  long file_size;

  if (!entry || !data || entry->compression != 0 ||
      entry->packed_size != entry->unpacked_size) {
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
  buffer = (uint8_t *)malloc(entry->unpacked_size ? entry->unpacked_size : 1);
  if (!buffer || (entry->unpacked_size &&
                  fread(buffer, 1, entry->unpacked_size, file) != entry->unpacked_size)) {
    free(buffer);
    fclose(file);
    fail(error, error_size, "could not read KSP chunk");
    return 0;
  }
  fclose(file);
  if (ksp_crc32(buffer, entry->unpacked_size) != entry->crc32) {
    free(buffer);
    fail(error, error_size, "KSP chunk CRC mismatch");
    return 0;
  }
  *data = buffer;
  return 1;
}

void ksp_free_index(KSP_INDEX *index) {
  if (index) {
    free(index->entries);
    memset(index, 0, sizeof(*index));
  }
}
