//===- UnwindInfoSection.h ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_UNWIND_INFO_H
#define LLD_MACHO_UNWIND_INFO_H

#include "ConcatOutputSection.h"
#include "SyntheticSections.h"
#include "llvm/ADT/DenseMap.h"

namespace lld::macho {

class UnwindInfoSection : public SyntheticSection {
public:
  // If all functions are free of unwind info, we can omit the unwind info
  // section entirely.
  bool isNeeded() const override { return !allEntriesAreOmitted; }
  void addSymbol(const Defined *);
  virtual void prepare() = 0;

protected:
  UnwindInfoSection();

  std::vector<const Defined *> symbols;
  llvm::DenseMap<
      std::pair<const InputSection *, uint64_t /*Defined::value*/>, uint32_t>
      nonZeroSymbols;
  bool allEntriesAreOmitted = true;
};

UnwindInfoSection *makeUnwindInfoSection();

} // namespace lld::macho

#endif
