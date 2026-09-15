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
struct PreparedTapiSymbols;

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
  // For callers that already computed the name hash, e.g. off the ordered walk.
  Symbol *addDylib(llvm::CachedHashStringRef name, DylibFile *file,
                   bool isWeakDef, bool isTlv);
  Symbol *addDynamicLookup(StringRef name);

  Symbol *addLazyArchive(StringRef name, ArchiveFile *file,
                         const llvm::object::Archive::Symbol &sym);
  // addLazyArchive() for an index the parallel input phase held back. It differs
  // from addLazyArchive() only in that a dylib definition registered after the
  // archive does not keep the slot, because in the serial order this archive's
  // lazy symbol was already there.
  void addResidualLazyArchive(StringRef name, ArchiveFile *file,
                              const llvm::object::Archive::Symbol &sym);
  Symbol *addLazyObject(StringRef name, InputFile &file);

  Defined *addSynthetic(StringRef name, InputSection *, uint64_t value,
                        bool isPrivateExtern, bool includeInSymtab,
                        bool referencedDynamically);

  ArrayRef<Symbol *> getSymbols() const { return symVector; }

  // The shard a name is routed to. Public so that a worker preparing a TAPI
  // file can partition its names before the ordered walk reaches it.
  static constexpr unsigned numDylibShards = 64;
  static unsigned shardOfName(llvm::CachedHashStringRef name) {
    return (name.hash() >> 25) & (numDylibShards - 1);
  }

  // --- Batched dylib publication -------------------------------------------
  //
  // A prepared TAPI file's names are final names with hashes in the ordered
  // constructor's exact order, and addDylib()'s outcome for a name depends only
  // on that name's slot and on the sequence of records for that name. So an
  // autolink run's name loops can be collected as ranked records and replayed
  // one thread per shard, without locks, at the run's boundary. The ordered
  // thread keeps everything else: diagnostics, -t, inputFiles order, the
  // re-export recursion and the synthetic-dylib bookkeeping.
  //
  // deferDylibNames() takes one prepared file's whole name loop;
  // deferDylibSymbol() takes a single $ld$previous name, which inserts at the
  // constructor's position and so needs its own record. A record's rank is its
  // position in the batch, which is that serial position.
  void deferDylibNames(DylibFile *file, DylibFile *exportingFile,
                       PreparedTapiSymbols *prepared, bool anyHidden);
  void deferDylibSymbol(llvm::CachedHashStringRef name, DylibFile *owner);
  // Replays the batch and appends the slots it created to symVector in rank
  // order. Must run before anything else reads or writes the table.
  void publishDylibBatch();
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
  // serial linker would have created the slots. It leaves the shards in place:
  // they are the name map for the rest of the link.
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

  // The serial linker created a slot for every archive index entry, so a symbol
  // an extracted member defines sits in symVector where the archive's index was
  // published, not where the member was parsed. The walker reserves those
  // positions; the member's own records then fill them in.
  void reserveArchiveSlot(StringRef name, InputFile *archive,
                          RecordOrdinal ordinal);

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
    // with, in its top bit, a flag saying that one record for this name has been
    // deferred and so every later one must be too, so that the replay sees a
    // single ordered stream. The three ordinal fields share 63 bits, so that bit
    // is free.
    RecordOrdinal winnerAndDeferred;

    static constexpr uint64_t deferredBit = 1ull << 63;
    RecordOrdinal winner() const {
      RecordOrdinal o = winnerAndDeferred;
      o.v &= ~deferredBit;
      return o;
    }
    void setWinner(RecordOrdinal ordinal) {
      assert((ordinal.v & deferredBit) == 0);
      ordinal.v |= winnerAndDeferred.v & deferredBit;
      winnerAndDeferred = ordinal;
    }
    bool isDeferred() const { return winnerAndDeferred.v & deferredBit; }
    void setDeferred() { winnerAndDeferred.v |= deferredBit; }
  };
  static_assert(sizeof(SlotMeta) == 16,
                "the fold's per-slot header is paid per name");
  static_assert(sizeof(SlotMeta) % alignof(SymbolUnion) == 0,
                "SlotMeta must not disturb SymbolUnion's alignment");

  static SlotMeta &metaOf(Symbol *sym) {
    return *(reinterpret_cast<SlotMeta *>(sym) - 1);
  }

  static constexpr unsigned numShards = numDylibShards;
  // The shards are lock domains only. Slots are not allocated from them: a slot
  // drawn from a per-shard arena would put consecutive symbols in 64 different
  // places, and every later phase walks symVector or chases Symbol* out of a
  // relocation, so it would pay a cache miss per symbol.
  struct Shard {
    llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> map;
    std::vector<Symbol *> created;
    std::mutex mu;
    // Slots created while a dylib batch is published. One thread owns a shard
    // for the duration, so the arena needs no lock, and a shard's slots stay
    // contiguous in the order that shard created them.
    llvm::BumpPtrAllocator arena;
  };
  static unsigned shardOf(llvm::CachedHashStringRef name) {
    return shardOfName(name);
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
  std::pair<Symbol *, bool> insert(llvm::CachedHashStringRef name,
                                   const InputFile *);
  // Used only when the parallel input phase never ran: once enableSharding() has
  // been called the shards serve every lookup and insert for the rest of the
  // link, and this map stays empty.
  llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> symMap;
  std::vector<Symbol *> symVector;
  std::unique_ptr<Shard[]> shards;
  // The sharded map replaces symMap for the rest of the link once it is built,
  // and keeps it for good: endParallelInput() ends the fold, not the sharding.
  bool sharded = false;
  // True only while workers may be folding records.
  bool folding = false;

  // One deferred dylib publication: either a prepared file's whole name loop,
  // or a single $ld$previous name added to a synthetic dylib.
  struct DylibBatchRecord {
    // The file whose symbols[] the created slots are written back into; null
    // for a $ld$previous record.
    DylibFile *file = nullptr;
    // The dylib the names are published for (the umbrella, or the synthetic
    // dylib of a $ld$previous record).
    DylibFile *exportingFile = nullptr;
    PreparedTapiSymbols *prepared = nullptr;
    llvm::CachedHashStringRef name{llvm::StringRef()};
    Symbol *result = nullptr;
    // Publication sequence of this record's first name. base + index increases
    // exactly as the ordered constructor's insert order did.
    uint32_t base = 0;
    bool anyHidden = false;
  };
  std::vector<DylibBatchRecord> dylibBatch;
  // True only while publishDylibBatch()'s per-shard walk runs.
  bool publishing = false;
  Symbol *addDylibImpl(llvm::CachedHashStringRef name, DylibFile *file,
                       bool isWeakDef, bool isTlv, bool *wasInsertedOut);
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
