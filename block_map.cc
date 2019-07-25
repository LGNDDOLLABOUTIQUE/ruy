/* Copyright 2019 Google LLC. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "block_map.h"

#include "third_party/gemmlowp/profiling/instrumentation.h"
#include "check_macros.h"
#include "opt_set.h"
#include "size_util.h"

namespace ruy {

void GetBlockByIndex(const BlockMap& block_map, std::uint32_t index,
                     SidePair<std::uint16_t>* block) {
  gemmlowp::ScopedProfilingLabel label("GetBlockByIndex");
  std::uint16_t rectr =
      index & ((1 << block_map.rectangularness_log2[Side::kLhs]) - 1);
  std::uint16_t rectc =
      index & ((1 << block_map.rectangularness_log2[Side::kRhs]) - 1);

  std::uint16_t n1 = index >> (block_map.rectangularness_log2[Side::kLhs] +
                               block_map.rectangularness_log2[Side::kRhs]);
  RUY_DCHECK_EQ(index, (n1 << (block_map.rectangularness_log2[Side::kLhs] +
                               block_map.rectangularness_log2[Side::kRhs])) +
                           rectr + rectc);

  std::uint16_t br, bc;
  if (block_map.traversal_order == BlockMapTraversalOrder::kLinear) {
    br = n1 & ((1 << block_map.num_blocks_base_log2) - 1);
    bc = n1 >> block_map.num_blocks_base_log2;
  } else {
    // Decode fractal z-order
    std::uint16_t n2 =
        (n1 & 0x9999) | ((n1 & 0x4444) >> 1) | ((n1 & 0x2222) << 1);
    std::uint16_t n4 =
        (n2 & 0xc3c3) | ((n2 & 0x3030) >> 2) | ((n2 & 0x0c0c) << 2);
    std::uint16_t n8 =
        (n4 & 0xf00f) | ((n4 & 0x0f00) >> 4) | ((n4 & 0x00f0) << 4);
    br = n8 & 0xff;
    bc = n8 >> 8;
    if (block_map.traversal_order == BlockMapTraversalOrder::kFractalU) {
      // Change fractal z-order to u-order
      br ^= bc;
    }
  }

  br = (br << block_map.rectangularness_log2[Side::kLhs]) + rectr;
  bc = (bc << block_map.rectangularness_log2[Side::kRhs]) + rectc;

  // Store
  (*block)[Side::kLhs] = br;
  (*block)[Side::kRhs] = bc;
}

namespace {

int floor_log2_quotient(int num, int denom) {
  if (num <= denom) {
    return 0;
  }
  int log2_quotient = floor_log2(num) - ceil_log2(denom);
  if ((denom << (log2_quotient + 1)) <= num) {
    log2_quotient++;
  }
  return log2_quotient;
}

}  // namespace

void MakeBlockMap(int rows, int cols, int depth, int kernel_rows,
                  int kernel_cols, int lhs_scalar_size, int rhs_scalar_size,
                  int cache_friendly_traversal_threshold, BlockMap* block_map) {
  gemmlowp::ScopedProfilingLabel label("MakeBlockMap");
  RUY_DCHECK_GE(rows, kernel_rows);
  RUY_DCHECK_GE(cols, kernel_cols);

  block_map->traversal_order = BlockMapTraversalOrder::kLinear;
  if (RUY_OPT_ENABLED(RUY_OPT_FRACTAL) &&
      (rows * lhs_scalar_size + cols * rhs_scalar_size) * depth >=
          cache_friendly_traversal_threshold) {
    block_map->traversal_order = RUY_OPT_ENABLED(RUY_OPT_FRACTAL_U)
                                     ? BlockMapTraversalOrder::kFractalU
                                     : BlockMapTraversalOrder::kFractalZ;
  }

  // See the comment on BlockMap in block_map.h.
  // The destination matrix shape (rows x cols) is to be subdivided into a
  // square (N x N) grid of blocks, whose shapes must be multiples of the
  // kernel block shape (kernel_rows x kernel_cols).
  // Inside each of these N*N blocks, we may have one further level of
  // subdivision either along rows or along cols but not both, to handle
  // better the highly rectangular cases. That is what we call
  // 'rectangularness'.  This extra level of subdivision is into
  // (1 << rows_rectangularness_log2) blocks along rows dimension, or into
  // (1 << cols_rectangularness_log2) blocks along cols dimension.
  int rows_rectangularness_log2 = 0;
  int cols_rectangularness_log2 = 0;
  // In order to compute these rectangularness values, we need to divide
  // the destination matrix's aspect ratio,
  //    rows / cols
  // by the kernel block's aspect ratio,
  //    kernel_block_rows / kernel_block_cols.
  // The quotient of these two quotients simplifies to
  //    (rows * kernel_cols) / (cols * kernel_rows)
  // Whence the introduction of the following products:
  const int rows_times_kernel_cols = rows * kernel_cols;
  const int cols_times_kernel_rows = cols * kernel_rows;
  if (rows_times_kernel_cols > cols_times_kernel_rows) {
    rows_rectangularness_log2 =
        floor_log2_quotient(rows_times_kernel_cols, cols_times_kernel_rows);
    // Sanity check that we did not over-estimate rows_rectangularness_log2.
    RUY_DCHECK_GE(rows_times_kernel_cols >> rows_rectangularness_log2,
                  cols_times_kernel_rows);
  } else if (cols_times_kernel_rows > rows_times_kernel_cols) {
    cols_rectangularness_log2 =
        floor_log2_quotient(cols_times_kernel_rows, rows_times_kernel_cols);
    // Sanity check that we did not over-estimate cols_rectangularness_log2.
    RUY_DCHECK_GE(cols_times_kernel_rows >> cols_rectangularness_log2,
                  rows_times_kernel_cols);
  }

  RUY_DCHECK(!rows_rectangularness_log2 || !cols_rectangularness_log2);

  const int size = std::min(rows, cols);
  const int size_floor_log2 = floor_log2(size);
  const int depth_ceil_log2 = ceil_log2(depth);
  const int kernel_width_log2 = ceil_log2(std::max(kernel_cols, kernel_rows));

  // l1_size_log2 was originally, coarsely speaking the number of rows of LHS,
  // or the number of columns of RHS in a matrix multiplication that we expect,
  // to fit in L1 cache.
  //
  // This initial rationale is not necessarily still relevant. The logic below
  // was determined empirically, not in a principled way.
  int l1_size_log2;
  if (size_floor_log2 <= 3) {
    l1_size_log2 = size_floor_log2;
  } else if (size_floor_log2 <= 6) {
    l1_size_log2 = 4;
  } else {
    l1_size_log2 = 5;
  }

  // The 15 here implicitly encodes target a 32k L1 cache (2^15 == 32k).
  // Once again this only has a distant memory of being originally motivated
  // by such clear principles linking this logic to cache sizes.
  l1_size_log2 = std::min(
      l1_size_log2, 15 - depth_ceil_log2 -
                        ceil_log2(std::max(lhs_scalar_size, rhs_scalar_size)));
  l1_size_log2 = std::max(l1_size_log2, kernel_width_log2);
  l1_size_log2 = std::min(l1_size_log2, size_floor_log2);
  l1_size_log2 = std::max(l1_size_log2, size_floor_log2 - 8);

  int num_blocks_base_log2 = size_floor_log2 - l1_size_log2;
  RUY_DCHECK_GE(num_blocks_base_log2, 0);
  RUY_DCHECK_LE(num_blocks_base_log2, 8);
  if (num_blocks_base_log2 == 0) {
    if ((rows % kernel_rows) || (cols % kernel_cols)) {
      num_blocks_base_log2 = 1;
    }
  }
  RUY_DCHECK_LE(num_blocks_base_log2 + rows_rectangularness_log2, 16);
  RUY_DCHECK_LE(num_blocks_base_log2 + cols_rectangularness_log2, 16);

  int rows_rounded_up = round_up_pot(rows, kernel_rows);
  int cols_rounded_up = round_up_pot(cols, kernel_cols);

  const int num_blocks_of_rows_log2 =
      num_blocks_base_log2 + rows_rectangularness_log2;
  const int num_blocks_of_cols_log2 =
      num_blocks_base_log2 + cols_rectangularness_log2;

  std::uint16_t smallr =
      round_down_pot(rows_rounded_up >> num_blocks_of_rows_log2, kernel_rows);
  std::uint16_t smallc =
      round_down_pot(cols_rounded_up >> num_blocks_of_cols_log2, kernel_cols);
  std::uint16_t missr =
      round_up_pot(rows_rounded_up - (smallr << num_blocks_of_rows_log2),
                   kernel_rows) /
      kernel_rows;
  std::uint16_t missc =
      round_up_pot(cols_rounded_up - (smallc << num_blocks_of_cols_log2),
                   kernel_cols) /
      kernel_cols;

  block_map->dims[Side::kLhs] = rows;
  block_map->dims[Side::kRhs] = cols;
  block_map->kernel_dims[Side::kLhs] = kernel_rows;
  block_map->kernel_dims[Side::kRhs] = kernel_cols;
  block_map->num_blocks_base_log2 = num_blocks_base_log2;
  block_map->rectangularness_log2[Side::kLhs] = rows_rectangularness_log2;
  block_map->rectangularness_log2[Side::kRhs] = cols_rectangularness_log2;
  block_map->small_block_dims[Side::kLhs] = smallr;
  block_map->small_block_dims[Side::kRhs] = smallc;
  block_map->large_blocks[Side::kLhs] = missr;
  block_map->large_blocks[Side::kRhs] = missc;
}

void GetBlockMatrixCoords(Side side, const BlockMap& block_map,
                          const SidePair<std::uint16_t>& block, int* start,
                          int* end) {
  gemmlowp::ScopedProfilingLabel label("GetBlockMatrixCoords");
  const std::uint16_t b = block[side];
  *start =
      b * block_map.small_block_dims[side] +
      std::min(b, block_map.large_blocks[side]) * block_map.kernel_dims[side];
  *end = *start + block_map.small_block_dims[side] +
         (b < block_map.large_blocks[side] ? block_map.kernel_dims[side] : 0);

  RUY_DCHECK_EQ(0, *start % block_map.kernel_dims[side]);
  RUY_DCHECK_EQ(0, *end % block_map.kernel_dims[side]);
  RUY_DCHECK_LE(*end, block_map.dims[side]);
  RUY_DCHECK_LT(*start, *end);
  RUY_DCHECK_GE(*start, 0);
}

void GetBlockMatrixCoords(const BlockMap& block_map,
                          const SidePair<std::uint16_t>& block,
                          SidePair<int>* start, SidePair<int>* end) {
  for (Side side : {Side::kLhs, Side::kRhs}) {
    GetBlockMatrixCoords(side, block_map, block, &(*start)[side],
                         &(*end)[side]);
  }
}

}  // namespace ruy
