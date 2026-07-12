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

unsigned char* zx0_output_data;
int zx0_output_index;
int zx0_input_index;
int zx0_bit_index;
int zx0_bit_mask;
int zx0_diff;
int zx0_backtrack;

static void zx0_read_bytes(int n, int *delta) {
    zx0_input_index += n;
    zx0_diff += n;
    if (*delta < zx0_diff)
        *delta = zx0_diff;
}

static void zx0_write_byte(int value) {
    zx0_output_data[zx0_output_index++] = value;
    zx0_diff--;
}

static void zx0_write_bit(int value) {
    if (zx0_backtrack) {
        if (value)
            zx0_output_data[zx0_output_index-1] |= 1;
        zx0_backtrack = ZX0_FALSE;
    } else {
        if (!zx0_bit_mask) {
            zx0_bit_mask = 128;
            zx0_bit_index = zx0_output_index;
            zx0_write_byte(0);
        }
        if (value)
            zx0_output_data[zx0_bit_index] |= zx0_bit_mask;
        zx0_bit_mask >>= 1;
    }
}

static void zx0_write_interlaced_elias_gamma(int value, int backwards_mode, int invert_mode) {
    int i;

    for (i = 2; i <= value; i <<= 1)
        ;
    i >>= 1;
    while (i >>= 1) {
        zx0_write_bit(backwards_mode);
        zx0_write_bit(invert_mode ? !(value & i) : (value & i));
    }
    zx0_write_bit(!backwards_mode);
}

unsigned char *zx0_compress_blocks(ZX0_BLOCK *optimal, unsigned char *input_data, int input_size, int skip, int backwards_mode, int invert_mode, int *output_size, int *delta) {
    ZX0_BLOCK *prev;
    ZX0_BLOCK *next;
    int last_offset = ZX0_INITIAL_OFFSET;
    int length;
    int i;

    /* calculate and allocate output buffer */
    *output_size = (optimal->bits+25)/8;
    zx0_output_data = (unsigned char *)malloc(*output_size);
    if (!zx0_output_data) {
         fprintf(stderr, "Error: Insufficient memory\n");
         exit(1);
    }

    /* un-reverse optimal sequence */
    prev = NULL;
    while (optimal) {
        next = optimal->chain;
        optimal->chain = prev;
        prev = optimal;
        optimal = next;
    }

    /* initialize data */
    zx0_diff = *output_size-input_size+skip;
    *delta = 0;
    zx0_input_index = skip;
    zx0_output_index = 0;
    zx0_bit_mask = 0;
    zx0_backtrack = ZX0_TRUE;

    /* generate output */
    for (optimal = prev->chain; optimal; prev=optimal, optimal = optimal->chain) {
        length = optimal->index-prev->index;

        if (!optimal->offset) {
            /* copy literals indicator */
            zx0_write_bit(0);

            /* copy literals length */
            zx0_write_interlaced_elias_gamma(length, backwards_mode, ZX0_FALSE);

            /* copy literals values */
            for (i = 0; i < length; i++) {
                zx0_write_byte(input_data[zx0_input_index]);
                zx0_read_bytes(1, delta);
            }
        } else if (optimal->offset == last_offset) {
            /* copy from last offset indicator */
            zx0_write_bit(0);

            /* copy from last offset length */
            zx0_write_interlaced_elias_gamma(length, backwards_mode, ZX0_FALSE);
            zx0_read_bytes(length, delta);
        } else {
            /* copy from new offset indicator */
            zx0_write_bit(1);

            /* copy from new offset MSB */
            zx0_write_interlaced_elias_gamma((optimal->offset-1)/128+1, backwards_mode, invert_mode);

            /* copy from new offset LSB */
            if (backwards_mode)
                zx0_write_byte(((optimal->offset-1)%128)<<1);
            else
                zx0_write_byte((127-(optimal->offset-1)%128)<<1);

            /* copy from new offset length */
            zx0_backtrack = ZX0_TRUE;
            zx0_write_interlaced_elias_gamma(length-1, backwards_mode, ZX0_FALSE);
            zx0_read_bytes(length, delta);

            last_offset = optimal->offset;
        }
    }

    /* end marker */
    zx0_write_bit(1);
    zx0_write_interlaced_elias_gamma(256, backwards_mode, invert_mode);

    /* done! */
    return zx0_output_data;
}
