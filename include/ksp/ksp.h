#ifndef KSP_KSP_H
#define KSP_KSP_H

#include <stddef.h>
#include <stdint.h>

#define KSP_TRAILER_SIZE 24u
#define KSP_DIRECTORY_HEADER_SIZE 16u
#define KSP_DIRECTORY_ENTRY_SIZE 32u

typedef struct {
  char type[4];
  uint32_t id;
  const uint8_t *data;
  uint32_t size;
} KSP_CHUNK;

typedef struct {
  char type[5];
  uint32_t id;
  uint32_t offset;
  uint32_t packed_size;
  uint32_t unpacked_size;
  uint32_t crc32;
  uint16_t compression;
  uint16_t flags;
  uint32_t aux;
} KSP_ENTRY;

typedef struct {
  uint32_t file_size;
  uint32_t directory_offset;
  uint32_t directory_size;
  uint32_t entry_count;
  KSP_ENTRY *entries;
} KSP_INDEX;

/* Write a KSP file containing an already-valid KSSX prefix and uncompressed
 * chunks. The caller retains ownership of all input buffers. */
int ksp_write_file(const char *output_path, const uint8_t *kss_prefix,
                   uint32_t kss_prefix_size, const KSP_CHUNK *chunks,
                   uint32_t chunk_count, char *error, size_t error_size);

/* Read and validate the KSP trailer, directory, chunk bounds, and CRCs. */
int ksp_validate_file(const char *path, int verify_crc, KSP_INDEX *index,
                      char *error, size_t error_size);

int ksp_read_chunk(const char *path, const KSP_ENTRY *entry, uint8_t **data,
                   char *error, size_t error_size);

void ksp_free_index(KSP_INDEX *index);
uint32_t ksp_crc32(const uint8_t *data, size_t size);

#endif
