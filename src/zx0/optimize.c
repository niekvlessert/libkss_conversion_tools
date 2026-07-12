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

#define MAX_SCALE 50

static int zx0_offset_ceiling(int index, int offset_limit) {
    return index > offset_limit ? offset_limit : index < ZX0_INITIAL_OFFSET ? ZX0_INITIAL_OFFSET : index;
}

static int zx0_elias_gamma_bits(int value) {
    int bits = 1;
    while (value >>= 1)
        bits += 2;
    return bits;
}

ZX0_BLOCK* zx0_optimize(unsigned char *input_data, int input_size, int skip, int offset_limit) {
    ZX0_BLOCK **last_literal;
    ZX0_BLOCK **last_match;
    ZX0_BLOCK **optimal;
    int* match_length;
    int* best_length;
    int best_length_size;
    int bits;
    int index;
    int offset;
    int length;
    int bits2;
    int max_offset = zx0_offset_ceiling(input_size-1, offset_limit);

    /* allocate all main data structures at once */
    last_literal = (ZX0_BLOCK **)calloc(max_offset+1, sizeof(ZX0_BLOCK *));
    last_match = (ZX0_BLOCK **)calloc(max_offset+1, sizeof(ZX0_BLOCK *));
    optimal = (ZX0_BLOCK **)calloc(input_size, sizeof(ZX0_BLOCK *));
    match_length = (int *)calloc(max_offset+1, sizeof(int));
    best_length = (int *)malloc(input_size*sizeof(int));
    if (!last_literal || !last_match || !optimal || !match_length || !best_length) {
        fprintf(stderr, "Error: Insufficient memory\n");
        exit(1);
    }
    if (input_size > 2)
        best_length[2] = 2;

    /* start with fake block */
    zx0_assign(&last_match[ZX0_INITIAL_OFFSET], zx0_allocate(-1, skip-1, ZX0_INITIAL_OFFSET, NULL));

    /* process remaining bytes */
    for (index = skip; index < input_size; index++) {
        best_length_size = 2;
        max_offset = zx0_offset_ceiling(index, offset_limit);
        for (offset = 1; offset <= max_offset; offset++) {
            if (index != skip && index >= offset && input_data[index] == input_data[index-offset]) {
                /* copy from last offset */
                if (last_literal[offset]) {
                    length = index-last_literal[offset]->index;
                    bits = last_literal[offset]->bits + 1 + zx0_elias_gamma_bits(length);
                    zx0_assign(&last_match[offset], zx0_allocate(bits, index, offset, last_literal[offset]));
                    if (!optimal[index] || optimal[index]->bits > bits)
                        zx0_assign(&optimal[index], last_match[offset]);
                }
                /* copy from new offset */
                if (++match_length[offset] > 1) {
                    if (best_length_size < match_length[offset]) {
                        bits = optimal[index-best_length[best_length_size]]->bits + zx0_elias_gamma_bits(best_length[best_length_size]-1);
                        do {
                            best_length_size++;
                            bits2 = optimal[index-best_length_size]->bits + zx0_elias_gamma_bits(best_length_size-1);
                            if (bits2 <= bits) {
                                best_length[best_length_size] = best_length_size;
                                bits = bits2;
                            } else {
                                best_length[best_length_size] = best_length[best_length_size-1];
                            }
                        } while(best_length_size < match_length[offset]);
                    }
                    length = best_length[match_length[offset]];
                    bits = optimal[index-length]->bits + 8 + zx0_elias_gamma_bits((offset-1)/128+1) + zx0_elias_gamma_bits(length-1);
                    if (!last_match[offset] || last_match[offset]->index != index || last_match[offset]->bits > bits) {
                        zx0_assign(&last_match[offset], zx0_allocate(bits, index, offset, optimal[index-length]));
                        if (!optimal[index] || optimal[index]->bits > bits)
                            zx0_assign(&optimal[index], last_match[offset]);
                    }
                }
            } else {
                /* copy literals */
                match_length[offset] = 0;
                if (last_match[offset]) {
                    length = index-last_match[offset]->index;
                    bits = last_match[offset]->bits + 1 + zx0_elias_gamma_bits(length) + length*8;
                    zx0_assign(&last_literal[offset], zx0_allocate(bits, index, 0, last_match[offset]));
                    if (!optimal[index] || optimal[index]->bits > bits)
                        zx0_assign(&optimal[index], last_literal[offset]);
                }
            }
        }

    }

    return optimal[input_size-1];
}
