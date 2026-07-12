/*
 * (c) Copyright 2021 by Einar Saukas. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * The name of its author may not be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>

#include "zx0.h"

#define QTY_BLOCKS 10000

ZX0_BLOCK *zx0_ghost_root = NULL;
ZX0_BLOCK *zx0_dead_array = NULL;
int zx0_dead_array_size = 0;

ZX0_BLOCK *zx0_allocate(int bits, int index, int offset, ZX0_BLOCK *chain) {
    ZX0_BLOCK *ptr;

    if (zx0_ghost_root) {
        ptr = zx0_ghost_root;
        zx0_ghost_root = ptr->ghost_chain;
        if (ptr->chain && !--ptr->chain->references) {
            ptr->chain->ghost_chain = zx0_ghost_root;
            zx0_ghost_root = ptr->chain;
        }
    } else {
        if (!zx0_dead_array_size) {
            zx0_dead_array = (ZX0_BLOCK *)malloc(QTY_BLOCKS*sizeof(ZX0_BLOCK));
            if (!zx0_dead_array) {
                fprintf(stderr, "Error: Insufficient memory\n");
                exit(1);
            }
            zx0_dead_array_size = QTY_BLOCKS;
        }
        ptr = &zx0_dead_array[--zx0_dead_array_size];
    }
    ptr->bits = bits;
    ptr->index = index;
    ptr->offset = offset;
    if (chain)
        chain->references++;
    ptr->chain = chain;
    ptr->references = 0;
    return ptr;
}

void zx0_assign(ZX0_BLOCK **ptr, ZX0_BLOCK *chain) {
    chain->references++;
    if (*ptr && !--(*ptr)->references) {
        (*ptr)->ghost_chain = zx0_ghost_root;
        zx0_ghost_root = *ptr;
    }
    *ptr = chain;
}
