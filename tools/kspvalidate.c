#include "ksp/ksp.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  KSP_INDEX index;
  char error[256];
  uint32_t i;
  if (argc != 2) {
    fprintf(stderr, "usage: %s file.ksp\n", argv[0]);
    return 2;
  }
  if (!ksp_validate_file(argv[1], 1, &index, error, sizeof(error))) {
    fprintf(stderr, "invalid KSP: %s\n", error);
    return 1;
  }
  printf("valid KSP1: %u bytes, %u chunks (%s layout)\n", index.file_size,
         index.entry_count,
         ksp_index_is_compact(&index) ? "compact" : "legacy");
  printf("directory: offset=0x%X size=%u\n", index.directory_offset,
         index.directory_size);
  for (i = 0; i < index.entry_count; i++) {
    KSP_ENTRY *entry = &index.entries[i];
    printf("  %s[%u]: offset=0x%X packed=%u unpacked=%u compression=%u crc=%08X\n",
           entry->type, entry->id, entry->offset, entry->packed_size,
           entry->unpacked_size, entry->compression, entry->crc32);
  }
  ksp_free_index(&index);
  return 0;
}
