//===- RelocationsTest.cpp
//-------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../MachO/Relocations.h"
#include "../../MachO/InputSection.h"
#include "lld/Common/CommonLinkerContext.h"

#include "gtest/gtest.h"

using namespace lld;
using namespace lld::macho;

namespace {
class RelocationTest : public testing::Test {
protected:
  void SetUp() override { new CommonLinkerContext; }
  void TearDown() override { CommonLinkerContext::destroy(); }
};

static Symbol *symbol(uintptr_t value) {
  return reinterpret_cast<Symbol *>(value);
}

static InputSection *section(uintptr_t value) {
  return reinterpret_cast<InputSection *>(value);
}

TEST_F(RelocationTest, InlineAddendBoundariesAndFields) {
  Relocation defaultConstructed;
  EXPECT_EQ(llvm::MachO::GENERIC_RELOC_INVALID, defaultConstructed.getType());
  EXPECT_FALSE(defaultConstructed.isPCRel());
  EXPECT_EQ(0, defaultConstructed.getLength());
  EXPECT_EQ(0U, defaultConstructed.getOffset());
  EXPECT_EQ(0, defaultConstructed.getAddend());
  EXPECT_TRUE(defaultConstructed.getReferent().isNull());

  Relocation low(255, true, 3, UINT32_MAX, -1048576, symbol(0x1000));
  EXPECT_EQ(255, low.getType());
  EXPECT_TRUE(low.isPCRel());
  EXPECT_EQ(3, low.getLength());
  EXPECT_EQ(UINT32_MAX, low.getOffset());
  EXPECT_EQ(-1048576, low.getAddend());
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(low.getReferent()));

  Relocation high(0, false, 0, 0, 1048575, section(0x2000));
  EXPECT_EQ(1048575, high.getAddend());
  EXPECT_EQ(section(0x2000), llvm::cast<InputSection *>(high.getReferent()));

  Relocation positiveOverflow(0, false, 0, 0, 1048576, symbol(0x1000));
  EXPECT_EQ(1048576, positiveOverflow.getAddend());
  Relocation negativeOverflow(0, false, 0, 0, -1048577, symbol(0x1000));
  EXPECT_EQ(-1048577, negativeOverflow.getAddend());

  // Null referents and type 255 are valid normalized relocation states.
  Relocation null(255, false, 0, UINT32_MAX, 0, Relocation::Referent());
  EXPECT_TRUE(null.getReferent().isNull());
  EXPECT_EQ(255, null.getType());
  EXPECT_EQ(UINT32_MAX, null.getOffset());
}

TEST_F(RelocationTest, WideAddendsAndCopyOnWrite) {
  constexpr int64_t wide = INT64_C(1) << 40;
  constexpr int64_t anotherWide = -(INT64_C(1) << 41);
  Relocation original(7, false, 2, 9, wide, symbol(0x1000));
  Relocation copy = original;

  copy.setReferent(section(0x2000));
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(original.getReferent()));
  EXPECT_EQ(wide, original.getAddend());
  EXPECT_EQ(section(0x2000), llvm::cast<InputSection *>(copy.getReferent()));
  EXPECT_EQ(wide, copy.getAddend());

  copy.setAddend(anotherWide);
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(original.getReferent()));
  EXPECT_EQ(wide, original.getAddend());
  EXPECT_EQ(section(0x2000), llvm::cast<InputSection *>(copy.getReferent()));
  EXPECT_EQ(anotherWide, copy.getAddend());

  Relocation anotherCopy = original;
  anotherCopy.setTarget(section(0x3000), INT64_MIN);
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(original.getReferent()));
  EXPECT_EQ(wide, original.getAddend());
  EXPECT_EQ(section(0x3000),
            llvm::cast<InputSection *>(anotherCopy.getReferent()));
  EXPECT_EQ(INT64_MIN, anotherCopy.getAddend());
}

TEST_F(RelocationTest, WideInlineWideTransitions) {
  Relocation r(1, false, 2, 0, INT64_MAX, symbol(0x1000));
  EXPECT_EQ(INT64_MAX, r.getAddend());
  r.setAddend(17);
  EXPECT_EQ(17, r.getAddend());
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(r.getReferent()));
  r.setAddend(-(INT64_C(1) << 40));
  EXPECT_EQ(-(INT64_C(1) << 40), r.getAddend());
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(r.getReferent()));
}

TEST_F(RelocationTest, MetadataSettersPreserveTarget) {
  Relocation r(1, false, 0, 3, -17, symbol(0x1000));
  r.setType(255);
  r.setPCRel(true);
  r.setLength(3);
  r.setOffset(UINT32_MAX);
  EXPECT_EQ(255, r.getType());
  EXPECT_TRUE(r.isPCRel());
  EXPECT_EQ(3, r.getLength());
  EXPECT_EQ(UINT32_MAX, r.getOffset());
  EXPECT_EQ(-17, r.getAddend());
  EXPECT_EQ(symbol(0x1000), llvm::cast<Symbol *>(r.getReferent()));

  constexpr int64_t wide = INT64_C(1) << 40;
  r.setTarget(section(0x2000), wide);
  r.setType(7);
  r.setPCRel(false);
  r.setLength(2);
  r.setOffset(9);
  EXPECT_EQ(7, r.getType());
  EXPECT_FALSE(r.isPCRel());
  EXPECT_EQ(2, r.getLength());
  EXPECT_EQ(9U, r.getOffset());
  EXPECT_EQ(wide, r.getAddend());
  EXPECT_EQ(section(0x2000), llvm::cast<InputSection *>(r.getReferent()));

  r.setAddend(-19);
  EXPECT_EQ(7, r.getType());
  EXPECT_FALSE(r.isPCRel());
  EXPECT_EQ(2, r.getLength());
  EXPECT_EQ(9U, r.getOffset());
  EXPECT_EQ(-19, r.getAddend());
  EXPECT_EQ(section(0x2000), llvm::cast<InputSection *>(r.getReferent()));
}
} // namespace
