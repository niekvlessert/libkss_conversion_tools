#ifndef KSP_KSP_H
#define KSP_KSP_H

#include <stddef.h>
#include <stdint.h>

#define KSP_TRAILER_SIZE 24u
#define KSP_KSS_HEADER_SIZE 0x20u
#define KSP_DIRECTORY_HEADER_SIZE 16u
#define KSP_DIRECTORY_ENTRY_SIZE 32u
#define KSP_COMPRESSION_NONE 0u
#define KSP_COMPRESSION_ZX0 1u

typedef struct {
  char type[4];
  uint32_t id;
  const uint8_t *data;
  uint32_t size;
  /* Requested compression. The writer may leave a chunk uncompressed when
   * compression would not make it smaller. ZX0 is supported for ENGN and
   * SONG chunks only. */
  uint16_t compression;
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

/* Write a compact KSP file. The first 0x20 bytes of the supplied KSSX image
 * are retained as the KSSX header; its load image is reconstructed from the
 * ENGN/SONG chunks when a player opens the KSP. The caller retains ownership
 * of all input buffers. */
int ksp_write_file(const char *output_path, const uint8_t *kss_prefix,
                   uint32_t kss_prefix_size, const KSP_CHUNK *chunks,
                   uint32_t chunk_count, char *error, size_t error_size);

/* Read and validate the KSP trailer, directory, chunk bounds, and CRCs. */
int ksp_validate_file(const char *path, int verify_crc, KSP_INDEX *index,
                      char *error, size_t error_size);

int ksp_read_chunk(const char *path, const KSP_ENTRY *entry, uint8_t **data,
                   char *error, size_t error_size);

int ksp_index_is_compact(const KSP_INDEX *index);

/* Materialize the KSS image described by a compact KSP. The returned buffer
 * is owned by the caller and is suitable for KSS_bin2kss(). */
int ksp_build_kss_image(const char *path, const KSP_INDEX *index,
                        uint8_t **image, uint32_t *image_size,
                        char *error, size_t error_size);

void ksp_free_index(KSP_INDEX *index);
uint32_t ksp_crc32(const uint8_t *data, size_t size);

#endif
