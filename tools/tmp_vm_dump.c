#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "kss/kss.h"
#include "vm/vm.h"

int main(void) {
    uint8_t *data = NULL;
    KSS *kss;
    VM *vm;
    uint32_t i;
    kss = KSS_load_file("msx/F1.KSS");
    if (!kss) return 1;
    vm = VM_new(44100);
    VM_init_memory(vm, kss->ram_mode, kss->load_adr, kss->load_len,
                   kss->data + (kss->kssx ? 0x20 : 0x10));
    VM_init_bank(vm, kss->bank_mode, kss->bank_num, kss->bank_offset,
                 kss->data + (kss->kssx ? 0x20 : 0x10) + kss->load_len);
    VM_reset_device(vm);
    VM_reset(vm, MSX_CLK, kss->init_adr, kss->play_adr, 60.0, 54,
             kss->DA8_enable);
    printf("header load=%04X len=%04X init=%04X play=%04X banks=%u mode=%u\n",
           kss->load_adr, kss->load_len, kss->init_adr, kss->play_adr,
           kss->bank_num, kss->bank_mode);
    for (i = 0; i < 0x80; ++i) printf("%02X", (unsigned)MMAP_read_memory(vm->mmap, 0xE000 + i));
    putchar('\n');
    for (i = 0; i < 16; ++i) {
        VM_exec(vm, MSX_CLK / 60);
        printf("%u e:", i);
        for (uint32_t j = 0; j < 32; ++j) printf("%02X", (unsigned)MMAP_read_memory(vm->mmap, 0xE000 + j));
        printf(" p:");
        for (uint32_t j = 0; j < 16; ++j) printf("%02X", (unsigned)PSG_readReg(vm->psg, j));
        putchar('\n');
    }
    VM_delete(vm);
    KSS_delete(kss);
    return 0;
}
