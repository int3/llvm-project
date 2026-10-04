//===- CodeSignatureFlushTest.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../MachO/SyntheticSections.h"

#include "gtest/gtest.h"

using namespace lld::macho;

namespace {

// Checks that the ranges [i * stride, min((i + 1) * stride, total)) for
// i < numRanges start on page boundaries and cover every byte of [0, total).
void expectTiles(uint64_t total, uint64_t numRanges, uint64_t pageSize) {
  uint64_t stride = codeSignatureFlushStride(total, numRanges, pageSize);
  SCOPED_TRACE(testing::Message() << "total=" << total << " ranges="
                                  << numRanges << " page=" << pageSize);
  EXPECT_EQ(stride % pageSize, 0u);
  EXPECT_GT(stride, 0u);
  // The last range ends at or past the end of the output.
  EXPECT_GE(numRanges * stride, total);
  // No range is wasted on more than one page of rounding: a smaller stride
  // would leave a tail.
  EXPECT_LT(numRanges * (stride - pageSize), total);
}

TEST(CodeSignatureFlushTest, LastPartialPageIsCovered) {
  for (uint64_t page : {4096u, 16384u}) {
    // total / 8 is a whole number of pages but total is not: flooring before
    // aligning gives a stride of exactly one page per range and leaves the
    // last byte unflushed.
    uint64_t total = 8 * page + 1;
    EXPECT_LT(8 * llvm::alignTo(total / 8, page), total);
    expectTiles(total, 8, page);
    // The same at a realistic output size (~206 MB at 16 KiB pages, the Rust
    // debug output); only a tail of under eight bytes past such a size fails.
    total = 8 * 1572 * page + 5;
    EXPECT_LT(8 * llvm::alignTo(total / 8, page), total);
    expectTiles(total, 8, page);
  }
}

TEST(CodeSignatureFlushTest, Sweep) {
  for (uint64_t page : {4096u, 16384u})
    for (uint64_t ranges : {1u, 2u, 7u, 8u, 16u})
      for (uint64_t total = 8 * 1024 * 1024; total < 8 * 1024 * 1024 + 3 * page;
           total += 509)
        expectTiles(total, ranges, page);
}

} // namespace
