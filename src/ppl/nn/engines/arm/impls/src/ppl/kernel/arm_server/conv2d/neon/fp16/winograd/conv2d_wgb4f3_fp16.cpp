// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifdef PPLNN_USE_ARMV8_2_FP16

#include "ppl/kernel/arm_server/conv2d/neon/fp16/winograd/conv2d_wgb4f3_fp16.h"

#include <arm_neon.h>
#include <chrono>
#include <malloc.h>

#include "ppl/kernel/arm_server/conv2d/neon/fp16/n8cx_hgemm/n8cx_hgemm.h"
#include "ppl/kernel/arm_server/common/internal_include.h"

namespace ppl { namespace kernel { namespace arm_server { namespace neon {

#define CBLK()  8
#define ICBLK() CBLK()
#define OCBLK() CBLK()

#define WINOGRAD_B4F3_OUTPUT_BLOCKSIZE() 4
#define WINOGRAD_B4F3_INPUT_BLOCKSIZE()  6
#define WINOGRAD_B4F3_NUM_SET()          36

#define WGB4F3_OBLK() WINOGRAD_B4F3_OUTPUT_BLOCKSIZE()
#define WGB4F3_IBLK() WINOGRAD_B4F3_INPUT_BLOCKSIZE()
#define WGB4F3_NSET() WINOGRAD_B4F3_NUM_SET()

#define N8CX_HGEMM_N10_BLOCK0() 10
#define N8CX_HGEMM_N12_BLOCK0() 12

#define LLC_CACHELINE_SIZE() 128

static inline size_t conv_winograd_b4f3_get_input_buffer_size_fp16(
    const int64_t channels,
    const int64_t tile_l2_size)
{
    /* inner parallel mode */
    const int64_t num_wg_blocks    = tile_l2_size;
    const size_t input_buffer_size = CEIL128(WGB4F3_NSET() * CEIL8(channels) * num_wg_blocks * sizeof(__fp16)) + LLC_CACHELINE_SIZE();
    return input_buffer_size;
}

static inline size_t conv_winograd_b4f3_get_output_buffer_size_fp16(
    const int64_t num_output,
    const int64_t tile_l2_size)
{
    /* inner parallel mode */
    const int64_t num_wg_blocks     = tile_l2_size;
    const size_t output_buffer_size = CEIL128(WGB4F3_NSET() * CEIL8(num_output) * num_wg_blocks * sizeof(__fp16)) + LLC_CACHELINE_SIZE();
    return output_buffer_size;
}

static inline void conv2d_n8cx_wgb4f3_prep_input_block_fp16(
    const __fp16 *input_block,
    const float16x8_t &vzeros,
    const float16x8_t &vcoeff,
    __fp16 *prep_input_block,
    const int64_t src_w,
    const int64_t in_wg_set_offset,
    const bool ih_valid[WGB4F3_IBLK()],
    const bool iw_valid[WGB4F3_IBLK()])
{
    float16x8_t v[24];
    float16x8_t vp[6];

    // D[0][:]
    if (ih_valid[0]) {
        const __fp16 *input_base = input_block;
        v[0]                     = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[1]                     = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[2]                     = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[3]                     = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[4]                     = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[5]                     = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[0] = vzeros;
        v[1] = vzeros;
        v[2] = vzeros;
        v[3] = vzeros;
        v[4] = vzeros;
        v[5] = vzeros;
    }

    vp[0] = vmulq_laneq_f16(v[0], vcoeff, 2);
    vp[1] = vmulq_laneq_f16(v[1], vcoeff, 2);
    vp[2] = vmulq_laneq_f16(v[2], vcoeff, 2);
    vp[3] = vmulq_laneq_f16(v[3], vcoeff, 2);
    vp[4] = vmulq_laneq_f16(v[4], vcoeff, 2);
    vp[5] = vmulq_laneq_f16(v[5], vcoeff, 2);

    // D[2][:]
    if (ih_valid[2]) {
        const __fp16 *input_base = input_block + 2 * src_w * ICBLK();
        v[6]                     = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[7]                     = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[8]                     = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[9]                     = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[10]                    = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[11]                    = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[6]  = vzeros;
        v[7]  = vzeros;
        v[8]  = vzeros;
        v[9]  = vzeros;
        v[10] = vzeros;
        v[11] = vzeros;
    }

    vp[0] = vfmsq_laneq_f16(vp[0], v[6], vcoeff, 3);
    vp[1] = vfmsq_laneq_f16(vp[1], v[7], vcoeff, 3);
    vp[2] = vfmsq_laneq_f16(vp[2], v[8], vcoeff, 3);
    vp[3] = vfmsq_laneq_f16(vp[3], v[9], vcoeff, 3);
    vp[4] = vfmsq_laneq_f16(vp[4], v[10], vcoeff, 3);
    vp[5] = vfmsq_laneq_f16(vp[5], v[11], vcoeff, 3);

    if (ih_valid[4]) {
        const __fp16 *input_base = input_block + 4 * src_w * ICBLK();
        v[18]                    = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[19]                    = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[20]                    = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[21]                    = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[22]                    = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[23]                    = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[18] = vzeros;
        v[19] = vzeros;
        v[20] = vzeros;
        v[21] = vzeros;
        v[22] = vzeros;
        v[23] = vzeros;
    }

    vp[0] = vaddq_f16(vp[0], v[18]);
    vp[1] = vaddq_f16(vp[1], v[19]);
    vp[2] = vaddq_f16(vp[2], v[20]);
    vp[3] = vaddq_f16(vp[3], v[21]);
    vp[4] = vaddq_f16(vp[4], v[22]);
    vp[5] = vaddq_f16(vp[5], v[23]);

    // vp[0~5] -> v[0~5]
    v[1]  = vaddq_f16(vp[3], vp[4]);
    v[2]  = vsubq_f16(vp[4], vp[3]);
    v[3]  = vsubq_f16(vp[4], vp[2]);
    v[4]  = vsubq_f16(vp[4], vp[2]);
    vp[4] = vfmaq_laneq_f16(vp[4], vp[0], vcoeff, 2);
    vp[5] = vfmaq_laneq_f16(vp[5], vp[1], vcoeff, 2);
    v[1]  = vfmsq_laneq_f16(v[1], vp[1], vcoeff, 2);
    v[2]  = vfmaq_laneq_f16(v[2], vp[1], vcoeff, 2);
    v[3]  = vfmsq_laneq_f16(v[3], vp[1], vcoeff, 1);
    v[4]  = vfmaq_laneq_f16(v[4], vp[1], vcoeff, 1);
    vp[4] = vfmsq_laneq_f16(vp[4], vp[2], vcoeff, 3);
    vp[5] = vfmsq_laneq_f16(vp[5], vp[3], vcoeff, 3);
    v[1]  = vfmsq_laneq_f16(v[1], vp[2], vcoeff, 2);
    v[2]  = vfmsq_laneq_f16(v[2], vp[2], vcoeff, 2);
    v[3]  = vfmaq_laneq_f16(v[3], vp[3], vcoeff, 1);
    v[4]  = vfmsq_laneq_f16(v[4], vp[3], vcoeff, 1);

    // D[3][:]
    if (ih_valid[3]) {
        const __fp16 *input_base = input_block + 3 * src_w * ICBLK();
        v[12]                    = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[13]                    = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[14]                    = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[15]                    = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[16]                    = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[17]                    = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[12] = vzeros;
        v[13] = vzeros;
        v[14] = vzeros;
        v[15] = vzeros;
        v[16] = vzeros;
        v[17] = vzeros;
    }

    vst1q_f16(prep_input_block + 0 * in_wg_set_offset, vp[4]);
    vst1q_f16(prep_input_block + 1 * in_wg_set_offset, v[1]);
    vst1q_f16(prep_input_block + 2 * in_wg_set_offset, v[2]);
    vst1q_f16(prep_input_block + 3 * in_wg_set_offset, v[3]);
    vst1q_f16(prep_input_block + 4 * in_wg_set_offset, v[4]);
    vst1q_f16(prep_input_block + 5 * in_wg_set_offset, vp[5]);

    // D[1][:]
    if (ih_valid[1]) {
        const __fp16 *input_base = input_block + src_w * ICBLK();
        v[0]                     = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[1]                     = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[2]                     = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[3]                     = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[4]                     = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[5]                     = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[0] = vzeros;
        v[1] = vzeros;
        v[2] = vzeros;
        v[3] = vzeros;
        v[4] = vzeros;
        v[5] = vzeros;
    }

    // (x, -1, x, 1)
    vp[0] = vsubq_f16(v[18], v[6]);
    vp[1] = vsubq_f16(v[19], v[7]);
    vp[2] = vsubq_f16(v[20], v[8]);
    vp[3] = vsubq_f16(v[21], v[9]);
    vp[4] = vsubq_f16(v[22], v[10]);
    vp[5] = vsubq_f16(v[23], v[11]);

    // (x, -4, x, 1)
    v[18] = vfmsq_laneq_f16(v[18], v[6], vcoeff, 2);
    v[19] = vfmsq_laneq_f16(v[19], v[7], vcoeff, 2);
    v[20] = vfmsq_laneq_f16(v[20], v[8], vcoeff, 2);
    v[21] = vfmsq_laneq_f16(v[21], v[9], vcoeff, 2);
    v[22] = vfmsq_laneq_f16(v[22], v[10], vcoeff, 2);
    v[23] = vfmsq_laneq_f16(v[23], v[11], vcoeff, 2);

    // (1, x, -1, x)
    v[6]  = vsubq_f16(v[0], v[12]);
    v[7]  = vsubq_f16(v[1], v[13]);
    v[8]  = vsubq_f16(v[2], v[14]);
    v[9]  = vsubq_f16(v[3], v[15]);
    v[10] = vsubq_f16(v[4], v[16]);
    v[11] = vsubq_f16(v[5], v[17]);

    // (-4, x, 1, x)
    v[12] = vfmsq_laneq_f16(v[12], v[0], vcoeff, 2);
    v[13] = vfmsq_laneq_f16(v[13], v[1], vcoeff, 2);
    v[14] = vfmsq_laneq_f16(v[14], v[2], vcoeff, 2);
    v[15] = vfmsq_laneq_f16(v[15], v[3], vcoeff, 2);
    v[16] = vfmsq_laneq_f16(v[16], v[4], vcoeff, 2);
    v[17] = vfmsq_laneq_f16(v[17], v[5], vcoeff, 2);

    // (2, x, -2, x)
    v[6]  = vmulq_laneq_f16(v[6], vcoeff, 1);
    v[7]  = vmulq_laneq_f16(v[7], vcoeff, 1);
    v[8]  = vmulq_laneq_f16(v[8], vcoeff, 1);
    v[9]  = vmulq_laneq_f16(v[9], vcoeff, 1);
    v[10] = vmulq_laneq_f16(v[10], vcoeff, 1);
    v[11] = vmulq_laneq_f16(v[11], vcoeff, 1);

    // (-4, -4, 1, 1)
    v[0] = vaddq_f16(v[12], v[18]);
    v[1] = vaddq_f16(v[13], v[19]);
    v[2] = vaddq_f16(v[14], v[20]);
    v[3] = vaddq_f16(v[15], v[21]);
    v[4] = vaddq_f16(v[16], v[22]);
    v[5] = vaddq_f16(v[17], v[23]);

    // (4, -4, -1, 1)
    v[12] = vsubq_f16(v[18], v[12]);
    v[13] = vsubq_f16(v[19], v[13]);
    v[14] = vsubq_f16(v[20], v[14]);
    v[15] = vsubq_f16(v[21], v[15]);
    v[16] = vsubq_f16(v[22], v[16]);
    v[17] = vsubq_f16(v[23], v[17]);

    // (2, -1, -2, 1)
    v[18] = vaddq_f16(vp[0], v[6]);
    v[19] = vaddq_f16(vp[1], v[7]);
    v[20] = vaddq_f16(vp[2], v[8]);
    v[21] = vaddq_f16(vp[3], v[9]);
    v[22] = vaddq_f16(vp[4], v[10]);
    v[23] = vaddq_f16(vp[5], v[11]);

    // (-2, -1, 2, 1)
    v[6]  = vsubq_f16(vp[0], v[6]);
    v[7]  = vsubq_f16(vp[1], v[7]);
    v[8]  = vsubq_f16(vp[2], v[8]);
    v[9]  = vsubq_f16(vp[3], v[9]);
    v[10] = vsubq_f16(vp[4], v[10]);
    v[11] = vsubq_f16(vp[5], v[11]);

    vp[1] = vaddq_f16(v[3], v[4]);
    vp[2] = vsubq_f16(v[4], v[3]);
    vp[3] = vsubq_f16(v[4], v[2]);
    vp[4] = vsubq_f16(v[4], v[2]);
    v[4]  = vfmaq_laneq_f16(v[4], v[0], vcoeff, 2);
    v[5]  = vfmaq_laneq_f16(v[5], v[1], vcoeff, 2);
    vp[1] = vfmsq_laneq_f16(vp[1], v[1], vcoeff, 2);
    vp[2] = vfmaq_laneq_f16(vp[2], v[1], vcoeff, 2);
    vp[3] = vfmsq_laneq_f16(vp[3], v[1], vcoeff, 1);
    vp[4] = vfmaq_laneq_f16(vp[4], v[1], vcoeff, 1);
    v[4]  = vfmsq_laneq_f16(v[4], v[2], vcoeff, 3);
    v[5]  = vfmsq_laneq_f16(v[5], v[3], vcoeff, 3);
    vp[1] = vfmsq_laneq_f16(vp[1], v[2], vcoeff, 2);
    vp[2] = vfmsq_laneq_f16(vp[2], v[2], vcoeff, 2);
    vp[3] = vfmaq_laneq_f16(vp[3], v[3], vcoeff, 1);
    vp[4] = vfmsq_laneq_f16(vp[4], v[3], vcoeff, 1);

    vst1q_f16(prep_input_block + 6 * in_wg_set_offset, v[4]);
    vst1q_f16(prep_input_block + 7 * in_wg_set_offset, vp[1]);
    vst1q_f16(prep_input_block + 8 * in_wg_set_offset, vp[2]);
    vst1q_f16(prep_input_block + 9 * in_wg_set_offset, vp[3]);
    vst1q_f16(prep_input_block + 10 * in_wg_set_offset, vp[4]);
    vst1q_f16(prep_input_block + 11 * in_wg_set_offset, v[5]);

    if (ih_valid[1]) {
        const __fp16 *input_base = input_block + src_w * ICBLK();
        v[0]                     = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[1]                     = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[2]                     = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[3]                     = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[4]                     = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[5]                     = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[0] = vzeros;
        v[1] = vzeros;
        v[2] = vzeros;
        v[3] = vzeros;
        v[4] = vzeros;
        v[5] = vzeros;
    }

    vp[1] = vaddq_f16(v[15], v[16]);
    vp[2] = vsubq_f16(v[16], v[15]);
    vp[3] = vsubq_f16(v[16], v[14]);
    vp[4] = vsubq_f16(v[16], v[14]);
    v[16] = vfmaq_laneq_f16(v[16], v[12], vcoeff, 2);
    v[17] = vfmaq_laneq_f16(v[17], v[13], vcoeff, 2);
    vp[1] = vfmsq_laneq_f16(vp[1], v[13], vcoeff, 2);
    vp[2] = vfmaq_laneq_f16(vp[2], v[13], vcoeff, 2);
    vp[3] = vfmsq_laneq_f16(vp[3], v[13], vcoeff, 1);
    vp[4] = vfmaq_laneq_f16(vp[4], v[13], vcoeff, 1);
    v[16] = vfmsq_laneq_f16(v[16], v[14], vcoeff, 3);
    v[17] = vfmsq_laneq_f16(v[17], v[15], vcoeff, 3);
    vp[1] = vfmsq_laneq_f16(vp[1], v[14], vcoeff, 2);
    vp[2] = vfmsq_laneq_f16(vp[2], v[14], vcoeff, 2);
    vp[3] = vfmaq_laneq_f16(vp[3], v[15], vcoeff, 1);
    vp[4] = vfmsq_laneq_f16(vp[4], v[15], vcoeff, 1);

    vst1q_f16(prep_input_block + 12 * in_wg_set_offset, v[16]);
    vst1q_f16(prep_input_block + 13 * in_wg_set_offset, vp[1]);
    vst1q_f16(prep_input_block + 14 * in_wg_set_offset, vp[2]);
    vst1q_f16(prep_input_block + 15 * in_wg_set_offset, vp[3]);
    vst1q_f16(prep_input_block + 16 * in_wg_set_offset, vp[4]);
    vst1q_f16(prep_input_block + 17 * in_wg_set_offset, v[17]);

    if (ih_valid[5]) {
        const __fp16 *input_base = input_block + 5 * src_w * ICBLK();
        v[12]                    = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[13]                    = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[14]                    = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[15]                    = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[16]                    = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[17]                    = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[12] = vzeros;
        v[13] = vzeros;
        v[14] = vzeros;
        v[15] = vzeros;
        v[16] = vzeros;
        v[17] = vzeros;
    }

    vp[1] = vaddq_f16(v[9], v[10]);
    vp[2] = vsubq_f16(v[10], v[9]);
    vp[3] = vsubq_f16(v[10], v[8]);
    vp[4] = vsubq_f16(v[10], v[8]);
    v[10] = vfmaq_laneq_f16(v[10], v[6], vcoeff, 2);
    v[11] = vfmaq_laneq_f16(v[11], v[7], vcoeff, 2);
    vp[1] = vfmsq_laneq_f16(vp[1], v[7], vcoeff, 2);
    vp[2] = vfmaq_laneq_f16(vp[2], v[7], vcoeff, 2);
    vp[3] = vfmsq_laneq_f16(vp[3], v[7], vcoeff, 1);
    vp[4] = vfmaq_laneq_f16(vp[4], v[7], vcoeff, 1);
    v[10] = vfmsq_laneq_f16(v[10], v[8], vcoeff, 3);
    v[11] = vfmsq_laneq_f16(v[11], v[9], vcoeff, 3);
    vp[1] = vfmsq_laneq_f16(vp[1], v[8], vcoeff, 2);
    vp[2] = vfmsq_laneq_f16(vp[2], v[8], vcoeff, 2);
    vp[3] = vfmaq_laneq_f16(vp[3], v[9], vcoeff, 1);
    vp[4] = vfmsq_laneq_f16(vp[4], v[9], vcoeff, 1);

    v[12] = vfmaq_laneq_f16(v[12], v[0], vcoeff, 2);
    v[13] = vfmaq_laneq_f16(v[13], v[1], vcoeff, 2);
    v[14] = vfmaq_laneq_f16(v[14], v[2], vcoeff, 2);
    v[15] = vfmaq_laneq_f16(v[15], v[3], vcoeff, 2);
    v[16] = vfmaq_laneq_f16(v[16], v[4], vcoeff, 2);
    v[17] = vfmaq_laneq_f16(v[17], v[5], vcoeff, 2);

    vst1q_f16(prep_input_block + 18 * in_wg_set_offset, v[10]);
    vst1q_f16(prep_input_block + 19 * in_wg_set_offset, vp[1]);
    vst1q_f16(prep_input_block + 20 * in_wg_set_offset, vp[2]);
    vst1q_f16(prep_input_block + 21 * in_wg_set_offset, vp[3]);
    vst1q_f16(prep_input_block + 22 * in_wg_set_offset, vp[4]);
    vst1q_f16(prep_input_block + 23 * in_wg_set_offset, v[11]);

    // D[3][:]
    if (ih_valid[3]) {
        const __fp16 *input_base = input_block + 3 * src_w * ICBLK();
        v[6]                     = iw_valid[0] ? vld1q_f16(input_base) : vzeros;
        v[7]                     = iw_valid[1] ? vld1q_f16(input_base + 1 * ICBLK()) : vzeros;
        v[8]                     = iw_valid[2] ? vld1q_f16(input_base + 2 * ICBLK()) : vzeros;
        v[9]                     = iw_valid[3] ? vld1q_f16(input_base + 3 * ICBLK()) : vzeros;
        v[10]                    = iw_valid[4] ? vld1q_f16(input_base + 4 * ICBLK()) : vzeros;
        v[11]                    = iw_valid[5] ? vld1q_f16(input_base + 5 * ICBLK()) : vzeros;
    } else {
        v[6]  = vzeros;
        v[7]  = vzeros;
        v[8]  = vzeros;
        v[9]  = vzeros;
        v[10] = vzeros;
        v[11] = vzeros;
    }

    vp[1] = vaddq_f16(v[21], v[22]);
    vp[2] = vsubq_f16(v[22], v[21]);
    vp[3] = vsubq_f16(v[22], v[20]);
    vp[4] = vsubq_f16(v[22], v[20]);
    v[22] = vfmaq_laneq_f16(v[22], v[18], vcoeff, 2);
    v[23] = vfmaq_laneq_f16(v[23], v[19], vcoeff, 2);
    vp[1] = vfmsq_laneq_f16(vp[1], v[19], vcoeff, 2);
    vp[2] = vfmaq_laneq_f16(vp[2], v[19], vcoeff, 2);
    vp[3] = vfmsq_laneq_f16(vp[3], v[19], vcoeff, 1);
    vp[4] = vfmaq_laneq_f16(vp[4], v[19], vcoeff, 1);
    v[22] = vfmsq_laneq_f16(v[22], v[20], vcoeff, 3);
    v[23] = vfmsq_laneq_f16(v[23], v[21], vcoeff, 3);
    vp[1] = vfmsq_laneq_f16(vp[1], v[20], vcoeff, 2);
    vp[2] = vfmsq_laneq_f16(vp[2], v[20], vcoeff, 2);
    vp[3] = vfmaq_laneq_f16(vp[3], v[21], vcoeff, 1);
    vp[4] = vfmsq_laneq_f16(vp[4], v[21], vcoeff, 1);

    v[12] = vfmsq_laneq_f16(v[12], v[6], vcoeff, 3);
    v[13] = vfmsq_laneq_f16(v[13], v[7], vcoeff, 3);
    v[14] = vfmsq_laneq_f16(v[14], v[8], vcoeff, 3);
    v[15] = vfmsq_laneq_f16(v[15], v[9], vcoeff, 3);
    v[16] = vfmsq_laneq_f16(v[16], v[10], vcoeff, 3);
    v[17] = vfmsq_laneq_f16(v[17], v[11], vcoeff, 3);

    vst1q_f16(prep_input_block + 24 * in_wg_set_offset, v[22]);
    vst1q_f16(prep_input_block + 25 * in_wg_set_offset, vp[1]);
    vst1q_f16(prep_input_block + 26 * in_wg_set_offset, vp[2]);
    vst1q_f16(prep_input_block + 27 * in_wg_set_offset, vp[3]);
    vst1q_f16(prep_input_block + 28 * in_wg_set_offset, vp[4]);
    vst1q_f16(prep_input_block + 29 * in_wg_set_offset, v[23]);

    vp[1] = vaddq_f16(v[15], v[16]);
    vp[2] = vsubq_f16(v[16], v[15]);
    vp[3] = vsubq_f16(v[16], v[14]);
    vp[4] = vsubq_f16(v[16], v[14]);
    v[16] = vfmaq_laneq_f16(v[16], v[12], vcoeff, 2);
    v[17] = vfmaq_laneq_f16(v[17], v[13], vcoeff, 2);
    vp[1] = vfmsq_laneq_f16(vp[1], v[13], vcoeff, 2);
    vp[2] = vfmaq_laneq_f16(vp[2], v[13], vcoeff, 2);
    vp[3] = vfmsq_laneq_f16(vp[3], v[13], vcoeff, 1);
    vp[4] = vfmaq_laneq_f16(vp[4], v[13], vcoeff, 1);
    v[16] = vfmsq_laneq_f16(v[16], v[14], vcoeff, 3);
    v[17] = vfmsq_laneq_f16(v[17], v[15], vcoeff, 3);
    vp[1] = vfmsq_laneq_f16(vp[1], v[14], vcoeff, 2);
    vp[2] = vfmsq_laneq_f16(vp[2], v[14], vcoeff, 2);
    vp[3] = vfmaq_laneq_f16(vp[3], v[15], vcoeff, 1);
    vp[4] = vfmsq_laneq_f16(vp[4], v[15], vcoeff, 1);

    vst1q_f16(prep_input_block + 30 * in_wg_set_offset, v[16]);
    vst1q_f16(prep_input_block + 31 * in_wg_set_offset, vp[1]);
    vst1q_f16(prep_input_block + 32 * in_wg_set_offset, vp[2]);
    vst1q_f16(prep_input_block + 33 * in_wg_set_offset, vp[3]);
    vst1q_f16(prep_input_block + 34 * in_wg_set_offset, vp[4]);
    vst1q_f16(prep_input_block + 35 * in_wg_set_offset, v[17]);
}

static inline void conv2d_n8cx_wgb4f3_postp_output_block_fp16(
    const __fp16 *raw_output_block,
    const float16x8_t &vbias,
    const float16x8_t &vcoeff,
    __fp16 *output_block, // oc_start biased, oh_start, ow_start biased
    __fp16 *sum_block,
    const int64_t wg_out_set_offset,
    const int64_t dst_w,
    const int64_t num_valid_oh,
    const int64_t num_valid_ow,
    const uint32_t fuse_flag)
{
    float16x8_t vio[6], vmid[24];
    vmid[0] = vld1q_f16(raw_output_block + 0 * wg_out_set_offset);
    vmid[1] = vld1q_f16(raw_output_block + 1 * wg_out_set_offset);
    vmid[2] = vld1q_f16(raw_output_block + 2 * wg_out_set_offset);
    vmid[3] = vld1q_f16(raw_output_block + 3 * wg_out_set_offset);
    vmid[4] = vld1q_f16(raw_output_block + 4 * wg_out_set_offset);
    vmid[5] = vld1q_f16(raw_output_block + 5 * wg_out_set_offset);

    vmid[18] = vld1q_f16(raw_output_block + 30 * wg_out_set_offset);
    vmid[19] = vld1q_f16(raw_output_block + 31 * wg_out_set_offset);
    vmid[20] = vld1q_f16(raw_output_block + 32 * wg_out_set_offset);
    vmid[21] = vld1q_f16(raw_output_block + 33 * wg_out_set_offset);
    vmid[22] = vld1q_f16(raw_output_block + 34 * wg_out_set_offset);
    vmid[23] = vld1q_f16(raw_output_block + 35 * wg_out_set_offset);

    vmid[6]  = vld1q_f16(raw_output_block + 6 * wg_out_set_offset);
    vmid[7]  = vld1q_f16(raw_output_block + 7 * wg_out_set_offset);
    vmid[8]  = vld1q_f16(raw_output_block + 8 * wg_out_set_offset);
    vmid[9]  = vld1q_f16(raw_output_block + 9 * wg_out_set_offset);
    vmid[10] = vld1q_f16(raw_output_block + 10 * wg_out_set_offset);
    vmid[11] = vld1q_f16(raw_output_block + 11 * wg_out_set_offset);

    // r0 + r1
    vmid[0] = vaddq_f16(vmid[0], vmid[6]);
    vmid[1] = vaddq_f16(vmid[1], vmid[7]);
    vmid[2] = vaddq_f16(vmid[2], vmid[8]);
    vmid[3] = vaddq_f16(vmid[3], vmid[9]);
    vmid[4] = vaddq_f16(vmid[4], vmid[10]);
    vmid[5] = vaddq_f16(vmid[5], vmid[11]);

    // r5 + r1
    vmid[18] = vaddq_f16(vmid[18], vmid[6]);
    vmid[19] = vaddq_f16(vmid[19], vmid[7]);
    vmid[20] = vaddq_f16(vmid[20], vmid[8]);
    vmid[21] = vaddq_f16(vmid[21], vmid[9]);
    vmid[22] = vaddq_f16(vmid[22], vmid[10]);
    vmid[23] = vaddq_f16(vmid[23], vmid[11]);

    vio[0] = vld1q_f16(raw_output_block + 12 * wg_out_set_offset);
    vio[1] = vld1q_f16(raw_output_block + 13 * wg_out_set_offset);
    vio[2] = vld1q_f16(raw_output_block + 14 * wg_out_set_offset);
    vio[3] = vld1q_f16(raw_output_block + 15 * wg_out_set_offset);
    vio[4] = vld1q_f16(raw_output_block + 16 * wg_out_set_offset);
    vio[5] = vld1q_f16(raw_output_block + 17 * wg_out_set_offset);

    // r0 + r1 + r2
    vmid[0] = vaddq_f16(vmid[0], vio[0]);
    vmid[1] = vaddq_f16(vmid[1], vio[1]);
    vmid[2] = vaddq_f16(vmid[2], vio[2]);
    vmid[3] = vaddq_f16(vmid[3], vio[3]);
    vmid[4] = vaddq_f16(vmid[4], vio[4]);
    vmid[5] = vaddq_f16(vmid[5], vio[5]);

    // r1 - r2 + r5
    vmid[18] = vsubq_f16(vmid[18], vio[0]);
    vmid[19] = vsubq_f16(vmid[19], vio[1]);
    vmid[20] = vsubq_f16(vmid[20], vio[2]);
    vmid[21] = vsubq_f16(vmid[21], vio[3]);
    vmid[22] = vsubq_f16(vmid[22], vio[4]);
    vmid[23] = vsubq_f16(vmid[23], vio[5]);

    // r1 + r2
    vmid[12] = vaddq_f16(vmid[6], vio[0]);
    vmid[13] = vaddq_f16(vmid[7], vio[1]);
    vmid[14] = vaddq_f16(vmid[8], vio[2]);
    vmid[15] = vaddq_f16(vmid[9], vio[3]);
    vmid[16] = vaddq_f16(vmid[10], vio[4]);
    vmid[17] = vaddq_f16(vmid[11], vio[5]);

    // r1 - r2
    vmid[6]  = vsubq_f16(vmid[6], vio[0]);
    vmid[7]  = vsubq_f16(vmid[7], vio[1]);
    vmid[8]  = vsubq_f16(vmid[8], vio[2]);
    vmid[9]  = vsubq_f16(vmid[9], vio[3]);
    vmid[10] = vsubq_f16(vmid[10], vio[4]);
    vmid[11] = vsubq_f16(vmid[11], vio[5]);

    vio[0] = vld1q_f16(raw_output_block + 18 * wg_out_set_offset);
    vio[1] = vld1q_f16(raw_output_block + 19 * wg_out_set_offset);
    vio[2] = vld1q_f16(raw_output_block + 20 * wg_out_set_offset);
    vio[3] = vld1q_f16(raw_output_block + 21 * wg_out_set_offset);
    vio[4] = vld1q_f16(raw_output_block + 22 * wg_out_set_offset);
    vio[5] = vld1q_f16(raw_output_block + 23 * wg_out_set_offset);

    // r0 + r1 + r2 + r3
    vmid[0] = vaddq_f16(vmid[0], vio[0]);
    vmid[1] = vaddq_f16(vmid[1], vio[1]);
    vmid[2] = vaddq_f16(vmid[2], vio[2]);
    vmid[3] = vaddq_f16(vmid[3], vio[3]);
    vmid[4] = vaddq_f16(vmid[4], vio[4]);
    vmid[5] = vaddq_f16(vmid[5], vio[5]);

    // r1 - r2 + 2*r3
    vmid[6]  = vfmaq_laneq_f16(vmid[6], vio[0], vcoeff, 1);
    vmid[7]  = vfmaq_laneq_f16(vmid[7], vio[1], vcoeff, 1);
    vmid[8]  = vfmaq_laneq_f16(vmid[8], vio[2], vcoeff, 1);
    vmid[9]  = vfmaq_laneq_f16(vmid[9], vio[3], vcoeff, 1);
    vmid[10] = vfmaq_laneq_f16(vmid[10], vio[4], vcoeff, 1);
    vmid[11] = vfmaq_laneq_f16(vmid[11], vio[5], vcoeff, 1);

    // r1 + r2 + 4*r3
    vmid[12] = vfmaq_laneq_f16(vmid[12], vio[0], vcoeff, 2);
    vmid[13] = vfmaq_laneq_f16(vmid[13], vio[1], vcoeff, 2);
    vmid[14] = vfmaq_laneq_f16(vmid[14], vio[2], vcoeff, 2);
    vmid[15] = vfmaq_laneq_f16(vmid[15], vio[3], vcoeff, 2);
    vmid[16] = vfmaq_laneq_f16(vmid[16], vio[4], vcoeff, 2);
    vmid[17] = vfmaq_laneq_f16(vmid[17], vio[5], vcoeff, 2);

    // r1 - r2 + 8*r3 + r5
    vmid[18] = vfmaq_laneq_f16(vmid[18], vio[0], vcoeff, 3);
    vmid[19] = vfmaq_laneq_f16(vmid[19], vio[1], vcoeff, 3);
    vmid[20] = vfmaq_laneq_f16(vmid[20], vio[2], vcoeff, 3);
    vmid[21] = vfmaq_laneq_f16(vmid[21], vio[3], vcoeff, 3);
    vmid[22] = vfmaq_laneq_f16(vmid[22], vio[4], vcoeff, 3);
    vmid[23] = vfmaq_laneq_f16(vmid[23], vio[5], vcoeff, 3);

    vio[0] = vld1q_f16(raw_output_block + 24 * wg_out_set_offset);
    vio[1] = vld1q_f16(raw_output_block + 25 * wg_out_set_offset);
    vio[2] = vld1q_f16(raw_output_block + 26 * wg_out_set_offset);
    vio[3] = vld1q_f16(raw_output_block + 27 * wg_out_set_offset);
    vio[4] = vld1q_f16(raw_output_block + 28 * wg_out_set_offset);
    vio[5] = vld1q_f16(raw_output_block + 29 * wg_out_set_offset);

    // r0 + r1 + r2 + r3 + r4
    vmid[0] = vaddq_f16(vmid[0], vio[0]);
    vmid[1] = vaddq_f16(vmid[1], vio[1]);
    vmid[2] = vaddq_f16(vmid[2], vio[2]);
    vmid[3] = vaddq_f16(vmid[3], vio[3]);
    vmid[4] = vaddq_f16(vmid[4], vio[4]);
    vmid[5] = vaddq_f16(vmid[5], vio[5]);

    // r1 - r2 + 2*r3 - 2*r4
    vmid[6]  = vfmsq_laneq_f16(vmid[6], vio[0], vcoeff, 1);
    vmid[7]  = vfmsq_laneq_f16(vmid[7], vio[1], vcoeff, 1);
    vmid[8]  = vfmsq_laneq_f16(vmid[8], vio[2], vcoeff, 1);
    vmid[9]  = vfmsq_laneq_f16(vmid[9], vio[3], vcoeff, 1);
    vmid[10] = vfmsq_laneq_f16(vmid[10], vio[4], vcoeff, 1);
    vmid[11] = vfmsq_laneq_f16(vmid[11], vio[5], vcoeff, 1);

    // r1 + r2 + 4*r3 + 4*r4
    vmid[12] = vfmaq_laneq_f16(vmid[12], vio[0], vcoeff, 2);
    vmid[13] = vfmaq_laneq_f16(vmid[13], vio[1], vcoeff, 2);
    vmid[14] = vfmaq_laneq_f16(vmid[14], vio[2], vcoeff, 2);
    vmid[15] = vfmaq_laneq_f16(vmid[15], vio[3], vcoeff, 2);
    vmid[16] = vfmaq_laneq_f16(vmid[16], vio[4], vcoeff, 2);
    vmid[17] = vfmaq_laneq_f16(vmid[17], vio[5], vcoeff, 2);

    // r1 - r2 + 8*r3 - 8*r4 + r5
    vmid[18] = vfmsq_laneq_f16(vmid[18], vio[0], vcoeff, 3);
    vmid[19] = vfmsq_laneq_f16(vmid[19], vio[1], vcoeff, 3);
    vmid[20] = vfmsq_laneq_f16(vmid[20], vio[2], vcoeff, 3);
    vmid[21] = vfmsq_laneq_f16(vmid[21], vio[3], vcoeff, 3);
    vmid[22] = vfmsq_laneq_f16(vmid[22], vio[4], vcoeff, 3);
    vmid[23] = vfmsq_laneq_f16(vmid[23], vio[5], vcoeff, 3);

    vio[0] = vaddq_f16(vmid[0], vmid[1]);
    vio[1] = vsubq_f16(vmid[1], vmid[2]);
    vio[2] = vaddq_f16(vmid[1], vmid[2]);
    vio[3] = vsubq_f16(vmid[1], vmid[2]);
    vio[4] = vaddq_f16(vmid[6], vmid[7]);
    vio[5] = vsubq_f16(vmid[7], vmid[8]);

    vio[0] = vaddq_f16(vio[0], vmid[2]);
    vio[1] = vfmaq_laneq_f16(vio[1], vmid[3], vcoeff, 1);
    vio[2] = vfmaq_laneq_f16(vio[2], vmid[3], vcoeff, 2);
    vio[3] = vfmaq_laneq_f16(vio[3], vmid[3], vcoeff, 3);
    vio[4] = vaddq_f16(vio[4], vmid[8]);
    vio[5] = vfmaq_laneq_f16(vio[5], vmid[9], vcoeff, 1);

    vio[0] = vaddq_f16(vio[0], vmid[3]);
    vio[1] = vfmsq_laneq_f16(vio[1], vmid[4], vcoeff, 1);
    vio[2] = vfmaq_laneq_f16(vio[2], vmid[4], vcoeff, 2);
    vio[3] = vfmsq_laneq_f16(vio[3], vmid[4], vcoeff, 3);
    vio[4] = vaddq_f16(vio[4], vmid[9]);
    vio[5] = vfmsq_laneq_f16(vio[5], vmid[10], vcoeff, 1);

    vio[0] = vaddq_f16(vio[0], vmid[4]);
    vio[3] = vaddq_f16(vio[3], vmid[5]);
    vio[4] = vaddq_f16(vio[4], vmid[10]);

    vio[0] = vaddq_f16(vio[0], vbias);
    vio[1] = vaddq_f16(vio[1], vbias);
    vio[2] = vaddq_f16(vio[2], vbias);
    vio[3] = vaddq_f16(vio[3], vbias);

    vio[4] = vaddq_f16(vio[4], vbias);
    vio[5] = vaddq_f16(vio[5], vbias);

    vmid[0] = vaddq_f16(vmid[7], vmid[8]);
    vmid[1] = vsubq_f16(vmid[7], vmid[8]);
    vmid[2] = vaddq_f16(vmid[12], vmid[13]);
    vmid[3] = vsubq_f16(vmid[13], vmid[14]);
    vmid[4] = vaddq_f16(vmid[13], vmid[14]);
    vmid[5] = vsubq_f16(vmid[13], vmid[14]);

    vmid[0] = vfmaq_laneq_f16(vmid[0], vmid[9], vcoeff, 2);
    vmid[1] = vfmaq_laneq_f16(vmid[1], vmid[9], vcoeff, 3);
    vmid[2] = vaddq_f16(vmid[2], vmid[14]);
    vmid[3] = vfmaq_laneq_f16(vmid[3], vmid[15], vcoeff, 1);
    vmid[4] = vfmaq_laneq_f16(vmid[4], vmid[15], vcoeff, 2);
    vmid[5] = vfmaq_laneq_f16(vmid[5], vmid[15], vcoeff, 3);

    vmid[0] = vfmaq_laneq_f16(vmid[0], vmid[10], vcoeff, 2);
    vmid[1] = vfmsq_laneq_f16(vmid[1], vmid[10], vcoeff, 3);
    vmid[2] = vaddq_f16(vmid[2], vmid[15]);
    vmid[3] = vfmsq_laneq_f16(vmid[3], vmid[16], vcoeff, 1);
    vmid[4] = vfmaq_laneq_f16(vmid[4], vmid[16], vcoeff, 2);
    vmid[5] = vfmsq_laneq_f16(vmid[5], vmid[16], vcoeff, 3);

    vmid[1] = vaddq_f16(vmid[1], vmid[11]);
    vmid[2] = vaddq_f16(vmid[2], vmid[16]);
    vmid[5] = vaddq_f16(vmid[5], vmid[17]);

    vmid[0] = vaddq_f16(vmid[0], vbias);
    vmid[1] = vaddq_f16(vmid[1], vbias);

    vmid[2] = vaddq_f16(vmid[2], vbias);
    vmid[3] = vaddq_f16(vmid[3], vbias);
    vmid[4] = vaddq_f16(vmid[4], vbias);
    vmid[5] = vaddq_f16(vmid[5], vbias);

    vmid[6] = vaddq_f16(vmid[18], vmid[19]);
    vmid[7] = vsubq_f16(vmid[19], vmid[20]);
    vmid[8] = vaddq_f16(vmid[19], vmid[20]);
    vmid[9] = vsubq_f16(vmid[19], vmid[20]);

    vmid[6] = vaddq_f16(vmid[6], vmid[20]);
    vmid[7] = vfmaq_laneq_f16(vmid[7], vmid[21], vcoeff, 1);
    vmid[8] = vfmaq_laneq_f16(vmid[8], vmid[21], vcoeff, 2);
    vmid[9] = vfmaq_laneq_f16(vmid[9], vmid[21], vcoeff, 3);

    vmid[6] = vaddq_f16(vmid[6], vmid[21]);
    vmid[7] = vfmsq_laneq_f16(vmid[7], vmid[22], vcoeff, 1);
    vmid[8] = vfmaq_laneq_f16(vmid[8], vmid[22], vcoeff, 2);
    vmid[9] = vfmsq_laneq_f16(vmid[9], vmid[22], vcoeff, 3);

    vmid[6] = vaddq_f16(vmid[6], vmid[22]);
    vmid[9] = vaddq_f16(vmid[9], vmid[23]);

    vmid[6] = vaddq_f16(vmid[6], vbias);
    vmid[7] = vaddq_f16(vmid[7], vbias);
    vmid[8] = vaddq_f16(vmid[8], vbias);
    vmid[9] = vaddq_f16(vmid[9], vbias);

    if (fuse_flag & conv_fuse_flag::SUM) {
        if (num_valid_oh > 0) {
            if (num_valid_ow > 0) vio[0] = vaddq_f16(vio[0], vld1q_f16(sum_block));
            if (num_valid_ow > 1) vio[1] = vaddq_f16(vio[1], vld1q_f16(sum_block + (1 * OCBLK())));
            if (num_valid_ow > 2) vio[2] = vaddq_f16(vio[2], vld1q_f16(sum_block + (2 * OCBLK())));
            if (num_valid_ow > 3) vio[3] = vaddq_f16(vio[3], vld1q_f16(sum_block + (3 * OCBLK())));
        }
        if (num_valid_oh > 1) {
            __fp16 *sum_ptr = sum_block + dst_w * OCBLK();
            if (num_valid_ow > 0) vio[4] = vaddq_f16(vio[4], vld1q_f16(sum_ptr));
            if (num_valid_ow > 1) vio[5] = vaddq_f16(vio[5], vld1q_f16(sum_ptr + (1 * OCBLK())));
            if (num_valid_ow > 2) vmid[0] = vaddq_f16(vmid[0], vld1q_f16(sum_ptr + (2 * OCBLK())));
            if (num_valid_ow > 3) vmid[1] = vaddq_f16(vmid[1], vld1q_f16(sum_ptr + (3 * OCBLK())));
        }
        if (num_valid_oh > 2) {
            __fp16 *sum_ptr = sum_block + dst_w * (2 * OCBLK());
            if (num_valid_ow > 0) vmid[2] = vaddq_f16(vmid[2], vld1q_f16(sum_ptr));
            if (num_valid_ow > 1) vmid[3] = vaddq_f16(vmid[3], vld1q_f16(sum_ptr + (1 * OCBLK())));
            if (num_valid_ow > 2) vmid[4] = vaddq_f16(vmid[4], vld1q_f16(sum_ptr + (2 * OCBLK())));
            if (num_valid_ow > 3) vmid[5] = vaddq_f16(vmid[5], vld1q_f16(sum_ptr + (3 * OCBLK())));
        }
        if (num_valid_oh > 3) {
            __fp16 *sum_ptr = sum_block + dst_w * (3 * OCBLK());
            if (num_valid_ow > 0) vmid[6] = vaddq_f16(vmid[6], vld1q_f16(sum_ptr));
            if (num_valid_ow > 1) vmid[7] = vaddq_f16(vmid[7], vld1q_f16(sum_ptr + (1 * OCBLK())));
            if (num_valid_ow > 2) vmid[8] = vaddq_f16(vmid[8], vld1q_f16(sum_ptr + (2 * OCBLK())));
            if (num_valid_ow > 3) vmid[9] = vaddq_f16(vmid[9], vld1q_f16(sum_ptr + (3 * OCBLK())));
        }
    }

    if (fuse_flag & conv_fuse_flag::RELU) {
        float16x8_t vzero = vdupq_n_f16(0.0f);
        vmid[9]           = vmaxq_f16(vmid[9], vzero);
        vmid[8]           = vmaxq_f16(vmid[8], vzero);
        vmid[7]           = vmaxq_f16(vmid[7], vzero);
        vmid[6]           = vmaxq_f16(vmid[6], vzero);
        vmid[5]           = vmaxq_f16(vmid[5], vzero);
        vmid[4]           = vmaxq_f16(vmid[4], vzero);
        vmid[3]           = vmaxq_f16(vmid[3], vzero);
        vmid[2]           = vmaxq_f16(vmid[2], vzero);
        vmid[1]           = vmaxq_f16(vmid[1], vzero);
        vmid[0]           = vmaxq_f16(vmid[0], vzero);
        vio[5]            = vmaxq_f16(vio[5], vzero);
        vio[4]            = vmaxq_f16(vio[4], vzero);
        vio[3]            = vmaxq_f16(vio[3], vzero);
        vio[2]            = vmaxq_f16(vio[2], vzero);
        vio[1]            = vmaxq_f16(vio[1], vzero);
        vio[0]            = vmaxq_f16(vio[0], vzero);
    }

    if (fuse_flag & conv_fuse_flag::RELU6) {
        float16x8_t vsix = vdupq_n_f16(6.0f);
        vmid[9]          = vminq_f16(vmid[9], vsix);
        vmid[8]          = vminq_f16(vmid[8], vsix);
        vmid[7]          = vminq_f16(vmid[7], vsix);
        vmid[6]          = vminq_f16(vmid[6], vsix);
        vmid[5]          = vminq_f16(vmid[5], vsix);
        vmid[4]          = vminq_f16(vmid[4], vsix);
        vmid[3]          = vminq_f16(vmid[3], vsix);
        vmid[2]          = vminq_f16(vmid[2], vsix);
        vmid[1]          = vminq_f16(vmid[1], vsix);
        vmid[0]          = vminq_f16(vmid[0], vsix);
        vio[5]           = vminq_f16(vio[5], vsix);
        vio[4]           = vminq_f16(vio[4], vsix);
        vio[3]           = vminq_f16(vio[3], vsix);
        vio[2]           = vminq_f16(vio[2], vsix);
        vio[1]           = vminq_f16(vio[1], vsix);
        vio[0]           = vminq_f16(vio[0], vsix);
    }

    switch (num_valid_oh) {
        case 4:
            switch (num_valid_ow) {
                case 4:
                    vst1q_f16(output_block + dst_w * (3 * OCBLK()) + (3 * OCBLK()), vmid[9]);
                    __attribute__((fallthrough));
                case 3:
                    vst1q_f16(output_block + dst_w * (3 * OCBLK()) + (2 * OCBLK()), vmid[8]);
                    __attribute__((fallthrough));
                case 2:
                    vst1q_f16(output_block + dst_w * (3 * OCBLK()) + (1 * OCBLK()), vmid[7]);
                    __attribute__((fallthrough));
                case 1:
                    vst1q_f16(output_block + dst_w * (3 * OCBLK()), vmid[6]);
                    __attribute__((fallthrough));
                default:;
            }
            __attribute__((fallthrough));
        case 3:
            switch (num_valid_ow) {
                case 4:
                    vst1q_f16(output_block + dst_w * (2 * OCBLK()) + (3 * OCBLK()), vmid[5]);
                    __attribute__((fallthrough));
                case 3:
                    vst1q_f16(output_block + dst_w * (2 * OCBLK()) + (2 * OCBLK()), vmid[4]);
                    __attribute__((fallthrough));
                case 2:
                    vst1q_f16(output_block + dst_w * (2 * OCBLK()) + (1 * OCBLK()), vmid[3]);
                    __attribute__((fallthrough));
                case 1:
                    vst1q_f16(output_block + dst_w * (2 * OCBLK()), vmid[2]);
                    __attribute__((fallthrough));
                default:;
            }
            __attribute__((fallthrough));
        case 2:
            switch (num_valid_ow) {
                case 4:
                    vst1q_f16(output_block + dst_w * (OCBLK()) + (3 * OCBLK()), vmid[1]);
                    __attribute__((fallthrough));
                case 3:
                    vst1q_f16(output_block + dst_w * (OCBLK()) + (2 * OCBLK()), vmid[0]);
                    __attribute__((fallthrough));
                case 2:
                    vst1q_f16(output_block + dst_w * (OCBLK()) + (1 * OCBLK()), vio[5]);
                    __attribute__((fallthrough));
                case 1:
                    vst1q_f16(output_block + dst_w * (OCBLK()), vio[4]);
                    __attribute__((fallthrough));
                default:;
            }
            __attribute__((fallthrough));
        case 1:
            switch (num_valid_ow) {
                case 4:
                    vst1q_f16(output_block + (3 * OCBLK()), vio[3]);
                    __attribute__((fallthrough));
                case 3:
                    vst1q_f16(output_block + (2 * OCBLK()), vio[2]);
                    __attribute__((fallthrough));
                case 2:
                    vst1q_f16(output_block + (1 * OCBLK()), vio[1]);
                    __attribute__((fallthrough));
                case 1:
                    vst1q_f16(output_block, vio[0]);
                    __attribute__((fallthrough));
                default:;
            }
            __attribute__((fallthrough));
        default:;
    }
}

uint64_t conv2d_wgb4f3_fp16_runtime_executor::cal_temp_buffer_size()
{
    const conv2d_param &cp                      = *conv_param_;
    const conv2d_wgb4f3_fp16_schedule_param &sp = sched_param_;
    size_t input_buffer_size                    = conv_winograd_b4f3_get_input_buffer_size_fp16(
        cp.channels, sp.tile_seg);
    size_t output_buffer_size = conv_winograd_b4f3_get_output_buffer_size_fp16(
        cp.num_output, sp.tile_seg);

    sched_param_.input_buffer_size  = input_buffer_size;
    sched_param_.output_buffer_size = output_buffer_size;

    size_t total_buffer_size = input_buffer_size + output_buffer_size + LLC_CACHELINE_SIZE();
    return total_buffer_size;
}

void conv2d_wgb4f3_fp16_runtime_executor::adjust_schedule_param()
{
    return;
}

ppl::common::RetCode conv2d_wgb4f3_fp16_runtime_executor::prepare()
{
    if (!conv_param_ || !src_shape_ || !dst_shape_) {
        return ppl::common::RC_INVALID_VALUE;
    }

    adjust_schedule_param();
    return ppl::common::RC_SUCCESS;
}

ppl::common::RetCode conv2d_wgb4f3_fp16_runtime_executor::execute()
{
    const conv2d_param &cp                      = *conv_param_;
    const conv2d_wgb4f3_fp16_schedule_param &sp = sched_param_;
    const __fp16 *input                         = (const __fp16 *)src_;
    const __fp16 *cvt_filter                    = (const __fp16 *)cvt_filter_;
    const __fp16 *bias                          = (const __fp16 *)cvt_bias_;
    __fp16 *output                              = (__fp16 *)dst_;
    __fp16 *sum                                 = (__fp16 *)sum_;
    __fp16 *tmp_buffer                          = (__fp16 *)temp_buffer_;
    const int64_t src_h                           = src_shape_->GetDim(2);
    const int64_t src_w                           = src_shape_->GetDim(3);
    const int64_t channels                          = src_shape_->GetDim(1);
    const int64_t num_output                         = cp.num_output;
    const int64_t dst_h                          = dst_shape_->GetDim(2);
    const int64_t dst_w                          = dst_shape_->GetDim(3);
    const int64_t pad_h                          = cp.pad_h;
    const int64_t pad_w                          = cp.pad_w;
    const int64_t group                     = cp.group;
    const int64_t ics                           = sp.ic_seg;
    const int64_t ocs                           = sp.oc_seg;
    const int64_t tile_l2_size                  = sp.tile_seg;
    const int64_t num_batch                     = src_shape_->GetDim(0);
    const size_t input_prep_buffer_size         = sp.input_buffer_size;

    PRAGMA_OMP_PARALLEL()
    {
        const int64_t ic_packed = CEIL8(channels);
        const int64_t oc_packed = CEIL8(num_output);

        const int64_t ic_group    = channels / group;
        const int64_t oc_group    = num_output / group;
        const int64_t ic_g_packed = CEIL8(ic_group);
        const int64_t oc_g_packed = CEIL8(oc_group);

        const int64_t k_in_channel_section  = CEIL8(std::min(ics, ic_group));
        const int64_t k_out_channel_section = CEIL8(std::min(ocs, oc_group));

        const int64_t k_tile_l2 = tile_l2_size;

        const int64_t k_in_wg_set_offset  = k_in_channel_section * k_tile_l2;
        const int64_t k_out_wg_set_offset = oc_g_packed * k_tile_l2;

        /* Inner parallel mode */
        __fp16 *pre_proc_buffer  = tmp_buffer;
        __fp16 *post_proc_buffer = pre_proc_buffer + input_prep_buffer_size / sizeof(__fp16);

        const float16x8_t vzeros = vdupq_n_f16(0.0f);

        const int64_t num_h_blocks  = DIV_CEIL(dst_h, WGB4F3_OBLK());
        const int64_t num_w_blocks  = DIV_CEIL(dst_w, WGB4F3_OBLK());
        const int64_t num_hw_blocks = num_h_blocks * num_w_blocks;
        const int64_t num_tiles     = num_batch * num_hw_blocks;

        const int64_t hw_in               = src_h * src_w;
        const int64_t hw_out              = dst_h * dst_w;
        const int64_t input_b_stride      = ic_packed * hw_in;
        const int64_t output_b_stride     = oc_packed * hw_out;
        const int64_t input_g_stride      = ic_group * hw_in;
        const int64_t output_g_stride     = oc_group * hw_out;
        const int64_t filter_wgset_stride = oc_g_packed * ic_g_packed;
        const int64_t filter_g_stride     = WGB4F3_NSET() * filter_wgset_stride;

        bool ih_valid[WGB4F3_IBLK()];
        bool iw_valid[WGB4F3_IBLK()];

        for (int64_t g = 0; g < group; g++) {
            const __fp16 *input_g_base  = input + g * input_g_stride;
            const __fp16 *filter_g_base = cvt_filter + g * filter_g_stride;
            const __fp16 *bias_g_base   = bias + g * oc_group;
            __fp16 *output_g_base       = output + g * output_g_stride;
            __fp16 *sum_g_base          = sum + g * output_g_stride;

            for (int64_t tile_l2 = 0; tile_l2 < num_tiles; tile_l2 += k_tile_l2) {
                const int64_t wg_blocks = std::min(k_tile_l2, num_tiles - tile_l2);

                // Note: using `ic_group` in the loop is the same with using `ic_g_packed`.
                for (int64_t ic_l2 = 0; ic_l2 < ic_g_packed; ic_l2 += k_in_channel_section) {
                    const bool is_first_ic           = (ic_l2 == 0);
                    const bool is_last_ic            = (ic_l2 + k_in_channel_section >= ic_g_packed);
                    const int64_t in_channel_section = std::min(ic_g_packed - ic_l2, k_in_channel_section);

                    const float16x8_t vcoeff_prep = {1.0f, 2.0f, 4.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f};

                    PRAGMA_OMP_FOR_COLLAPSE(2)
                    for (int64_t ic = 0; ic < in_channel_section; ic += ICBLK()) {
                        for (int64_t tile_l0 = 0; tile_l0 < wg_blocks; tile_l0++) {
                            int64_t tile_id        = tile_l0 + tile_l2;
                            const int64_t batch_id = tile_id / num_hw_blocks;

                            const int64_t tile_hw_id = tile_id % num_hw_blocks;
                            const int64_t tile_h_id  = tile_hw_id / num_w_blocks;
                            const int64_t tile_w_id  = tile_hw_id % num_w_blocks;

                            const int64_t oh = tile_h_id * WGB4F3_OBLK();
                            const int64_t ow = tile_w_id * WGB4F3_OBLK();

                            const __fp16 *input_c_base = input_g_base + batch_id * input_b_stride + (ic_l2 + ic) * hw_in;
                            __fp16 *prep_in_c_base     = pre_proc_buffer + ic * wg_blocks;

                            const int64_t ih0 = -pad_h + oh;
                            const int64_t ih1 = ih0 + 1;
                            const int64_t ih2 = ih0 + 2;
                            const int64_t ih3 = ih0 + 3;
                            const int64_t ih4 = ih0 + 4;
                            const int64_t ih5 = ih0 + 5;

                            ih_valid[0] = (ih0 >= 0 && ih0 < src_h);
                            ih_valid[1] = (ih1 >= 0 && ih1 < src_h);
                            ih_valid[2] = (ih2 >= 0 && ih2 < src_h);
                            ih_valid[3] = (ih3 >= 0 && ih3 < src_h);
                            ih_valid[4] = (ih4 >= 0 && ih4 < src_h);
                            ih_valid[5] = (ih5 >= 0 && ih5 < src_h);

                            int64_t wg_block_idx  = tile_l0;
                            __fp16 *prep_in_block = prep_in_c_base + wg_block_idx * ICBLK();

                            const int64_t iw0 = -pad_w + ow;
                            const int64_t iw1 = iw0 + 1;
                            const int64_t iw2 = iw0 + 2;
                            const int64_t iw3 = iw0 + 3;
                            const int64_t iw4 = iw0 + 4;
                            const int64_t iw5 = iw0 + 5;

                            iw_valid[0] = (iw0 >= 0 && iw0 < src_w);
                            iw_valid[1] = (iw1 >= 0 && iw1 < src_w);
                            iw_valid[2] = (iw2 >= 0 && iw2 < src_w);
                            iw_valid[3] = (iw3 >= 0 && iw3 < src_w);
                            iw_valid[4] = (iw4 >= 0 && iw4 < src_w);
                            iw_valid[5] = (iw5 >= 0 && iw5 < src_w);

                            conv2d_n8cx_wgb4f3_prep_input_block_fp16(
                                input_c_base + ih0 * src_w * ICBLK() + iw0 * ICBLK(),
                                vzeros,
                                vcoeff_prep,
                                prep_in_block,
                                src_w,
                                k_in_wg_set_offset,
                                ih_valid,
                                iw_valid);

                        } // close loop over tile
                    } // close loop over ic(register)

                    const int32_t init_id = (is_first_ic) ? 0 : 2;
                    const int64_t fini_id = 0;
                    // Note: using `oc_group` in the loop is the same with using `oc_g_packed`.
                    for (int64_t oc_l2 = 0; oc_l2 < oc_g_packed; oc_l2 += k_out_channel_section) {
                        const int64_t out_channel_section = std::min(oc_g_packed - oc_l2, k_out_channel_section);
                        const __fp16 *cvt_filter_cc_base  = filter_g_base + oc_l2 * ic_g_packed + ic_l2 * CEIL8(out_channel_section); // pack to 4:OCBLK()
                        __fp16 *raw_out_cl2_base          = post_proc_buffer + oc_l2 * wg_blocks;

                        PRAGMA_OMP_FOR_COLLAPSE(2)
                        for (int64_t set_id = 0; set_id < WGB4F3_NSET(); set_id++) {
                            for (int64_t oc = 0; oc < out_channel_section; oc += 2 * OCBLK()) {
                                const int64_t m_l0 = std::min((int64_t)2 * OCBLK(), out_channel_section - oc);
                                if (m_l0 > OCBLK()) {
                                    for (int64_t block = 0; block < wg_blocks; block += N8CX_HGEMM_N10_BLOCK0()) {
                                        const int64_t n_l0 = std::min((int64_t)N8CX_HGEMM_N10_BLOCK0(), wg_blocks - block);
                                        hgemm_n8cx_kernel_m16nx_fp16_func_table[n_l0 - 1][init_id][fini_id](
                                            cvt_filter_cc_base + set_id * filter_wgset_stride + oc * CEIL8(in_channel_section),
                                            pre_proc_buffer + set_id * k_in_wg_set_offset + block * ICBLK(),
                                            nullptr, /* constant:bias */
                                            nullptr, /* fusedata:sum */
                                            raw_out_cl2_base + set_id * k_out_wg_set_offset + oc * wg_blocks + block * OCBLK(),
                                            m_l0,
                                            n_l0,
                                            in_channel_section,
                                            oc_g_packed,
                                            wg_blocks,
                                            0,
                                            wg_blocks);
                                    } // close loop over wg-block(register)
                                }
                                else {
                                    for (int64_t block = 0; block < wg_blocks; block += N8CX_HGEMM_N12_BLOCK0()) {
                                        const int64_t m_l0 = std::min((int64_t)OCBLK(), out_channel_section - oc);
                                        const int64_t n_l0 = std::min((int64_t)N8CX_HGEMM_N12_BLOCK0(), wg_blocks - block);
                                        hgemm_n8cx_kernel_m8nx_fp16_func_table[n_l0 - 1][init_id][fini_id](
                                            cvt_filter_cc_base + set_id * filter_wgset_stride + oc * CEIL8(in_channel_section),
                                            pre_proc_buffer + set_id * k_in_wg_set_offset + block * ICBLK(),
                                            nullptr, /* constant:bias */
                                            nullptr, /* fusedata:sum */
                                            raw_out_cl2_base + set_id * k_out_wg_set_offset + oc * wg_blocks + block * OCBLK(),
                                            m_l0,
                                            n_l0,
                                            in_channel_section,
                                            oc_g_packed,
                                            wg_blocks,
                                            0,
                                            wg_blocks);
                                    }
                                }
                            } // close loop over oc(register)
                        } // close loop over wg-set
                        // NOTE: implicit omp barrier

                        if (is_last_ic) {
                            __fp16 *output_oc_l2_base = output_g_base + oc_l2 * hw_out;
                            __fp16 *sum_oc_l2_base    = sum_g_base + oc_l2 * hw_out;

                            const float16x8_t vcoeff_postp = {1.0f, 2.0f, 4.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                            PRAGMA_OMP_FOR_COLLAPSE(2)
                            for (int64_t oc = 0; oc < out_channel_section; oc += OCBLK()) {
                                for (int64_t tile_l0 = 0; tile_l0 < wg_blocks; tile_l0++) {
                                    const __fp16 *raw_output_c_base = raw_out_cl2_base + oc * wg_blocks;
                                    const float16x8_t vbias         = vld1q_f16(bias_g_base + oc_l2 + oc);
                                    __fp16 *output_oc_base          = output_oc_l2_base + oc * hw_out;
                                    __fp16 *sum_oc_base             = sum_oc_l2_base + oc * hw_out;

                                    int64_t tile_id        = tile_l0 + tile_l2;
                                    const int64_t batch_id = tile_id / num_hw_blocks;

                                    const int64_t tile_hw_id = tile_id % num_hw_blocks;
                                    const int64_t tile_h_id  = tile_hw_id / num_w_blocks;
                                    const int64_t tile_w_id  = tile_hw_id % num_w_blocks;

                                    const int64_t oh = tile_h_id * WGB4F3_OBLK();
                                    const int64_t ow = tile_w_id * WGB4F3_OBLK();

                                    conv2d_n8cx_wgb4f3_postp_output_block_fp16(
                                        raw_output_c_base + tile_l0 * OCBLK(),
                                        vbias,
                                        vcoeff_postp,
                                        output_oc_base + batch_id * output_b_stride + (oh * dst_w + ow) * OCBLK(),
                                        sum_oc_base + batch_id * output_b_stride + (oh * dst_w + ow) * OCBLK(),
                                        k_out_wg_set_offset,
                                        dst_w,
                                        std::min((int64_t)WGB4F3_OBLK(), dst_h - oh),
                                        std::min((int64_t)WGB4F3_OBLK(), dst_w - ow),
                                        cp.fuse_flag);
                                } // close loop over tile
                            } // close loop over oc(register)
                        }
                    } // close loop over oc(l2)
                } // close loop over ic(l2)
            } // close loop over batch-dst_h-dst_w
        } // close loop over group
    }
    return ppl::common::RC_SUCCESS;
}

static size_t conv_winograd_b4f3_get_converted_filter_size_fp16(
    const int64_t channels,
    const int64_t num_output,
    const int64_t group)
{
    const int64_t ic_group             = channels / group;
    const int64_t oc_group             = num_output / group;
    const size_t converted_filter_size = group * WGB4F3_NSET() * CEIL8(oc_group) * CEIL8(ic_group) * sizeof(__fp16) + LLC_CACHELINE_SIZE();
    return converted_filter_size;
}

static void conv_winograd_b4f3_convert_filter_fp16(
    const __fp16 *filter,
    __fp16 *converted_filter,
    __fp16 *aux_filter_buffer,
    const int64_t channels,
    const int64_t num_output,
    const int64_t group,
    const int64_t k_ic_sec,
    const int64_t k_oc_sec)
{
    const float32x4_t vcoeff = {(1.0f / 4.0f), (-1.0f / 6.0f), (1.0f / 6.0f), (1.0f / 2.0f)};

    const int64_t ic_group        = channels / group;
    const int64_t oc_group        = num_output / group;
    const int64_t ic_group_packed = CEIL8(ic_group);
    const int64_t oc_group_packed = CEIL8(oc_group);

    const int64_t in_ch_section  = CEIL8(std::min(k_ic_sec, ic_group));
    const int64_t out_ch_section = CEIL8(std::min(k_oc_sec, oc_group));

    size_t filter_wg_set_offset = oc_group_packed * ic_group_packed;

    for (int64_t g = 0; g < group; g++) {
        const __fp16 *filter_g_base     = filter + g * oc_group * ic_group * 9;
        __fp16 *converted_filter_g_base = converted_filter + g * WGB4F3_NSET() * filter_wg_set_offset;

        // first pass
        // note: pack channels to 8channels
        __fp16 *aux_filter       = aux_filter_buffer;
        const int64_t half_c_blk = ICBLK() / 2;
        float g_ic_pck[9 * half_c_blk];
        for (int64_t oc = 0; oc < oc_group; oc++) {
            for (int64_t ic = 0; ic < ic_group; ic += half_c_blk) {
                const __fp16 *filter_base = filter_g_base + oc * ic_group * 9 + ic * 9;
                const int64_t icV         = std::min((int64_t)half_c_blk, ic_group - ic);

                for (int64_t lane_id = 0; lane_id < icV; lane_id++) {
                    for (int64_t kidx = 0; kidx < 9; kidx++) {
                        g_ic_pck[kidx * half_c_blk + lane_id] = filter_base[lane_id * 9 + kidx];
                    }
                }
                for (int64_t kidx = 0; kidx < 9; kidx++) {
                    for (int64_t lane_id = icV; lane_id < half_c_blk; lane_id++) {
                        g_ic_pck[kidx * half_c_blk + lane_id] = 0.0f;
                    }
                }

                float32x4_t g[9];
                float32x4_t G[9];
                float32x4_t Gt[9];

                // UPLEFT
                // g[0][:]
                g[0] = vld1q_f32(&g_ic_pck[0 * half_c_blk]);
                g[1] = vld1q_f32(&g_ic_pck[1 * half_c_blk]);
                g[2] = vld1q_f32(&g_ic_pck[2 * half_c_blk]);

                G[0] = vmulq_laneq_f32(g[0], vcoeff, 0);
                G[1] = vmulq_laneq_f32(g[1], vcoeff, 0);
                G[2] = vmulq_laneq_f32(g[2], vcoeff, 0);

                G[3] = vmulq_laneq_f32(g[0], vcoeff, 1);
                G[4] = vmulq_laneq_f32(g[1], vcoeff, 1);
                G[5] = vmulq_laneq_f32(g[2], vcoeff, 1);

                G[6] = vmulq_laneq_f32(g[0], vcoeff, 1);
                G[7] = vmulq_laneq_f32(g[1], vcoeff, 1);
                G[8] = vmulq_laneq_f32(g[2], vcoeff, 1);

                // g[1][:]
                g[3] = vld1q_f32(&g_ic_pck[3 * half_c_blk]);
                g[4] = vld1q_f32(&g_ic_pck[4 * half_c_blk]);
                g[5] = vld1q_f32(&g_ic_pck[5 * half_c_blk]);

                G[3] = vfmaq_laneq_f32(G[3], g[3], vcoeff, 1);
                G[4] = vfmaq_laneq_f32(G[4], g[4], vcoeff, 1);
                G[5] = vfmaq_laneq_f32(G[5], g[5], vcoeff, 1);

                G[6] = vfmsq_laneq_f32(G[6], g[3], vcoeff, 1);
                G[7] = vfmsq_laneq_f32(G[7], g[4], vcoeff, 1);
                G[8] = vfmsq_laneq_f32(G[8], g[5], vcoeff, 1);

                // g[2][:]
                g[6] = vld1q_f32(&g_ic_pck[6 * half_c_blk]);
                g[7] = vld1q_f32(&g_ic_pck[7 * half_c_blk]);
                g[8] = vld1q_f32(&g_ic_pck[8 * half_c_blk]);

                G[3] = vfmaq_laneq_f32(G[3], g[6], vcoeff, 1);
                G[4] = vfmaq_laneq_f32(G[4], g[7], vcoeff, 1);
                G[5] = vfmaq_laneq_f32(G[5], g[8], vcoeff, 1);

                G[6] = vfmaq_laneq_f32(G[6], g[6], vcoeff, 1);
                G[7] = vfmaq_laneq_f32(G[7], g[7], vcoeff, 1);
                G[8] = vfmaq_laneq_f32(G[8], g[8], vcoeff, 1);

                // Gt
                Gt[0] = vmulq_laneq_f32(G[0], vcoeff, 0);
                Gt[3] = vmulq_laneq_f32(G[3], vcoeff, 0);
                Gt[6] = vmulq_laneq_f32(G[6], vcoeff, 0);

                Gt[1] = vmulq_laneq_f32(G[0], vcoeff, 1);
                Gt[4] = vmulq_laneq_f32(G[3], vcoeff, 1);
                Gt[7] = vmulq_laneq_f32(G[6], vcoeff, 1);

                Gt[2] = vmulq_laneq_f32(G[0], vcoeff, 1);
                Gt[5] = vmulq_laneq_f32(G[3], vcoeff, 1);
                Gt[8] = vmulq_laneq_f32(G[6], vcoeff, 1);

                Gt[1] = vfmaq_laneq_f32(Gt[1], G[1], vcoeff, 1);
                Gt[4] = vfmaq_laneq_f32(Gt[4], G[4], vcoeff, 1);
                Gt[7] = vfmaq_laneq_f32(Gt[7], G[7], vcoeff, 1);

                Gt[2] = vfmsq_laneq_f32(Gt[2], G[1], vcoeff, 1);
                Gt[5] = vfmsq_laneq_f32(Gt[5], G[4], vcoeff, 1);
                Gt[8] = vfmsq_laneq_f32(Gt[8], G[7], vcoeff, 1);

                Gt[1] = vfmaq_laneq_f32(Gt[1], G[2], vcoeff, 1);
                Gt[4] = vfmaq_laneq_f32(Gt[4], G[5], vcoeff, 1);
                Gt[7] = vfmaq_laneq_f32(Gt[7], G[8], vcoeff, 1);

                Gt[2] = vfmaq_laneq_f32(Gt[2], G[2], vcoeff, 1);
                Gt[5] = vfmaq_laneq_f32(Gt[5], G[5], vcoeff, 1);
                Gt[8] = vfmaq_laneq_f32(Gt[8], G[8], vcoeff, 1);

                vst1_f16(aux_filter, vcvt_f16_f32(Gt[0]));
                vst1_f16(aux_filter + 1 * filter_wg_set_offset, vcvt_f16_f32(Gt[1]));
                vst1_f16(aux_filter + 2 * filter_wg_set_offset, vcvt_f16_f32(Gt[2]));

                vst1_f16(aux_filter + 6 * filter_wg_set_offset, vcvt_f16_f32(Gt[3]));
                vst1_f16(aux_filter + 7 * filter_wg_set_offset, vcvt_f16_f32(Gt[4]));
                vst1_f16(aux_filter + 8 * filter_wg_set_offset, vcvt_f16_f32(Gt[5]));

                vst1_f16(aux_filter + 12 * filter_wg_set_offset, vcvt_f16_f32(Gt[6]));
                vst1_f16(aux_filter + 13 * filter_wg_set_offset, vcvt_f16_f32(Gt[7]));
                vst1_f16(aux_filter + 14 * filter_wg_set_offset, vcvt_f16_f32(Gt[8]));

                // UPRIGHT
                Gt[0] = vmulq_laneq_f32(G[0], vcoeff, 0);
                Gt[3] = vmulq_laneq_f32(G[3], vcoeff, 0);
                Gt[6] = vmulq_laneq_f32(G[6], vcoeff, 0);

                Gt[1] = vmulq_laneq_f32(G[0], vcoeff, 0);
                Gt[4] = vmulq_laneq_f32(G[3], vcoeff, 0);
                Gt[7] = vmulq_laneq_f32(G[6], vcoeff, 0);

                Gt[0] = vfmaq_laneq_f32(Gt[0], G[1], vcoeff, 3);
                Gt[3] = vfmaq_laneq_f32(Gt[3], G[4], vcoeff, 3);
                Gt[6] = vfmaq_laneq_f32(Gt[6], G[7], vcoeff, 3);

                Gt[1] = vfmsq_laneq_f32(Gt[1], G[1], vcoeff, 3);
                Gt[4] = vfmsq_laneq_f32(Gt[4], G[4], vcoeff, 3);
                Gt[7] = vfmsq_laneq_f32(Gt[7], G[7], vcoeff, 3);

                Gt[0] = vaddq_f32(Gt[0], G[2]);
                Gt[3] = vaddq_f32(Gt[3], G[5]);
                Gt[6] = vaddq_f32(Gt[6], G[8]);

                Gt[1] = vaddq_f32(Gt[1], G[2]);
                Gt[4] = vaddq_f32(Gt[4], G[5]);
                Gt[7] = vaddq_f32(Gt[7], G[8]);

                Gt[2] = G[2];
                Gt[5] = G[5];
                Gt[8] = G[8];

                Gt[0] = vmulq_laneq_f32(Gt[0], vcoeff, 2);
                Gt[3] = vmulq_laneq_f32(Gt[3], vcoeff, 2);
                Gt[6] = vmulq_laneq_f32(Gt[6], vcoeff, 2);

                Gt[1] = vmulq_laneq_f32(Gt[1], vcoeff, 2);
                Gt[4] = vmulq_laneq_f32(Gt[4], vcoeff, 2);
                Gt[7] = vmulq_laneq_f32(Gt[7], vcoeff, 2);

                vst1_f16(aux_filter + 3 * filter_wg_set_offset, vcvt_f16_f32(Gt[0]));
                vst1_f16(aux_filter + 4 * filter_wg_set_offset, vcvt_f16_f32(Gt[1]));
                vst1_f16(aux_filter + 5 * filter_wg_set_offset, vcvt_f16_f32(Gt[2]));

                vst1_f16(aux_filter + 9 * filter_wg_set_offset, vcvt_f16_f32(Gt[3]));
                vst1_f16(aux_filter + 10 * filter_wg_set_offset, vcvt_f16_f32(Gt[4]));
                vst1_f16(aux_filter + 11 * filter_wg_set_offset, vcvt_f16_f32(Gt[5]));

                vst1_f16(aux_filter + 15 * filter_wg_set_offset, vcvt_f16_f32(Gt[6]));
                vst1_f16(aux_filter + 16 * filter_wg_set_offset, vcvt_f16_f32(Gt[7]));
                vst1_f16(aux_filter + 17 * filter_wg_set_offset, vcvt_f16_f32(Gt[8]));

                // DOWNLEFT
                G[0] = vmulq_laneq_f32(g[0], vcoeff, 0);
                G[1] = vmulq_laneq_f32(g[1], vcoeff, 0);
                G[2] = vmulq_laneq_f32(g[2], vcoeff, 0);

                G[3] = vmulq_laneq_f32(g[0], vcoeff, 0);
                G[4] = vmulq_laneq_f32(g[1], vcoeff, 0);
                G[5] = vmulq_laneq_f32(g[2], vcoeff, 0);

                G[0] = vfmaq_laneq_f32(G[0], g[3], vcoeff, 3);
                G[1] = vfmaq_laneq_f32(G[1], g[4], vcoeff, 3);
                G[2] = vfmaq_laneq_f32(G[2], g[5], vcoeff, 3);

                G[3] = vfmsq_laneq_f32(G[3], g[3], vcoeff, 3);
                G[4] = vfmsq_laneq_f32(G[4], g[4], vcoeff, 3);
                G[5] = vfmsq_laneq_f32(G[5], g[5], vcoeff, 3);

                G[0] = vaddq_f32(G[0], g[6]);
                G[1] = vaddq_f32(G[1], g[7]);
                G[2] = vaddq_f32(G[2], g[8]);

                G[3] = vaddq_f32(G[3], g[6]);
                G[4] = vaddq_f32(G[4], g[7]);
                G[5] = vaddq_f32(G[5], g[8]);

                G[6] = g[6];
                G[7] = g[7];
                G[8] = g[8];

                G[0] = vmulq_laneq_f32(G[0], vcoeff, 2);
                G[1] = vmulq_laneq_f32(G[1], vcoeff, 2);
                G[2] = vmulq_laneq_f32(G[2], vcoeff, 2);

                G[3] = vmulq_laneq_f32(G[3], vcoeff, 2);
                G[4] = vmulq_laneq_f32(G[4], vcoeff, 2);
                G[5] = vmulq_laneq_f32(G[5], vcoeff, 2);

                Gt[0] = vmulq_laneq_f32(G[0], vcoeff, 0);
                Gt[3] = vmulq_laneq_f32(G[3], vcoeff, 0);
                Gt[6] = vmulq_laneq_f32(G[6], vcoeff, 0);

                Gt[1] = vmulq_laneq_f32(G[0], vcoeff, 1);
                Gt[4] = vmulq_laneq_f32(G[3], vcoeff, 1);
                Gt[7] = vmulq_laneq_f32(G[6], vcoeff, 1);

                Gt[2] = vmulq_laneq_f32(G[0], vcoeff, 1);
                Gt[5] = vmulq_laneq_f32(G[3], vcoeff, 1);
                Gt[8] = vmulq_laneq_f32(G[6], vcoeff, 1);

                Gt[1] = vfmaq_laneq_f32(Gt[1], G[1], vcoeff, 1);
                Gt[4] = vfmaq_laneq_f32(Gt[4], G[4], vcoeff, 1);
                Gt[7] = vfmaq_laneq_f32(Gt[7], G[7], vcoeff, 1);

                Gt[2] = vfmsq_laneq_f32(Gt[2], G[1], vcoeff, 1);
                Gt[5] = vfmsq_laneq_f32(Gt[5], G[4], vcoeff, 1);
                Gt[8] = vfmsq_laneq_f32(Gt[8], G[7], vcoeff, 1);

                Gt[1] = vfmaq_laneq_f32(Gt[1], G[2], vcoeff, 1);
                Gt[4] = vfmaq_laneq_f32(Gt[4], G[5], vcoeff, 1);
                Gt[7] = vfmaq_laneq_f32(Gt[7], G[8], vcoeff, 1);

                Gt[2] = vfmaq_laneq_f32(Gt[2], G[2], vcoeff, 1);
                Gt[5] = vfmaq_laneq_f32(Gt[5], G[5], vcoeff, 1);
                Gt[8] = vfmaq_laneq_f32(Gt[8], G[8], vcoeff, 1);

                vst1_f16(aux_filter + 18 * filter_wg_set_offset, vcvt_f16_f32(Gt[0]));
                vst1_f16(aux_filter + 19 * filter_wg_set_offset, vcvt_f16_f32(Gt[1]));
                vst1_f16(aux_filter + 20 * filter_wg_set_offset, vcvt_f16_f32(Gt[2]));

                vst1_f16(aux_filter + 24 * filter_wg_set_offset, vcvt_f16_f32(Gt[3]));
                vst1_f16(aux_filter + 25 * filter_wg_set_offset, vcvt_f16_f32(Gt[4]));
                vst1_f16(aux_filter + 26 * filter_wg_set_offset, vcvt_f16_f32(Gt[5]));

                vst1_f16(aux_filter + 30 * filter_wg_set_offset, vcvt_f16_f32(Gt[6]));
                vst1_f16(aux_filter + 31 * filter_wg_set_offset, vcvt_f16_f32(Gt[7]));
                vst1_f16(aux_filter + 32 * filter_wg_set_offset, vcvt_f16_f32(Gt[8]));

                // DOWNRIGHT
                Gt[0] = vmulq_laneq_f32(G[0], vcoeff, 0);
                Gt[3] = vmulq_laneq_f32(G[3], vcoeff, 0);
                Gt[6] = vmulq_laneq_f32(G[6], vcoeff, 0);

                Gt[1] = vmulq_laneq_f32(G[0], vcoeff, 0);
                Gt[4] = vmulq_laneq_f32(G[3], vcoeff, 0);
                Gt[7] = vmulq_laneq_f32(G[6], vcoeff, 0);

                Gt[0] = vfmaq_laneq_f32(Gt[0], G[1], vcoeff, 3);
                Gt[3] = vfmaq_laneq_f32(Gt[3], G[4], vcoeff, 3);
                Gt[6] = vfmaq_laneq_f32(Gt[6], G[7], vcoeff, 3);

                Gt[1] = vfmsq_laneq_f32(Gt[1], G[1], vcoeff, 3);
                Gt[4] = vfmsq_laneq_f32(Gt[4], G[4], vcoeff, 3);
                Gt[7] = vfmsq_laneq_f32(Gt[7], G[7], vcoeff, 3);

                Gt[0] = vaddq_f32(Gt[0], G[2]);
                Gt[3] = vaddq_f32(Gt[3], G[5]);
                Gt[6] = vaddq_f32(Gt[6], G[8]);

                Gt[1] = vaddq_f32(Gt[1], G[2]);
                Gt[4] = vaddq_f32(Gt[4], G[5]);
                Gt[7] = vaddq_f32(Gt[7], G[8]);

                Gt[2] = G[2];
                Gt[5] = G[5];
                Gt[8] = G[8];

                Gt[0] = vmulq_laneq_f32(Gt[0], vcoeff, 2);
                Gt[3] = vmulq_laneq_f32(Gt[3], vcoeff, 2);
                Gt[6] = vmulq_laneq_f32(Gt[6], vcoeff, 2);

                Gt[1] = vmulq_laneq_f32(Gt[1], vcoeff, 2);
                Gt[4] = vmulq_laneq_f32(Gt[4], vcoeff, 2);
                Gt[7] = vmulq_laneq_f32(Gt[7], vcoeff, 2);

                vst1_f16(aux_filter + 21 * filter_wg_set_offset, vcvt_f16_f32(Gt[0]));
                vst1_f16(aux_filter + 22 * filter_wg_set_offset, vcvt_f16_f32(Gt[1]));
                vst1_f16(aux_filter + 23 * filter_wg_set_offset, vcvt_f16_f32(Gt[2]));

                vst1_f16(aux_filter + 27 * filter_wg_set_offset, vcvt_f16_f32(Gt[3]));
                vst1_f16(aux_filter + 28 * filter_wg_set_offset, vcvt_f16_f32(Gt[4]));
                vst1_f16(aux_filter + 29 * filter_wg_set_offset, vcvt_f16_f32(Gt[5]));

                vst1_f16(aux_filter + 33 * filter_wg_set_offset, vcvt_f16_f32(Gt[6]));
                vst1_f16(aux_filter + 34 * filter_wg_set_offset, vcvt_f16_f32(Gt[7]));
                vst1_f16(aux_filter + 35 * filter_wg_set_offset, vcvt_f16_f32(Gt[8]));

                aux_filter += half_c_blk;
            }
        }

        // second pass
        // note: pad num_output to 8c
        for (int64_t set_id = 0; set_id < WGB4F3_NSET(); set_id++) {
            const __fp16 *aux_filter_base = aux_filter_buffer + set_id * filter_wg_set_offset;
            __fp16 *converted_filter_base = converted_filter_g_base + set_id * filter_wg_set_offset;

            for (int64_t i = 0; i < oc_group; i += out_ch_section) {
                for (int64_t p = 0; p < ic_group_packed; p += in_ch_section) {
                    int64_t m_l1 = std::min(oc_group - i, out_ch_section);
                    int64_t k_l1 = std::min(ic_group_packed - p, in_ch_section);
                    hgemm_n8cx_inner_blocking_16x8_fp16(
                        aux_filter_base + i * ic_group_packed + p,
                        converted_filter_base + i * CEIL8(ic_group_packed) + p * CEIL8(m_l1),
                        ic_group_packed,
                        m_l1,
                        k_l1);
                } // close loop over outer K blocks
            } // close loop over outer M blocks
        } // close loop over wg-sets
    }
}

bool conv2d_wgb4f3_fp16_offline_manager::is_supported()
{
    const conv2d_param &cp = param_;
    if (cp.group > 1) {
        const int64_t ic_group = cp.channels / cp.group;
        const int64_t oc_group = cp.num_output / cp.group;
        if (ic_group % ICBLK() != 0 || oc_group % OCBLK()) {
            return false;
        }
    }
    if (cp.kernel_h != 3 || cp.kernel_w != 3 ||
        cp.stride_h != 1 || cp.stride_w != 1 ||
        cp.dilation_h != 1 || cp.dilation_w != 1) {
        return false;
    }
    return true;
}

ppl::common::RetCode conv2d_wgb4f3_fp16_offline_manager::fast_init_schedule_param()
{
    sched_param_.oc_seg   = 1024;
    sched_param_.ic_seg   = 192;
    sched_param_.tile_seg = 140;

    if (sched_param_.oc_seg != 1024) {
        return ppl::common::RC_INVALID_VALUE;
    }
    if (sched_param_.ic_seg != 192) {
        return ppl::common::RC_INVALID_VALUE;
    }
    if (sched_param_.tile_seg != 140) {
        return ppl::common::RC_INVALID_VALUE;
    }
    return ppl::common::RC_SUCCESS;
}

ppl::common::RetCode conv2d_wgb4f3_fp16_offline_manager::pick_best_schedule_param(const ppl::nn::TensorShape &src_shape, double &run_time, bool tune_blocksize)
{
    const int64_t num_output = param_.num_output;
    const int64_t channels   = param_.channels;

    const int64_t ic_g_pck = CEIL8(channels / param_.group);

    if (src_shape.GetDimCount() < 4) {
        return ppl::common::RC_INVALID_VALUE;
    }
    const int64_t num_batch = src_shape.GetDim(0);
    const int64_t src_h     = src_shape.GetDim(2);
    const int64_t src_w     = src_shape.GetDim(3);
    const int64_t dst_h     = ((src_h + 2 * param_.pad_h - param_.dilation_h * (param_.kernel_h - 1) - 1) / param_.stride_h + 1);
    const int64_t dst_w     = ((src_w + 2 * param_.pad_w - param_.dilation_w * (param_.kernel_w - 1) - 1) / param_.stride_w + 1);
    ppl::nn::TensorShape dst_shape;
    dst_shape.Reshape({num_batch, num_output, dst_h, dst_w});

    uint64_t cvt_filter_size = conv_winograd_b4f3_get_converted_filter_size_fp16(
        channels, num_output, param_.group);
    uint64_t cvt_bias_size = CEIL8(num_output) * sizeof(__fp16);
    uint64_t src_size      = num_batch * CEIL8(channels) * src_h * src_w * sizeof(__fp16);
    uint64_t dst_size      = num_batch * CEIL8(num_output) * dst_h * dst_w * sizeof(__fp16);
    __fp16 *cvt_filter     = (__fp16 *)allocator_->Alloc(cvt_filter_size);
    __fp16 *cvt_bias       = (__fp16 *)allocator_->Alloc(cvt_bias_size);
    __fp16 *src            = (__fp16 *)allocator_->Alloc(src_size);
    __fp16 *dst            = (__fp16 *)allocator_->Alloc(dst_size);

    for (uint64_t idx = 0; idx < cvt_filter_size / sizeof(__fp16); idx++) {
        cvt_filter[idx] = float(rand()) / float((RAND_MAX)) - 0.5;
    }
    for (uint64_t idx = 0; idx < cvt_bias_size / sizeof(__fp16); idx++) {
        cvt_bias[idx] = float(rand()) / float((RAND_MAX)) - 0.5;
    }
    for (uint64_t idx = 0; idx < src_size / sizeof(__fp16); idx++) {
        src[idx] = float(rand()) / float((RAND_MAX)) - 0.5;
    }
    for (uint64_t idx = 0; idx < dst_size / sizeof(__fp16); idx++) {
        dst[idx] = float(rand()) / float((RAND_MAX)) - 0.5;
    }

    std::vector<int64_t> candidate_oc_blk_list   = {1024};
    std::vector<int64_t> candidate_ic_blk_list   = {192};
    std::vector<int64_t> candidate_tile_blk_list = {140};

    int64_t ic_blk_est   = 192;
    int64_t tile_blk_est = 140;
    const int64_t target_l1_cache_size = 64 * 1024;
    const int64_t num_independent_fma  = 8;
    if (ic_blk_est > ic_g_pck) {
        tile_blk_est = (target_l1_cache_size / sizeof(__fp16) - 2 * OCBLK() * ic_g_pck) / (2 * OCBLK() + ic_g_pck);
        if (tile_blk_est % N8CX_HGEMM_N10_BLOCK0() != 0 && tile_blk_est % 10 < num_independent_fma) {
            tile_blk_est = (tile_blk_est / N8CX_HGEMM_N10_BLOCK0()) * N8CX_HGEMM_N10_BLOCK0();
        }
        ic_blk_est = ic_g_pck;
    }
    candidate_ic_blk_list[0]   = ic_blk_est;
    candidate_tile_blk_list[0] = tile_blk_est;

    if (tune_blocksize) {
        candidate_oc_blk_list   = {1024};
        candidate_ic_blk_list   = {64, 128, 192, 256};
        candidate_tile_blk_list = {32, 64,  140, 256};
    }

    int64_t best_oc_blk   = 1024;
    int64_t best_ic_blk   = 192;
    int64_t best_tile_blk = 140;
    int64_t best_run_time = std::numeric_limits<int64_t>::max();

    const int num_warmup_iter    = 1;
    const int num_benchmark_iter = 5;
    for (auto oc_seg : candidate_oc_blk_list) {
        for (auto ic_seg : candidate_ic_blk_list) {
            for (auto tile_seg : candidate_tile_blk_list) {
                sched_param_.oc_seg   = oc_seg;
                sched_param_.ic_seg   = ic_seg;
                sched_param_.tile_seg = tile_seg;

                auto conv_exe = gen_executor();
                conv_exe->set_cvt_filter(cvt_filter);
                conv_exe->set_cvt_bias(cvt_bias);
                conv_exe->set_src(src);
                conv_exe->set_src_shape(&src_shape);
                conv_exe->set_dst(dst);
                conv_exe->set_dst_shape(&dst_shape);
                conv_exe->prepare();
                uint64_t tmp_buf_size = conv_exe->cal_temp_buffer_size();
                __fp16 *tmp_buffer    = (__fp16 *)allocator_->Alloc(tmp_buf_size);
                conv_exe->set_temp_buffer(tmp_buffer);

                for (int i = 0; i < num_warmup_iter; i++) {
                    conv_exe->execute();
                }

                auto begin_ts = std::chrono::system_clock::now();
                for (int i = 0; i < num_benchmark_iter; i++) {
                    conv_exe->execute();
                }
                auto end_ts = std::chrono::system_clock::now();

                int64_t elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(end_ts - begin_ts).count();
                if (elapsed_time < best_run_time) {
                    best_oc_blk   = oc_seg;
                    best_ic_blk   = ic_seg;
                    best_tile_blk = tile_seg;
                    best_run_time = elapsed_time;
                }

                allocator_->Free(tmp_buffer);
                delete conv_exe;

                if ((int64_t)tile_seg >= num_batch * ((src_h + 3) / 4) * ((src_w + 3) / 4)) break;
            }
            if ((int64_t)ic_seg >= channels / param_.group) break;
        }
        if ((int64_t)oc_seg >= num_output / param_.group) break;
    }

    cvt_filter_ = nullptr;
    cvt_bias_   = nullptr;
    allocator_->Free(cvt_filter);
    allocator_->Free(cvt_bias);
    allocator_->Free(src);
    allocator_->Free(dst);

    sched_param_.oc_seg   = best_oc_blk;
    sched_param_.ic_seg   = best_ic_blk;
    sched_param_.tile_seg = best_tile_blk;
#ifdef PPLNN_ENABLE_KERNEL_PROFILING
    LOG(INFO) << "choose sp param oc: " << sched_param_.oc_seg;
    LOG(INFO) << "choose sp param ic: " << sched_param_.ic_seg;
    LOG(INFO) << "choose sp param tile: " << sched_param_.tile_seg;
    LOG(INFO) << "best run time: " << best_run_time / num_benchmark_iter / 1000 << " ms";
#endif
    run_time = (double)best_run_time / (double)num_benchmark_iter;
    return ppl::common::RC_SUCCESS;
}

ppl::common::RetCode conv2d_wgb4f3_fp16_offline_manager::gen_cvt_weights(const void *filter, const void *bias)
{
    if (cvt_bias_ != nullptr || cvt_filter_ != nullptr) {
        return ppl::common::RC_PERMISSION_DENIED;
    }

    const int64_t num_output = param_.num_output;
    const int64_t channels   = param_.channels;

    cvt_bias_size_               = CEIL8(num_output) * sizeof(__fp16);
    cvt_bias_                    = allocator_->Alloc(cvt_bias_size_);
    int64_t padding_offset_bytes = num_output * sizeof(__fp16);
    int64_t padding_bytes        = (CEIL8(num_output) - num_output) * sizeof(__fp16);
    memcpy(cvt_bias_, bias, padding_offset_bytes);
    memset((uint8_t *)cvt_bias_ + padding_offset_bytes, 0, padding_bytes);

    cvt_filter_size_ = conv_winograd_b4f3_get_converted_filter_size_fp16(
        channels, num_output, param_.group);
    cvt_filter_ = (__fp16 *)allocator_->Alloc(cvt_filter_size_);

    const int64_t ic_group = channels / param_.group;
    const int64_t oc_group = num_output / param_.group;
    size_t buffer_size     = WGB4F3_NSET() * CEIL8(ic_group) * CEIL8(oc_group) * sizeof(__fp16);
    __fp16 *aux_buffer     = (__fp16 *)allocator_->Alloc(buffer_size);

    conv_winograd_b4f3_convert_filter_fp16(
        (const __fp16 *)filter,
        (__fp16 *)cvt_filter_,
        aux_buffer,
        channels,
        num_output,
        param_.group,
        sched_param_.ic_seg,
        sched_param_.oc_seg);

    allocator_->Free(aux_buffer);
    return ppl::common::RC_SUCCESS;
}

conv2d_runtime_executor *conv2d_wgb4f3_fp16_offline_manager::gen_executor()
{
    return new conv2d_wgb4f3_fp16_runtime_executor(&param_, cvt_filter_, cvt_bias_, sched_param_);
}

#undef CBLK
#undef ICBLK
#undef OCBLK

}}}}; // namespace ppl::kernel::arm_server::neon

#endif
