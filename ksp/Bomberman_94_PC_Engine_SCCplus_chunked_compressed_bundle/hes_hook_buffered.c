#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HES_WRITE_OFFSET 0x202b0u
#define STOLEN 15u

struct trace_rec {
    uint8_t type;      /* 0 marker, 1 write */
    uint8_t reserved0;
    uint16_t addr;
    uint32_t frame;
    uint32_t time;
    uint8_t data;
    uint8_t reserved1[3];
};

static FILE *trace_fp = NULL;
static uint32_t current_frame;
static void (*original_write)(void *, int, int, int);

static void emit_rec(uint8_t type, uint32_t frame, uint32_t time, uint16_t addr, uint8_t data) {
    if (!trace_fp) return;
    struct trace_rec rec = {0};
    rec.type = type;
    rec.addr = addr;
    rec.frame = frame;
    rec.time = time;
    rec.data = data;
    if (fwrite(&rec, sizeof(rec), 1, trace_fp) != 1) _exit(125);
}

__attribute__((visibility("default")))
void hes_trace_marker(uint32_t frame) {
    current_frame = frame;
    emit_rec(0, frame, 0, 0, 0);
}

static void hooked_write(void *apu, int time, int addr, int data) {
    emit_rec(1, current_frame, (uint32_t)time, (uint16_t)addr, (uint8_t)data);
    original_write(apu, time, addr, data);
}

struct find_ctx { uintptr_t base; };
static int find_libgme(struct dl_phdr_info *info, size_t size, void *opaque) {
    (void)size;
    struct find_ctx *ctx = (struct find_ctx *)opaque;
    if (info->dlpi_name && strstr(info->dlpi_name, "libgme.so")) {
        ctx->base = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

static void absolute_jump(uint8_t *p, const void *target) {
    /* movabs rax, imm64 ; jmp rax */
    p[0] = 0x48; p[1] = 0xB8;
    uintptr_t value = (uintptr_t)target;
    memcpy(p + 2, &value, 8);
    p[10] = 0xFF; p[11] = 0xE0;
}

__attribute__((constructor))
static void install_hook(void) {
    const char *path = getenv("HES_TRACE_FILE");
    if (!path || !*path) return;
    trace_fp = fopen(path, "wb");
    if (!trace_fp) { perror("fopen HES_TRACE_FILE"); _exit(120); }
    static char trace_buffer[8 * 1024 * 1024];
    setvbuf(trace_fp, trace_buffer, _IOFBF, sizeof(trace_buffer));

    struct find_ctx ctx = {0};
    dl_iterate_phdr(find_libgme, &ctx);
    if (!ctx.base) { fprintf(stderr, "hes hook: libgme not found\n"); _exit(121); }
    uint8_t *target = (uint8_t *)(ctx.base + HES_WRITE_OFFSET);
    const uint8_t expected[STOLEN] = {
        0xF3,0x0F,0x1E,0xFA, 0x41,0x56, 0x41,0x55, 0x41,0x54,
        0x55, 0x53, 0x48,0x89,0xFB
    };
    if (memcmp(target, expected, STOLEN) != 0) {
        fprintf(stderr, "hes hook: unexpected libgme bytes at %p\n", target);
        for (unsigned i=0;i<STOLEN;i++) fprintf(stderr, "%02x%s", target[i], i+1==STOLEN?"\n":" ");
        _exit(122);
    }

    uint8_t *tramp = mmap(NULL, 64, PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) { perror("mmap trampoline"); _exit(123); }
    memcpy(tramp, target, STOLEN);
    absolute_jump(tramp + STOLEN, target + STOLEN);
    __builtin___clear_cache((char *)tramp, (char *)tramp + STOLEN + 12);
    original_write = (void (*)(void *, int, int, int))tramp;

    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~((uintptr_t)page_size - 1);
    if (mprotect((void *)page, (size_t)page_size, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
        perror("mprotect target"); _exit(124);
    }
    absolute_jump(target, hooked_write);
    for (unsigned i=12;i<STOLEN;i++) target[i] = 0x90;
    __builtin___clear_cache((char *)target, (char *)target + STOLEN);
    mprotect((void *)page, (size_t)page_size, PROT_READ|PROT_EXEC);
}

__attribute__((destructor))
static void close_trace(void) {
    if (trace_fp) fclose(trace_fp);
}
