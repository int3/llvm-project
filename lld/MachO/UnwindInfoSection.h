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
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"

namespace lld::macho {

class UnwindInfoSection : public SyntheticSection {
public:
  // If all functions are free of unwind info, we can omit the unwind info
  // section entirely.
  bool isNeeded() const override { return !allEntriesAreOmitted; }
  // \p fastIsec is the ConcatInputSection whose unwindSymbolIndex slot holds
  // this symbol's index, or null when the fallback map must be used;
  // \p hasUnwindEntry is d->unwindEntry() != nullptr. Both are computed by the
  // parallel classification pass in Writer::scanSymbols() so that this serial
  // replay does not have to dereference the Defined.
  void addSymbol(const Defined *d, ConcatInputSection *fastIsec,
                 bool hasUnwindEntry);
  // Grow the symbol slots and their entry bits for up to \p n more
  // addSymbol() calls, so neither reallocates once per slot.
  void reserveSymbols(size_t n) {
    symbols.reserve(symbols.size() + n);
    symbolHasEntry.reserve(symbols.size() + n);
  }
  virtual void prepare() = 0;

protected:
  UnwindInfoSection();

  std::vector<const Defined *> symbols;
  // symbolHasEntry[i] is set when symbols[i]->unwindEntry() is non-null, so
  // prepare() need not dereference every recorded symbol.
  llvm::BitVector symbolHasEntry;
  llvm::DenseMap<
      std::pair<const InputSection *, uint64_t /*Defined::value*/>, uint32_t>
      nonZeroSymbols;
  bool allEntriesAreOmitted = true;
};

UnwindInfoSection *makeUnwindInfoSection();

} // namespace lld::macho

#endif
