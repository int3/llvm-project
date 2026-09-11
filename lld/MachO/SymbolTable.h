//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_SYMBOL_TABLE_H
#define LLD_MACHO_SYMBOL_TABLE_H

#include "Symbols.h"

#include "lld/Common/LLVM.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Object/Archive.h"
#include "llvm/Support/Allocator.h"

#include <mutex>
#include <vector>

namespace lld::macho {

class ArchiveFile;
class DylibFile;
class InputFile;
class ObjFile;
class InputSection;
class MachHeaderSection;
class Symbol;
class Defined;
class Undefined;

/*
 * Note that the SymbolTable handles name collisions by calling
 * replaceSymbol(), which does an in-place update of the Symbol via `placement
 * new`. Therefore, there is no need to update any relocations that hold
 * pointers the "old" Symbol -- they will automatically point to the new one.
 */
class SymbolTable {
public:
  Defined *addDefined(StringRef name, InputFile *, InputSection *,
                      uint64_t value, uint64_t size, bool isWeakDef,
                      bool isPrivateExtern, bool isReferencedDynamically,
                      bool noDeadStrip, bool isWeakDefCanBeHidden,
                      bool isCold = false);

  Defined *aliasDefined(Defined *src, StringRef target, InputFile *newFile,
                        bool makePrivateExtern = false);

  Symbol *addUndefined(StringRef name, InputFile *, bool isWeakRef);

  Symbol *addCommon(StringRef name, InputFile *, uint64_t size, uint32_t align,
                    bool isPrivateExtern);

  Symbol *addDylib(StringRef name, DylibFile *file, bool isWeakDef, bool isTlv);
  Symbol *addDynamicLookup(StringRef name);

  Symbol *addLazyArchive(StringRef name, ArchiveFile *file,
                         const llvm::object::Archive::Symbol &sym);
  Symbol *addLazyObject(StringRef name, InputFile &file);

  Defined *addSynthetic(StringRef name, InputSection *, uint64_t value,
                        bool isPrivateExtern, bool includeInSymtab,
                        bool referencedDynamically);

  ArrayRef<Symbol *> getSymbols() const { return symVector; }
  Symbol *find(llvm::CachedHashStringRef name);
  Symbol *find(StringRef name) { return find(llvm::CachedHashStringRef(name)); }

  // --- Parallel input phase ------------------------------------------------
  //
  // enableSharding() replaces the single name map with a fixed set of
  // independently locked shards, so that workers parsing different object files
  // can resolve records concurrently. It must run before any symbol exists,
  // because each slot allocated in this mode carries a hidden SlotMeta header.
  // It is a no-op layout change on its own: nothing folds until
  // beginParallelInput().
  void enableSharding();
  bool isSharded() const { return sharded; }

  // Between these two calls, add*() records that carry a ParseContext are
  // folded into the shards by rank and ordinal instead of being applied in
  // arrival order. endParallelInput() rebuilds symVector in the order the
  // serial linker would have created the slots.
  void beginParallelInput();
  void endParallelInput();
  bool isFolding() const { return folding; }

  // While folding, records added through the serial bodies (an -l dylib's
  // exports, say) take their ordinals from here so that their slots land in the
  // right place in symVector.
  ParseContext *serialCtx = nullptr;

  // Apply the deferred loser effects and the deferred records collected by the
  // fold. Both run serially, in ordinal order, after endParallelInput().
  void applyPendingCoalesces(llvm::MutableArrayRef<PendingCoalesce> pending);

  // addDefined() with the arguments bundled, used by the fold and its replay.
  Symbol *addDefinedRecord(const DeferredDefinedRecord &rec);

private:
  // Per-slot bookkeeping for the fold. Allocated immediately in front of the
  // SymbolUnion so that Symbol itself does not grow: mechanism 44 left only two
  // spare bytes in Symbol, so two ordinal fields would have pushed Symbol back
  // to 40 bytes and Defined to 72.
  struct SlotMeta {
    // The lowest ordinal of any record that has touched this slot. This is the
    // point at which the serial linker would have created it, and therefore the
    // slot's position in symVector.
    RecordOrdinal firstOrdinal;
    // The ordinal of the record that produced the symbol currently in the slot,
    // plus, in the top bit, a flag saying that one record for this name has been
    // deferred and so every later record for it must be deferred too, so that
    // the replay sees a single ordered stream. A file ordinal never reaches 2^31,
    // so the top bit of an ordinal is always free.
    RecordOrdinal winnerAndDeferred;

    static constexpr RecordOrdinal deferredBit = RecordOrdinal(1) << 63;
    RecordOrdinal winner() const { return winnerAndDeferred & ~deferredBit; }
    void setWinner(RecordOrdinal ordinal) {
      assert((ordinal & deferredBit) == 0);
      winnerAndDeferred = ordinal | (winnerAndDeferred & deferredBit);
    }
    bool isDeferred() const { return winnerAndDeferred & deferredBit; }
    void setDeferred() { winnerAndDeferred |= deferredBit; }
  };
  static_assert(sizeof(SlotMeta) == 16,
                "the fold's per-slot header is paid per name");
  static_assert(sizeof(SlotMeta) % alignof(SymbolUnion) == 0,
                "SlotMeta must not disturb SymbolUnion's alignment");

  static SlotMeta &metaOf(Symbol *sym) {
    return *(reinterpret_cast<SlotMeta *>(sym) - 1);
  }

  static constexpr unsigned numShards = 64;
  // The shards are lock domains only. Slots are not allocated from them: a slot
  // drawn from a per-shard arena would put consecutive symbols in 64 different
  // places, and every later phase walks symVector or chases Symbol* out of a
  // relocation, so it would pay a cache miss per symbol.
  struct Shard {
    llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> map;
    std::vector<Symbol *> created;
    std::mutex mu;
  };
  static unsigned shardOf(llvm::CachedHashStringRef name) {
    return (name.hash() >> 25) & (numShards - 1);
  }
  Symbol *allocateSlot();
  // The fold's per-slot header is only paid for slots that exist while the fold
  // runs; everything the serial phases create afterwards keeps the plain layout.
  bool headersActive = false;

  Symbol *foldDefined(const DeferredDefinedRecord &rec);
  Symbol *foldUndefined(StringRef name, InputFile *, bool isWeakRef);
  Symbol *foldCommon(StringRef name, InputFile *, uint64_t size, uint32_t align,
                     bool isPrivateExtern);

  std::pair<Symbol *, bool> insert(StringRef name, const InputFile *);
  llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> symMap;
  std::vector<Symbol *> symVector;
  std::unique_ptr<Shard[]> shards;
  // The sharded map replaces symMap for the rest of the link once it is built.
  bool sharded = false;
  // True only while workers may be folding records.
  bool folding = false;
};

void reportPendingUndefinedSymbols();
void reportPendingDuplicateSymbols();

// Call reportPendingUndefinedSymbols() to emit diagnostics.
void treatUndefinedSymbol(const Undefined &, StringRef source);
void treatUndefinedSymbol(const Undefined &, const InputSection *,
                          uint64_t offset);

extern std::unique_ptr<SymbolTable> symtab;

} // namespace lld::macho

#endif
