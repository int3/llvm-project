//===- RelocationsTest.cpp
//-------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../MachO/Relocations.h"
#include "../../MachO/InputFiles.h"
#include "../../MachO/InputSection.h"
#include "../../MachO/SymbolTable.h"
#include "../../MachO/Symbols.h"
#include "../../MachO/Target.h"
#include "lld/Common/CommonLinkerContext.h"

#include "gtest/gtest.h"

#include <memory>

using namespace lld;
using namespace lld::macho;

namespace {
class RelocationTest : public testing::Test {
protected:
  void SetUp() override {
    new CommonLinkerContext;
    config = std::make_unique<Configuration>();
  }
  void TearDown() override {
    config.reset();
    CommonLinkerContext::destroy();
  }
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

TEST_F(RelocationTest, SymbolQueriesFollowCurrentKind) {
  Defined absolute("absolute", nullptr, nullptr, 0x1234, 0,
                   /*isWeakDef=*/true, /*isExternal=*/true,
                   /*isPrivateExtern=*/false, /*includeInSymtab=*/true,
                   /*isReferencedDynamically=*/false, /*noDeadStrip=*/false);
  Symbol *sym = &absolute;
  EXPECT_EQ(0x1234U, sym->getVA());
  EXPECT_TRUE(sym->isWeakDef());
  EXPECT_FALSE(sym->isWeakRef());
  EXPECT_FALSE(sym->isTlv());

  Undefined undefined("undefined", nullptr, RefState::Weak,
                      /*wasBitcodeSymbol=*/false);
  sym = &undefined;
  EXPECT_EQ(0U, sym->getVA());
  EXPECT_FALSE(sym->isWeakDef());
  EXPECT_TRUE(sym->isWeakRef());
  EXPECT_FALSE(sym->isTlv());

  DylibSymbol dylib(nullptr, "dylib", /*isWeakDef=*/true, RefState::Weak,
                    /*isTlv=*/true);
  sym = &dylib;
  EXPECT_EQ(0U, sym->getVA());
  EXPECT_TRUE(sym->isWeakDef());
  EXPECT_TRUE(sym->isWeakRef());
  EXPECT_TRUE(sym->isTlv());

  CommonSymbol common("common", nullptr, 4, 4, false);
  sym = &common;
  EXPECT_EQ(0U, sym->getVA());
  EXPECT_FALSE(sym->isWeakDef());
  EXPECT_FALSE(sym->isWeakRef());
  EXPECT_FALSE(sym->isTlv());

  AliasSymbol alias(nullptr, "alias", "absolute", false);
  sym = &alias;
  EXPECT_EQ(0U, sym->getVA());
  EXPECT_FALSE(sym->isWeakDef());
  EXPECT_FALSE(sym->isWeakRef());
  EXPECT_FALSE(sym->isTlv());

  Section section(nullptr, "__DATA", "__thread_vars",
                  llvm::MachO::S_THREAD_LOCAL_VARIABLES, 0);
  ConcatInputSection isec(section, {});
  Defined tlv("tlv", nullptr, &isec, 0, 0, /*isWeakDef=*/false,
              /*isExternal=*/true, /*isPrivateExtern=*/false,
              /*includeInSymtab=*/true, /*isReferencedDynamically=*/false,
              /*noDeadStrip=*/false);
  EXPECT_TRUE(static_cast<Symbol *>(&tlv)->isTlv());
}

TEST_F(RelocationTest, SymbolUnionReplacementKeepsQueriesAndFlags) {
  config->deadStrip = true;
  DylibFile file(nullptr);
  SymbolUnion storage;
  Symbol *sym = new (&storage) Undefined("undefined", &file,
                                          RefState::Strong, false);
  EXPECT_FALSE(sym->isLive());
  EXPECT_FALSE(sym->isUsedInRegularObj);
  EXPECT_FALSE(sym->isWeakRef());
  sym->isUsedInRegularObj = true;
  sym->used.store(true, std::memory_order_relaxed);

  Symbol *const address = sym;
  sym = replaceSymbol<DylibSymbol>(sym, &file, "dylib",
                                   /*isWeakDef=*/true, RefState::Weak,
                                   /*isTlv=*/true);
  EXPECT_EQ(address, sym);
  EXPECT_TRUE(sym->isUsedInRegularObj);
  EXPECT_TRUE(sym->isLive());
  EXPECT_EQ(0U, sym->getVA());
  EXPECT_TRUE(sym->isWeakDef());
  EXPECT_TRUE(sym->isWeakRef());
  EXPECT_TRUE(sym->isTlv());

  sym = replaceSymbol<Defined>(sym, "defined", &file, nullptr, 0x5678, 0,
                               /*isWeakDef=*/false, /*isExternal=*/true,
                               /*isPrivateExtern=*/false,
                               /*includeInSymtab=*/true,
                               /*isReferencedDynamically=*/false,
                               /*noDeadStrip=*/false);
  EXPECT_EQ(address, sym);
  EXPECT_TRUE(sym->isUsedInRegularObj);
  EXPECT_TRUE(sym->isLive());
  EXPECT_EQ(0x5678U, sym->getVA());
  EXPECT_FALSE(sym->isWeakDef());
  EXPECT_FALSE(sym->isWeakRef());
  EXPECT_FALSE(sym->isTlv());

  sym = replaceSymbol<Undefined>(sym, "undefined-again", &file,
                                 RefState::Weak, false);
  EXPECT_EQ(address, sym);
  EXPECT_TRUE(sym->isUsedInRegularObj);
  EXPECT_TRUE(sym->isLive());
  EXPECT_EQ(0U, sym->getVA());
  EXPECT_FALSE(sym->isWeakDef());
  EXPECT_TRUE(sym->isWeakRef());
  EXPECT_FALSE(sym->isTlv());
}

TEST_F(RelocationTest, SymbolTableConstructsFreshSymbolsDirectly) {
  SymbolTable table;
  DylibFile dylib(nullptr);
  TargetInfo *savedTarget = target;
  target = createX86_64TargetInfo();
  ObjFile native(MemoryBufferRef("", "native.o"), 0, "",
                 /*lazy=*/false, /*forceHidden=*/false,
                 /*compatArch=*/false);
  target = savedTarget;

  auto checkFresh = [&](Symbol *sym, StringRef name, bool live, bool regular) {
    EXPECT_EQ(name, sym->getName());
    EXPECT_EQ(live, sym->isLive());
    EXPECT_EQ(regular, sym->isUsedInRegularObj);
    EXPECT_EQ(UINT32_MAX, sym->gotIndex);
    EXPECT_EQ(UINT32_MAX, sym->symtabIndex);
    EXPECT_FALSE(sym->isInStubs());
  };
  for (bool deadStrip : {false, true}) {
    config->deadStrip = deadStrip;
    StringRef definedName = deadStrip ? "defined-dead" : "defined-live";
    StringRef undefinedName =
        deadStrip ? "undefined-dead" : "undefined-live";
    StringRef commonName = deadStrip ? "common-dead" : "common-live";
    StringRef dylibName = deadStrip ? "dylib-dead" : "dylib-live";
    StringRef lazyName = deadStrip ? "lazy-dead" : "lazy-live";
    Symbol *defined = table.addDefined(
        definedName, nullptr, nullptr, 0, 0, /*isWeakDef=*/false,
        /*isPrivateExtern=*/false,
        /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
        /*isWeakDefCanBeHidden=*/false);
    EXPECT_TRUE(isa<Defined>(defined));
    checkFresh(defined, definedName, !deadStrip, true);

    Symbol *undefined = table.addUndefined(undefinedName, nullptr, false);
    EXPECT_TRUE(isa<Undefined>(undefined));
    checkFresh(undefined, undefinedName, !deadStrip, true);

    Symbol *common = table.addCommon(commonName, nullptr, 4, 4, false);
    EXPECT_TRUE(isa<CommonSymbol>(common));
    checkFresh(common, commonName, !deadStrip, true);

    Symbol *dylibSymbol = table.addDylib(dylibName, &dylib, false, false);
    EXPECT_TRUE(isa<DylibSymbol>(dylibSymbol));
    checkFresh(dylibSymbol, dylibName, !deadStrip, false);

    Symbol *lazy = table.addLazyObject(lazyName, native);
    EXPECT_TRUE(isa<LazyObject>(lazy));
    checkFresh(lazy, lazyName, !deadStrip, true);
  }
}

TEST_F(RelocationTest, SymbolTableReplacesExistingSymbols) {
  SymbolTable table;
  config->deadStrip = true;
  DylibFile dylib(nullptr);

  Symbol *symbol = table.addUndefined("replace", nullptr, false);
  symbol->used.store(true, std::memory_order_relaxed);
  symbol->isUsedInRegularObj = true;
  Symbol *const address = symbol;

  symbol = table.addDylib("replace", &dylib, false, false);
  EXPECT_EQ(address, symbol);
  EXPECT_TRUE(symbol->isLive());
  EXPECT_TRUE(symbol->isUsedInRegularObj);
  EXPECT_TRUE(dylib.isReferenced());

  symbol = table.addDefined(
      "replace", nullptr, nullptr, 0, 0, /*isWeakDef=*/false,
      /*isPrivateExtern=*/false, /*isReferencedDynamically=*/false,
      /*noDeadStrip=*/false, /*isWeakDefCanBeHidden=*/false);
  EXPECT_EQ(address, symbol);
  EXPECT_TRUE(symbol->isLive());
  EXPECT_TRUE(symbol->isUsedInRegularObj);
  EXPECT_FALSE(dylib.isReferenced());

  Symbol *weak = table.addUndefined("upgrade", nullptr, true);
  EXPECT_EQ(RefState::Weak, cast<Undefined>(weak)->refState);
  Symbol *strong = table.addUndefined("upgrade", nullptr, false);
  EXPECT_EQ(weak, strong);
  EXPECT_EQ(RefState::Strong, cast<Undefined>(strong)->refState);
}
} // namespace
