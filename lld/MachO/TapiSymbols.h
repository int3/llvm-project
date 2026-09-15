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

#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/TextAPI/InterfaceFile.h"
#include <memory>
#include <vector>

namespace lld::macho {

// A publishable symbol under its final linker name -- the ObjC class,
// metaclass, EH-type and ivar prefixes are already applied -- with the name
// hash computed once, off the ordered walk.
struct PreparedTapiName {
  explicit PreparedTapiName(llvm::CachedHashStringRef name, bool isWeakDef,
                            bool isTlv)
      : name(name), isWeakDef(isWeakDef), isTlv(isTlv) {}

  llvm::CachedHashStringRef name;
  bool isWeakDef;
  bool isTlv;
};

// Which of the three "$ld$" actions a record carries. Records for any other
// action are not created: handleLDSymbol() consumes them without an effect.
enum class PreparedLDAction : uint8_t { Previous, InstallName, Hide };

// A "$ld$" string that does not parse still warns, at its original position and
// with its original text, so the parse failure travels as a record rather than
// being applied.
enum class PreparedLDWarning : uint8_t {
  None,
  StartVersion,
  EndVersion,
  CompatVersion,
  OSVersion,
  HiddenVersion,
};

// A pre-parsed "$ld$" symbol. Everything the ordered thread still has to do is
// mutate the dylib or the symbol table; all of the splitting, the version
// parsing and the platform and deployment-window tests happened on a worker.
// Records whose action cannot apply to this target -- a platform, a version
// window or an "os<version>" condition that excludes it -- are dropped during
// preparation, because handleLDSymbol() would have returned without an effect.
// Every record that survives preparation therefore applies.
struct PreparedLDSymbol {
  PreparedLDAction action = PreparedLDAction::Previous;
  PreparedLDWarning warning = PreparedLDWarning::None;
  // Previous: a compatibility version was given and parsed, so it replaces both
  // the dylib's compatibility version and a synthetic dylib's current version.
  bool hasCompatVersion = false;
  uint32_t compatVersion = 0;
  // The whole "$ld$..." name, for the diagnostics. Points into the interface.
  llvm::StringRef originalName;
  // Previous and InstallName.
  llvm::StringRef installName;
  // Previous: the symbol to add to the synthetic dylib; empty when the record
  // renames the dylib it is in.
  llvm::StringRef symbolName;
  // Hide: the name to hide, in the prepared allocator so that it outlives the
  // interface, with its hash computed once.
  llvm::CachedHashStringRef hiddenName{llvm::StringRef()};
};

// Which symbols need the ordered handleLDSymbol() call, and the deterministic
// order of the remaining publishable symbols. Nothing here touches the symbol
// table, the string saver or any other ordered state.
struct PreparedTapiSymbols {
  // Names materialized off the ordered walk. Ownership moves to the DylibFile,
  // so the names outlive the InterfaceFile they were read from.
  std::unique_ptr<llvm::BumpPtrAllocator> names;
  // Arch-compatible "$ld$" symbols that can still have an effect, in interface
  // order. The ordered thread applies them in this order.
  std::vector<PreparedLDSymbol> ldSymbols;
  // The remaining arch-compatible symbols of a publishable kind under their
  // final names, ordered by interface symbol name (interface symbol order is
  // non-deterministic) and then by the order the names are derived in.
  std::vector<PreparedTapiName> normalNames;
  // normalNames pre-partitioned by symbol-table shard for the batched
  // publication: shardOrder[shardStart[s] .. shardStart[s + 1]) are the indices
  // of the names routed to shard s, in normalNames order. Computed here so that
  // the publish step neither re-hashes nor re-scans. Empty when there are no
  // normal names.
  std::vector<uint32_t> shardStart;
  std::vector<uint32_t> shardOrder;
};

} // namespace lld::macho

#endif
