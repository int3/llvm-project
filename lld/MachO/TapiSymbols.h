//===- TapiSymbols.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The order-independent half of the TAPI DylibFile constructor, which the TAPI
// preparation pipeline runs off the ordered input walk.
//
// This is a private header rather than part of InputFiles.h because naming
// llvm::MachO::Symbol in a header that Driver.cpp includes would make the
// unqualified name Symbol ambiguous there, lld::macho::Symbol being the
// intended meaning throughout.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_TAPI_SYMBOLS_H
#define LLD_MACHO_TAPI_SYMBOLS_H

#include "llvm/TextAPI/InterfaceFile.h"
#include <vector>

namespace lld::macho {

// Which symbols need the ordered handleLDSymbol() call, and the deterministic
// order of the remaining publishable symbols. Nothing here touches the symbol
// table, the string saver or any other ordered state.
struct PreparedTapiSymbols {
  // Arch-compatible "$ld$" symbols, in interface order. handleLDSymbol() still
  // runs on these, in this order, on the ordered thread.
  std::vector<const llvm::MachO::Symbol *> ldSymbols;
  // The remaining arch-compatible symbols of a publishable kind, sorted by
  // name because interface symbol order is non-deterministic.
  std::vector<const llvm::MachO::Symbol *> normalSymbols;
};

} // namespace lld::macho

#endif
