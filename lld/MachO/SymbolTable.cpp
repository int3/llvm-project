//===- SymbolTable.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "ConcatOutputSection.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/Parallel.h"

using namespace llvm;
using namespace lld;
using namespace lld::macho;

namespace {
template <typename T, typename... Args>
T *constructOrReplaceSymbol(Symbol *sym, bool wasInserted, Args &&...args) {
  if (wasInserted)
    return new (sym) T(std::forward<Args>(args)...);
  return replaceSymbol<T>(sym, std::forward<Args>(args)...);
}
} // namespace

Symbol *SymbolTable::find(CachedHashStringRef cachedName) {
  if (sharded) {
    Shard &shard = shards[shardOf(cachedName)];
    auto it = shard.map.find(cachedName);
    return it == shard.map.end() ? nullptr : it->second;
  }
  auto it = symMap.find(cachedName);
  if (it == symMap.end())
    return nullptr;
  return it->second;
}

void SymbolTable::enableSharding() {
  assert(!sharded && symMap.empty() &&
         "sharding must be chosen before any symbol exists");
  shards = std::make_unique<Shard[]>(numShards);
  sharded = true;
  headersActive = true;
}

Symbol *SymbolTable::allocateSlot() {
  if (!headersActive)
    return reinterpret_cast<Symbol *>(
        getSpecificAllocSingleton<SymbolUnion>().Allocate());
  // While the fold runs, each worker draws from its own arena, so the names one
  // file creates stay together in the order that file created them.
  thread_local llvm::BumpPtrAllocator foldAlloc;
  void *storage = foldAlloc.Allocate(sizeof(SlotMeta) + sizeof(SymbolUnion),
                                     alignof(SymbolUnion));
  auto *meta = new (storage) SlotMeta();
  return reinterpret_cast<Symbol *>(meta + 1);
}

void SymbolTable::beginParallelInput() {
  assert(sharded);
  folding = true;
}

void SymbolTable::endParallelInput() {
  assert(folding);
  folding = false;
  headersActive = false;
  serialCtx = nullptr;

  size_t count = 0;
  for (unsigned i = 0; i != numShards; ++i)
    count += shards[i].created.size();
  // The slot's lowest record ordinal is the position at which the serial linker
  // would have inserted it, so sorting by it reproduces symVector's order --
  // which Writer::scanSymbols, SymtabSection, markLive's roots and the
  // undefined-symbol diagnostics all observe. The key is copied next to the
  // pointer so that the sort does not chase a header per comparison, and no two
  // slots can share a lowest ordinal, so the order is total and the parallel
  // sort is deterministic.
  std::vector<std::pair<RecordOrdinal, Symbol *>> created;
  created.reserve(count);
  for (unsigned i = 0; i != numShards; ++i) {
    for (Symbol *sym : shards[i].created)
      created.emplace_back(metaOf(sym).firstOrdinal, sym);
    shards[i].created.clear();
    shards[i].created.shrink_to_fit();
  }
  parallelSort(created.begin(), created.end(),
               [](const std::pair<RecordOrdinal, Symbol *> &a,
                  const std::pair<RecordOrdinal, Symbol *> &b) {
                 return a.first < b.first;
               });
  symVector.reserve(symVector.size() + created.size());
  for (const auto &[ordinal, sym] : created)
    symVector.push_back(sym);

  // The shards stay. Nothing reads the name map's iteration order -- symVector is
  // the iteration structure, and symMap has no reader other than find() and
  // insert(), both of which route by hash while sharded -- so merging the 64
  // shard maps back into one only to serve the rest of the link's lookups is a
  // pass over every name for nothing. On an archive-heavy link it was the serial
  // half of "Compact symbol vector" (109 ms of Chromium's 148 before the -ObjC
  // walker, more after it).
}

std::pair<Symbol *, bool> SymbolTable::insert(StringRef name,
                                              const InputFile *file) {
  return insert(CachedHashStringRef(name), file);
}

std::pair<Symbol *, bool> SymbolTable::insert(CachedHashStringRef cachedName,
                                              const InputFile *file) {
  if (sharded) {
    Shard &shard = shards[shardOf(cachedName)];
    auto [it, wasInserted] = shard.map.try_emplace(cachedName, nullptr);
    if (wasInserted) {
      Symbol *sym = allocateSlot();
      it->second = sym;
      if (folding) {
        RecordOrdinal ord =
            serialCtx ? serialCtx->nextOrdinal() : RecordOrdinal();
        metaOf(sym).firstOrdinal = ord;
        metaOf(sym).setWinner(ord);
        shard.created.push_back(sym);
      } else {
        symVector.push_back(sym);
      }
      return {sym, true};
    }
    Symbol *sym = it->second;
    if (folding && serialCtx) {
      RecordOrdinal ord = serialCtx->nextOrdinal();
      SlotMeta &meta = metaOf(sym);
      if (ord < meta.firstOrdinal)
        meta.firstOrdinal = ord;
    }
    sym->isUsedInRegularObj |= !file || isa<ObjFile>(file);
    return {sym, false};
  }

  auto [it, wasInserted] = symMap.try_emplace(cachedName, nullptr);

  if (wasInserted) {
    // Name is a new symbol.
    it->second = reinterpret_cast<Symbol *>(
        getSpecificAllocSingleton<SymbolUnion>().Allocate());
    symVector.push_back(it->second);
  }
  Symbol *sym = it->second;

  if (!wasInserted)
    sym->isUsedInRegularObj |= !file || isa<ObjFile>(file);
  return {sym, wasInserted};
}

namespace {
struct DuplicateSymbolDiag {
  // Pair containing source location and source file
  const std::pair<std::string, std::string> src1;
  const std::pair<std::string, std::string> src2;
  const Symbol *sym;

  DuplicateSymbolDiag(const std::pair<std::string, std::string> src1,
                      const std::pair<std::string, std::string> src2,
                      const Symbol *sym)
      : src1(src1), src2(src2), sym(sym) {}
};
SmallVector<DuplicateSymbolDiag> dupSymDiags;
} // namespace

// Move local symbols at \p fromOff in \p fromIsec into \p toIsec, unless that
// symbol is \p skip, in which case we just remove it.
static void transplantSymbolsAtOffset(InputSection *fromIsec,
                                      InputSection *toIsec, Defined *skip,
                                      uint64_t fromOff, uint64_t toOff) {
  // Ensure the symbols will still be in address order after our insertions.
  auto symSucceedsOff = [](uint64_t off, const Symbol *s) {
    return cast<Defined>(s)->value > off;
  };
  assert(std::is_partitioned(toIsec->symbols.begin(), toIsec->symbols.end(),
                             [symSucceedsOff, toOff](const Symbol *s) {
                               return !symSucceedsOff(toOff, s);
                             }) &&
         "Symbols in toIsec must be partitioned by toOff.");
  // A transplanted local can be the method-list symbol, so the candidate bit
  // has to follow the symbols. It is conservative, so an over-approximation
  // here is harmless.
  toIsec->mayBeMethodList |= fromIsec->mayBeMethodList;
  auto insertIt = llvm::upper_bound(toIsec->symbols, toOff, symSucceedsOff);
  llvm::erase_if(fromIsec->symbols, [&](Symbol *s) {
    auto *d = cast<Defined>(s);
    if (d == skip)
      return true;
    if (d->value != fromOff || d->isExternal())
      return false;

    // This repeated insertion will be quadratic unless insertIt is the end
    // iterator. However, that is typically the case for files that have
    // .subsections_via_symbols set.
    insertIt = toIsec->symbols.insert(insertIt, d);
    d->originalIsec = toIsec;
    d->value = toOff;
    // We don't want to have more than one unwindEntry at a given address, so
    // drop the redundant ones. We can safely drop the unwindEntries of the
    // symbols in fromIsec since we will be adding another unwindEntry as we
    // finish parsing toIsec's file. (We can assume that toIsec has its own
    // unwindEntry because of the ODR.)
    d->setUnwindEntry(nullptr);
    return true;
  });
}


// Is there a non-external definition at offset \p off in \p isec that
// transplantSymbolsAtOffset() would move into the prevailing section?
static bool hasTransplantableLocals(const ConcatInputSection *isec,
                                    uint64_t off) {
  auto it = llvm::lower_bound(
      isec->symbols, off, [](Defined *d, uint64_t off) { return d->value < off; });
  for (auto end = isec->symbols.end(); it != end && (*it)->value == off; ++it)
    if (!(*it)->isExternal())
      return true;
  return false;
}

// Reconstruct the record that produced \p d, so that a definition the fold
// applied out of order can be rewound and replayed serially.
static DeferredDefinedRecord recordFromDefined(RecordOrdinal ordinal,
                                               Defined *d) {
  return DeferredDefinedRecord{ordinal,
                               d->getName(),
                               d->getFile(),
                               d->originalIsec,
                               d->value,
                               d->size,
                               d->isWeakDef(),
                               d->privateExtern,
                               d->referencedDynamically,
                               d->noDeadStrip,
                               d->weakDefCanBeHidden,
                               d->cold};
}

// Apply one definition to its slot under the slot's shard lock. The winner of a
// name is a function of the record set and the records' ordinals, not of the
// order in which the workers happen to arrive, so only the winner is ever
// constructed and every loser does only work that commutes. The transitions that
// do not commute -- duplicate strong definitions, a loser whose section has
// local aliases to move, and a weak definition displaced by a later strong one --
// are recorded for the serial passes instead.
Symbol *SymbolTable::foldDefined(const DeferredDefinedRecord &rec) {
  ParseContext &ctx = *rec.file->parseCtx;
  const RecordOrdinal ord = rec.ordinal;
  CachedHashStringRef cachedName(rec.name);
  Shard &shard = shards[shardOf(cachedName)];

  // With -flat_namespace, all extern symbols in dylibs are interposable.
  const bool interposable = ((config->namespaceKind == NamespaceKind::flat &&
                              config->outputType != MachO::MH_EXECUTE) ||
                             config->interposable) &&
                            !rec.isPrivateExtern;

  std::lock_guard<std::mutex> lock(shard.mu);
  auto [it, wasInserted] = shard.map.try_emplace(cachedName, nullptr);

  auto build = [&](Symbol *slot, bool fresh, bool overridesWeakDef) -> Defined * {
    if (fresh)
      return new (slot)
          Defined(rec.name, rec.file, rec.isec, rec.value, rec.size,
                  rec.isWeakDef, /*isExternal=*/true, rec.isPrivateExtern,
                  /*includeInSymtab=*/true, rec.isReferencedDynamically,
                  rec.noDeadStrip, overridesWeakDef, rec.isWeakDefCanBeHidden,
                  interposable, rec.isCold);
    return replaceSymbol<Defined>(
        slot, rec.name, rec.file, rec.isec, rec.value, rec.size, rec.isWeakDef,
        /*isExternal=*/true, rec.isPrivateExtern, /*includeInSymtab=*/true,
        rec.isReferencedDynamically, rec.noDeadStrip, overridesWeakDef,
        rec.isWeakDefCanBeHidden, interposable, rec.isCold);
  };

  if (wasInserted) {
    Symbol *slot = allocateSlot();
    it->second = slot;
    shard.created.push_back(slot);
    SlotMeta &meta = metaOf(slot);
    meta.firstOrdinal = ord;
    meta.setWinner(ord);
    build(slot, /*fresh=*/true, /*overridesWeakDef=*/false);
    return slot;
  }

  Symbol *slot = it->second;
  SlotMeta &meta = metaOf(slot);
  if (ord < meta.firstOrdinal)
    meta.firstOrdinal = ord;
  slot->isUsedInRegularObj |= isa<ObjFile>(rec.file);

  if (meta.isDeferred()) {
    // The slot holds the lowest-ordinal record of this name and every other one
    // is on the replay list, so that the replay sees exactly the serial stream
    // whatever order the workers arrive in.
    auto *defined = dyn_cast<Defined>(slot);
    if (defined && ord < meta.winner()) {
      // This record becomes the slot's prevailing definition, so if it is weak
      // its file owns the name at its own completion point and has to register
      // its unwind entries there. Flagging a file that turns out not to need it
      // is harmless.
      if (rec.isWeakDef)
        ctx.inlineUnwindFiles.push_back(rec.file);
      DeferredDefinedRecord later = recordFromDefined(meta.winner(), defined);
      if (auto *oldIsec =
              dyn_cast_or_null<ConcatInputSection>(defined->originalIsec))
        ctx.coalesces.push_back({meta.winner(), oldIsec, slot, defined->value,
                                 /*eraseSlot=*/true, /*coalesce=*/false});
      replaceSymbol<Defined>(
          slot, rec.name, rec.file, rec.isec, rec.value, rec.size,
          rec.isWeakDef, /*isExternal=*/true, rec.isPrivateExtern,
          /*includeInSymtab=*/true, rec.isReferencedDynamically,
          rec.noDeadStrip, /*canOverrideWeakDef=*/false,
          rec.isWeakDefCanBeHidden,
          ((config->namespaceKind == NamespaceKind::flat &&
            config->outputType != MachO::MH_EXECUTE) ||
           config->interposable) &&
              !rec.isPrivateExtern,
          rec.isCold);
      meta.setWinner(ord);
      ctx.deferredDefined.push_back(later);
    } else {
      ctx.deferredDefined.push_back(rec);
    }
    return slot;
  }

  if (auto *defined = dyn_cast<Defined>(slot)) {
    const bool existingWeak = defined->isWeakDef();
    const RecordOrdinal existingOrd = meta.winner();
    // The record with the lower ordinal is the one the serial linker applied
    // first, whichever of the two reached the shard first.
    const bool incomingFirst = ord < existingOrd;
    auto *existingIsec =
        dyn_cast_or_null<ConcatInputSection>(defined->originalIsec);
    const uint64_t existingValue = defined->value;

    if (rec.isWeakDef && existingWeak) {
      if (!incomingFirst) {
        // Ordinary weak coalescing. The metadata merge is commutative.
        defined->privateExtern &= rec.isPrivateExtern;
        defined->weakDefCanBeHidden &= rec.isWeakDefCanBeHidden;
        defined->referencedDynamically |= rec.isReferencedDynamically;
        defined->noDeadStrip |= rec.noDeadStrip;
        defined->cold |= rec.isCold;
        if (auto *ci = dyn_cast_or_null<ConcatInputSection>(rec.isec)) {
          ci->wasCoalesced = true;
          if (defined->originalIsec && hasTransplantableLocals(ci, rec.value))
            ctx.coalesces.push_back({ord, ci, slot, rec.value,
                                     /*eraseSlot=*/false, /*coalesce=*/true});
        }
        return slot;
      }
      // We have the lower ordinal, so we prevail and the definition already in
      // the slot becomes the loser.
      const bool oldPrivateExtern = defined->privateExtern;
      const bool oldHidden = defined->weakDefCanBeHidden;
      const bool oldRefDyn = defined->referencedDynamically;
      const bool oldNoDeadStrip = defined->noDeadStrip;
      const bool oldCold = defined->cold;
      Defined *winner = build(slot, /*fresh=*/false, /*overridesWeakDef=*/false);
      winner->privateExtern &= oldPrivateExtern;
      winner->weakDefCanBeHidden &= oldHidden;
      winner->referencedDynamically |= oldRefDyn;
      winner->noDeadStrip |= oldNoDeadStrip;
      winner->cold |= oldCold;
      meta.setWinner(ord);
      if (existingIsec)
        ctx.coalesces.push_back({existingOrd, existingIsec, slot, existingValue,
                                 /*eraseSlot=*/true, /*coalesce=*/true});
      return slot;
    }

    if (!rec.isWeakDef && !existingWeak) {
      // Duplicate strong definitions: the diagnostics are emitted in event
      // order and the later definition prevails, so replay them serially.
      if (incomingFirst) {
        DeferredDefinedRecord later = recordFromDefined(existingOrd, defined);
        build(slot, /*fresh=*/false, /*overridesWeakDef=*/false);
        meta.setWinner(ord);
        if (existingIsec)
          ctx.coalesces.push_back({existingOrd, existingIsec, slot,
                                   existingValue, /*eraseSlot=*/true,
                                   /*coalesce=*/false});
        ctx.deferredDefined.push_back(later);
      } else {
        ctx.deferredDefined.push_back(rec);
      }
      meta.setDeferred();
      return slot;
    }

    // One definition is weak and the other strong. The strong one prevails
    // either way, but if the weak one came first then it was the prevailing
    // symbol at its own file's completion, which unwind registration observes,
    // so that transition is replayed serially.
    const bool weakFirst = rec.isWeakDef ? incomingFirst : !incomingFirst;
    if (!weakFirst) {
      if (rec.isWeakDef) {
        // The prevailing strong definition is already in the slot.
        if (auto *ci = dyn_cast_or_null<ConcatInputSection>(rec.isec)) {
          ci->wasCoalesced = true;
          if (defined->originalIsec && hasTransplantableLocals(ci, rec.value))
            ctx.coalesces.push_back({ord, ci, slot, rec.value,
                                     /*eraseSlot=*/false, /*coalesce=*/true});
        }
        return slot;
      }
      // Our strong definition has the lower ordinal and displaces the weak one.
      build(slot, /*fresh=*/false, /*overridesWeakDef=*/false);
      meta.setWinner(ord);
      if (existingIsec)
        ctx.coalesces.push_back({existingOrd, existingIsec, slot, existingValue,
                                 /*eraseSlot=*/true, /*coalesce=*/true});
      return slot;
    }
    meta.setDeferred();
    // The weak definition prevails at the end of its own file, which is where
    // that file registers its unwind entries.
    if (rec.isWeakDef) {
      ctx.inlineUnwindFiles.push_back(rec.file);
      // The strong definition arrived first but belongs later; rewind it.
      DeferredDefinedRecord strong = recordFromDefined(existingOrd, defined);
      build(slot, /*fresh=*/false, /*overridesWeakDef=*/false);
      meta.setWinner(ord);
      if (existingIsec)
        ctx.coalesces.push_back({existingOrd, existingIsec, slot, existingValue,
                                 /*eraseSlot=*/true, /*coalesce=*/false});
      ctx.deferredDefined.push_back(strong);
    } else {
      if (defined->getFile())
        ctx.inlineUnwindFiles.push_back(defined->getFile());
      ctx.deferredDefined.push_back(rec);
    }
    return slot;
  }

  // A definition outranks everything else that can be in the slot at this
  // point. Lazy symbols cannot be: archives are not folded.
  switch (slot->kind()) {
  case Symbol::CommonKind:
  case Symbol::UndefinedKind:
    build(slot, /*fresh=*/false, /*overridesWeakDef=*/false);
    meta.setWinner(ord);
    return slot;
  case Symbol::DylibKind: {
    auto *dysym = cast<DylibSymbol>(slot);
    // The condition is symmetric, so it does not matter whether the dylib
    // record or this definition was applied first.
    const bool overridesWeakDef =
        !rec.isWeakDef && dysym->isWeakDef() &&
        !(weakOverrideSuppressedHook && weakOverrideSuppressedHook(dysym));
    dysym->unreference();
    build(slot, /*fresh=*/false, overridesWeakDef);
    meta.setWinner(ord);
    return slot;
  }
  default:
    llvm_unreachable("unexpected symbol kind during the parallel input fold");
  }
}

Symbol *SymbolTable::foldUndefined(StringRef name, InputFile *file,
                                   bool isWeakRef) {
  ParseContext &ctx = *file->parseCtx;
  const RecordOrdinal ord = ctx.nextUndefinedOrdinal();
  CachedHashStringRef cachedName(name);
  Shard &shard = shards[shardOf(cachedName)];
  const RefState refState = isWeakRef ? RefState::Weak : RefState::Strong;

  std::lock_guard<std::mutex> lock(shard.mu);
  auto [it, wasInserted] = shard.map.try_emplace(cachedName, nullptr);
  if (wasInserted) {
    Symbol *slot = allocateSlot();
    it->second = slot;
    shard.created.push_back(slot);
    SlotMeta &meta = metaOf(slot);
    meta.firstOrdinal = ord;
    meta.setWinner(ord);
    new (slot) Undefined(name, file, refState, /*wasBitcodeSymbol=*/false);
    return slot;
  }

  Symbol *slot = it->second;
  SlotMeta &meta = metaOf(slot);
  if (ord < meta.firstOrdinal)
    meta.firstOrdinal = ord;
  slot->isUsedInRegularObj |= isa<ObjFile>(file);

  if (auto *dysym = dyn_cast<DylibSymbol>(slot)) {
    // reference()/unreference() net out to the same count whichever order the
    // records arrive in.
    dysym->reference(refState);
  } else if (auto *undefined = dyn_cast<Undefined>(slot)) {
    const RefState merged = std::max(undefined->refState, refState);
    if (ord < meta.winner()) {
      // The serial linker attributes an undefined symbol to the first file that
      // referenced it, which the spelling-correction diagnostic reads.
      replaceSymbol<Undefined>(slot, name, file, merged,
                               /*wasBitcodeSymbol=*/false);
      meta.setWinner(ord);
    } else {
      undefined->refState = merged;
    }
  } else {
    assert(!slot->isLazy() && "lazy symbols are excluded from the fold");
  }
  return slot;
}

Symbol *SymbolTable::foldCommon(StringRef name, InputFile *file, uint64_t size,
                                uint32_t align, bool isPrivateExtern) {
  ParseContext &ctx = *file->parseCtx;
  const RecordOrdinal ord = ctx.nextOrdinal();
  CachedHashStringRef cachedName(name);
  Shard &shard = shards[shardOf(cachedName)];

  std::lock_guard<std::mutex> lock(shard.mu);
  auto [it, wasInserted] = shard.map.try_emplace(cachedName, nullptr);
  if (wasInserted) {
    Symbol *slot = allocateSlot();
    it->second = slot;
    shard.created.push_back(slot);
    SlotMeta &meta = metaOf(slot);
    meta.firstOrdinal = ord;
    meta.setWinner(ord);
    new (slot) CommonSymbol(name, file, size, align, isPrivateExtern);
    return slot;
  }

  Symbol *slot = it->second;
  SlotMeta &meta = metaOf(slot);
  if (ord < meta.firstOrdinal)
    meta.firstOrdinal = ord;
  slot->isUsedInRegularObj |= isa<ObjFile>(file);

  if (auto *common = dyn_cast<CommonSymbol>(slot)) {
    // The larger common wins; between equal sizes the later one wins, taking its
    // alignment and visibility with it.
    if (size < common->size ||
        (size == common->size && ord < meta.winner()))
      return slot;
  } else if (isa<Defined>(slot)) {
    return slot;
  }
  replaceSymbol<CommonSymbol>(slot, name, file, size, align, isPrivateExtern);
  meta.setWinner(ord);
  return slot;
}

void SymbolTable::applyPendingCoalesces(
    MutableArrayRef<PendingCoalesce> pending) {
  llvm::sort(pending, [](const PendingCoalesce &a, const PendingCoalesce &b) {
    return a.ordinal < b.ordinal;
  });
  for (const PendingCoalesce &p : pending) {
    if (p.eraseSlot)
      llvm::erase_if(p.isec->symbols, [&](Defined *d) {
        return static_cast<Symbol *>(d) == p.slot;
      });
    if (!p.coalesce)
      continue;
    p.isec->wasCoalesced = true;
    auto *winner = dyn_cast<Defined>(p.slot);
    if (!winner || !winner->originalIsec)
      continue;
    transplantSymbolsAtOffset(p.isec, winner->isec(), /*skip=*/nullptr, p.value,
                              winner->value);
  }
}

void SymbolTable::reserveArchiveSlot(StringRef name, InputFile *archive,
                                     RecordOrdinal ordinal) {
  assert(folding);
  CachedHashStringRef cachedName(name);
  Shard &shard = shards[shardOf(cachedName)];
  auto [it, wasInserted] = shard.map.try_emplace(cachedName, nullptr);
  if (!wasInserted) {
    SlotMeta &meta = metaOf(it->second);
    if (ordinal < meta.firstOrdinal)
      meta.firstOrdinal = ordinal;
    return;
  }
  Symbol *slot = allocateSlot();
  it->second = slot;
  shard.created.push_back(slot);
  SlotMeta &meta = metaOf(slot);
  meta.firstOrdinal = ordinal;
  meta.setWinner(ordinal);
  // A placeholder: the member that provides this name always defines it, so
  // this is replaced before anything reads it.
  new (slot) Undefined(name, archive, RefState::Strong,
                       /*wasBitcodeSymbol=*/false);
}

Symbol *SymbolTable::addDefinedRecord(const DeferredDefinedRecord &record) {
  if (folding && record.file && record.file->parseCtx) {
    DeferredDefinedRecord rec = record;
    rec.ordinal = record.file->parseCtx->nextOrdinal();
    return foldDefined(rec);
  }
  return addDefined(record.name, record.file, record.isec, record.value,
                    record.size, record.isWeakDef, record.isPrivateExtern,
                    record.isReferencedDynamically, record.noDeadStrip,
                    record.isWeakDefCanBeHidden, record.isCold);
}

Defined *SymbolTable::addDefined(StringRef name, InputFile *file,
                                 InputSection *isec, uint64_t value,
                                 uint64_t size, bool isWeakDef,
                                 bool isPrivateExtern,
                                 bool isReferencedDynamically, bool noDeadStrip,
                                 bool isWeakDefCanBeHidden, bool isCold) {
  assert((!folding || !file || !file->parseCtx) &&
         "definitions from a worker must go through addDefinedRecord()");
  bool overridesWeakDef = false;
  auto [s, wasInserted] = insert(name, file);

  assert(!file || !isa<BitcodeFile>(file) || !isec);

  if (!wasInserted) {
    if (auto *defined = dyn_cast<Defined>(s)) {
      if (isWeakDef) {
        // See further comment in createDefined() in InputFiles.cpp
        if (defined->isWeakDef()) {
          defined->privateExtern &= isPrivateExtern;
          defined->weakDefCanBeHidden &= isWeakDefCanBeHidden;
          defined->referencedDynamically |= isReferencedDynamically;
          defined->noDeadStrip |= noDeadStrip;
          // If either weak definition is cold, the merged symbol is cold.
          // This matches the behavior of both ld-prime and ld64.
          defined->cold |= isCold;
        }
        if (auto concatIsec = dyn_cast_or_null<ConcatInputSection>(isec)) {
          concatIsec->wasCoalesced = true;
          // Any local symbols that alias the coalesced symbol should be moved
          // into the prevailing section. Note that we have sorted the symbols
          // in ObjFile::parseSymbols() such that extern weak symbols appear
          // last, so we don't need to worry about subsequent symbols being
          // added to an already-coalesced section.
          if (defined->isec())
            transplantSymbolsAtOffset(concatIsec, defined->isec(),
                                      /*skip=*/nullptr, value, defined->value);
        }
        return defined;
      }

      if (defined->isWeakDef()) {
        if (auto concatIsec =
                dyn_cast_or_null<ConcatInputSection>(defined->isec())) {
          concatIsec->wasCoalesced = true;
          if (isec)
            transplantSymbolsAtOffset(concatIsec, isec, defined, defined->value,
                                      value);
        }
      } else {
        std::string srcLoc1 = defined->getSourceLocation();
        std::string srcLoc2 = isec ? isec->getSourceLocation(value) : "";
        std::string srcFile1 = toString(defined->getFile());
        std::string srcFile2 = toString(file);

        dupSymDiags.push_back({make_pair(srcLoc1, srcFile1),
                               make_pair(srcLoc2, srcFile2), defined});
      }

    } else if (auto *dysym = dyn_cast<DylibSymbol>(s)) {
      overridesWeakDef =
          !isWeakDef && dysym->isWeakDef() &&
          !(weakOverrideSuppressedHook && weakOverrideSuppressedHook(dysym));
      dysym->unreference();
    } else if (auto *undef = dyn_cast<Undefined>(s)) {
      if (undef->wasBitcodeSymbol) {
        auto objFile = dyn_cast<ObjFile>(file);
        if (!objFile) {
          // The file must be a native object file, as opposed to potentially
          // being another bitcode file. A situation arises when some symbols
          // are defined thru `module asm` and thus they are not present in the
          // bitcode's symbol table. Consider bitcode modules `A`, `B`, and `C`.
          // LTO compiles only `A` and `C`, since there's no explicit symbol
          // reference to `B` other than a symbol from `A` via `module asm`.
          // After LTO is finished, the missing symbol now appears in the
          // resulting object file for `A`, which  prematurely resolves another
          // prevailing symbol with `B` that hasn't been compiled, instead of
          // the resulting object for `C`. Consequently, an incorrect
          // relocation is generated for the prevailing symbol.
          assert(isa<BitcodeFile>(file) && "Bitcode file is expected.");
          std::string message =
              "The pending prevailing symbol(" + name.str() +
              ") in the bitcode file(" + toString(undef->getFile()) +
              ") is overridden by a non-native object (from bitcode): " +
              toString(file);
          error(message);
        } else if (!objFile->builtFromBitcode) {
          // Ideally, this should be an object file compiled from a bitcode
          // file. However, this might not hold true if a LC linker option is
          // used. In case LTO internalizes a prevailing hidden weak symbol,
          // there's a situation where an unresolved prevailing symbol might be
          // linked with the corresponding one from a native library, which is
          // loaded later after LTO. Although this could potentially result in
          // an ODR violation, we choose to permit this scenario as a warning.
          std::string message = "The pending prevailing symbol(" + name.str() +
                                ") in the bitcode file(" +
                                toString(undef->getFile()) +
                                ") is overridden by a post-processed native "
                                "object (from native archive): " +
                                toString(file);
          warn(message);
        } else {
          // Preserve the original bitcode file name (instead of using the
          // object file name).
          file = undef->getFile();
        }
      }
    }
    // Defined symbols take priority over other types of symbols, so in case
    // of a name conflict, we fall through to the replaceSymbol() call below.
  }

  // With -flat_namespace, all extern symbols in dylibs are interposable.
  bool interposable = ((config->namespaceKind == NamespaceKind::flat &&
                        config->outputType != MachO::MH_EXECUTE) ||
                       config->interposable) &&
                      !isPrivateExtern;
  Defined *defined = constructOrReplaceSymbol<Defined>(
      s, wasInserted, name, file, isec, value, size, isWeakDef,
      /*isExternal=*/true, isPrivateExtern, /*includeInSymtab=*/true,
      isReferencedDynamically, noDeadStrip, overridesWeakDef,
      isWeakDefCanBeHidden, interposable, isCold);
  return defined;
}

Defined *SymbolTable::aliasDefined(Defined *src, StringRef target,
                                   InputFile *newFile, bool makePrivateExtern) {
  bool isPrivateExtern = makePrivateExtern || src->privateExtern;
  return addDefined(target, newFile, src->isec(), src->value, src->size,
                    src->isWeakDef(), isPrivateExtern,
                    src->referencedDynamically, src->noDeadStrip,
                    src->weakDefCanBeHidden, src->cold);
}

Symbol *SymbolTable::addUndefined(StringRef name, InputFile *file,
                                  bool isWeakRef) {
  if (folding && file && file->parseCtx)
    return foldUndefined(name, file, isWeakRef);
  auto [s, wasInserted] = insert(name, file);

  RefState refState = isWeakRef ? RefState::Weak : RefState::Strong;

  if (wasInserted) {
    constructOrReplaceSymbol<Undefined>(s, /*wasInserted=*/true, name, file,
                                        refState, /*wasBitcodeSymbol=*/false);
    // The walker leaves no LazyArchive symbol behind for an index entry it did
    // not extract, so a name referenced after the input phase asks the archives
    // directly. The hook may extract a member, which replaces s.
    if (archiveProviderHook)
      archiveProviderHook(name, UINT32_MAX);
  }
  else if (auto *lazy = dyn_cast<LazyArchive>(s)) {
    // The slot holds a lazy symbol from an archive the walk did not own -- an
    // LC_LINKER_OPTION library published after the phase. If a walked archive
    // earlier on the command line also provides this name, that is the one the
    // serial linker fetched from, because its index entry created this slot
    // before the later archive ever saw it. The walk leaves no lazy symbol
    // behind, so the incumbent lazy symbol is not evidence that it did not.
    if (archiveProviderHook)
      archiveProviderHook(name, lazy->getFile()->id);
    if (auto *stillLazy = dyn_cast<LazyArchive>(s))
      stillLazy->fetchArchiveMember();
  }
  else if (isa<LazyObject>(s))
    extract(*s->getFile(), s->getName());
  else if (auto *dynsym = dyn_cast<DylibSymbol>(s)) {
    dynsym->reference(refState);
    // A LazyArchive symbol registered before this dylib would have shadowed it.
    if (archiveProviderHook)
      archiveProviderHook(name, UINT32_MAX);
  } else if (auto *undefined = dyn_cast<Undefined>(s)) {
    undefined->refState = std::max(undefined->refState, refState);
    if (archiveProviderHook)
      archiveProviderHook(name, UINT32_MAX);
  }
  return s;
}

Symbol *SymbolTable::addCommon(StringRef name, InputFile *file, uint64_t size,
                               uint32_t align, bool isPrivateExtern) {
  if (folding && file && file->parseCtx)
    return foldCommon(name, file, size, align, isPrivateExtern);
  auto [s, wasInserted] = insert(name, file);

  if (!wasInserted) {
    if (auto *common = dyn_cast<CommonSymbol>(s)) {
      if (size < common->size)
        return s;
    } else if (isa<Defined>(s)) {
      return s;
    }
    // Common symbols take priority over all non-Defined symbols, so in case of
    // a name conflict, we fall through to the replaceSymbol() call below.
  }

  constructOrReplaceSymbol<CommonSymbol>(s, wasInserted, name, file, size,
                                         align, isPrivateExtern);
  return s;
}

Symbol *SymbolTable::addDylib(StringRef name, DylibFile *file, bool isWeakDef,
                              bool isTlv) {
  return addDylib(CachedHashStringRef(name), file, isWeakDef, isTlv);
}

Symbol *SymbolTable::addDylib(CachedHashStringRef cachedName, DylibFile *file,
                              bool isWeakDef, bool isTlv) {
  StringRef name = cachedName.val();
  auto [s, wasInserted] = insert(cachedName, file);

  RefState refState = RefState::Unreferenced;
  if (!wasInserted) {
    if (auto *defined = dyn_cast<Defined>(s)) {
      if (isWeakDef && !defined->isWeakDef())
        defined->overridesWeakDef = true;
    } else if (auto *undefined = dyn_cast<Undefined>(s)) {
      refState = undefined->refState;
    } else if (auto *dysym = dyn_cast<DylibSymbol>(s)) {
      refState = dysym->getRefState();
    }
  }

  bool isDynamicLookup = file == nullptr;
  if (wasInserted || isa<Undefined>(s) ||
      (isa<DylibSymbol>(s) &&
       ((!isWeakDef && s->isWeakDef()) ||
        (!isDynamicLookup && cast<DylibSymbol>(s)->isDynamicLookup())))) {
    if (!wasInserted) {
      if (auto *dynsym = dyn_cast<DylibSymbol>(s))
        dynsym->unreference();
    }
    constructOrReplaceSymbol<DylibSymbol>(s, wasInserted, file, name,
                                           isWeakDef, refState, isTlv);
  }

  return s;
}

Symbol *SymbolTable::addDynamicLookup(StringRef name) {
  return addDylib(name, /*file=*/nullptr, /*isWeakDef=*/false, /*isTlv=*/false);
}

Symbol *SymbolTable::addLazyArchive(StringRef name, ArchiveFile *file,
                                    const object::Archive::Symbol &sym) {
  auto [s, wasInserted] = insert(name, file);

  if (wasInserted) {
    constructOrReplaceSymbol<LazyArchive>(s, /*wasInserted=*/true, file, sym);
  } else if (isa<Undefined>(s)) {
    file->fetch(sym);
  } else if (auto *dysym = dyn_cast<DylibSymbol>(s)) {
    if (dysym->isWeakDef()) {
      if (dysym->getRefState() != RefState::Unreferenced)
        file->fetch(sym);
      else
        replaceSymbol<LazyArchive>(s, file, sym);
    }
  }
  return s;
}

void SymbolTable::addResidualLazyArchive(
    StringRef name, ArchiveFile *file,
    const object::Archive::Symbol &sym) {
  auto [s, wasInserted] = insert(name, file);
  if (wasInserted) {
    constructOrReplaceSymbol<LazyArchive>(s, /*wasInserted=*/true, file, sym);
    return;
  }
  // A definition, or an earlier archive's lazy symbol, already owns the slot.
  if (isa<Defined>(s) || isa<CommonSymbol>(s) || s->isLazy())
    return;
  if (auto *dysym = dyn_cast<DylibSymbol>(s)) {
    // A weak dylib definition always yields to an archive member, whichever was
    // registered first.
    if (dysym->isWeakDef()) {
      if (dysym->getRefState() != RefState::Unreferenced)
        file->fetch(sym);
      else
        replaceSymbol<LazyArchive>(s, file, sym);
      return;
    }
    // A strong dylib registered before this archive kept the slot. One
    // registered after it never had it: the serial linker found a lazy symbol
    // here and left the dylib definition out.
    if (dysym->isDynamicLookup() || dysym->getFile()->id < file->id)
      return;
    dysym->unreference();
    replaceSymbol<LazyArchive>(s, file, sym);
    return;
  }
  if (isa<Undefined>(s)) {
    file->fetch(sym);
    return;
  }
}

Symbol *SymbolTable::addLazyObject(StringRef name, InputFile &file) {
  auto [s, wasInserted] = insert(name, &file);

  if (wasInserted) {
    constructOrReplaceSymbol<LazyObject>(s, /*wasInserted=*/true, file, name);
    s->isUsedInRegularObj |= isa<ObjFile>(&file);
  } else if (isa<Undefined>(s)) {
    extract(file, name);
  } else if (auto *dysym = dyn_cast<DylibSymbol>(s)) {
    if (dysym->isWeakDef()) {
      if (dysym->getRefState() != RefState::Unreferenced)
        extract(file, name);
      else
        replaceSymbol<LazyObject>(s, file, name);
    }
  }
  return s;
}

Defined *SymbolTable::addSynthetic(StringRef name, InputSection *isec,
                                   uint64_t value, bool isPrivateExtern,
                                   bool includeInSymtab,
                                   bool referencedDynamically) {
  assert(!isec || !isec->getFile()); // See makeSyntheticInputSection().
  Defined *s = addDefined(name, /*file=*/nullptr, isec, value, /*size=*/0,
                          /*isWeakDef=*/false, isPrivateExtern,
                          referencedDynamically, /*noDeadStrip=*/false,
                          /*isWeakDefCanBeHidden=*/false);
  s->includeInSymtab = includeInSymtab;
  return s;
}

enum class Boundary {
  Start,
  End,
};

static Defined *createBoundarySymbol(const Undefined &sym) {
  return symtab->addSynthetic(
      sym.getName(), /*isec=*/nullptr, /*value=*/-1, /*isPrivateExtern=*/true,
      /*includeInSymtab=*/false, /*referencedDynamically=*/false);
}

static void handleSectionBoundarySymbol(const Undefined &sym, StringRef segSect,
                                        Boundary which) {
  auto [segName, sectName] = segSect.split('$');

  // Attach the symbol to any InputSection that will end up in the right
  // OutputSection -- it doesn't matter which one we pick.
  // Don't bother looking through inputSections for a matching
  // ConcatInputSection -- we need to create ConcatInputSection for
  // non-existing sections anyways, and that codepath works even if we should
  // already have a ConcatInputSection with the right name.

  OutputSection *osec = nullptr;
  // This looks for __TEXT,__cstring etc.
  for (SyntheticSection *ssec : syntheticSections)
    if (ssec->segname == segName && ssec->name == sectName) {
      osec = ssec->isec->parent;
      break;
    }

  if (!osec) {
    ConcatInputSection *isec = makeSyntheticInputSection(segName, sectName);

    // This runs after markLive() and is only called for Undefineds that are
    // live. Marking the isec live ensures an OutputSection is created that the
    // start/end symbol can refer to.
    assert(sym.isLive());
    assert(isec->live);

    // This runs after gatherInputSections(), so need to explicitly set parent
    // and add to inputSections.
    osec = isec->parent = ConcatOutputSection::getOrCreateForInput(isec);
    inputSections.push_back(isec);
  }

  if (which == Boundary::Start)
    osec->sectionStartSymbols.push_back(createBoundarySymbol(sym));
  else
    osec->sectionEndSymbols.push_back(createBoundarySymbol(sym));
}

static void handleSegmentBoundarySymbol(const Undefined &sym, StringRef segName,
                                        Boundary which) {
  OutputSegment *seg = getOrCreateOutputSegment(segName);
  if (which == Boundary::Start)
    seg->segmentStartSymbols.push_back(createBoundarySymbol(sym));
  else
    seg->segmentEndSymbols.push_back(createBoundarySymbol(sym));
}

// Try to find a definition for an undefined symbol.
// Returns true if a definition was found and no diagnostics are needed.
static bool recoverFromUndefinedSymbol(const Undefined &sym) {
  // Handle start/end symbols.
  StringRef name = sym.getName();
  if (name.consume_front("section$start$")) {
    handleSectionBoundarySymbol(sym, name, Boundary::Start);
    return true;
  }
  if (name.consume_front("section$end$")) {
    handleSectionBoundarySymbol(sym, name, Boundary::End);
    return true;
  }
  if (name.consume_front("segment$start$")) {
    handleSegmentBoundarySymbol(sym, name, Boundary::Start);
    return true;
  }
  if (name.consume_front("segment$end$")) {
    handleSegmentBoundarySymbol(sym, name, Boundary::End);
    return true;
  }

  // Leave dtrace symbols, since we will handle them when we do the relocation
  if (name.starts_with("___dtrace_"))
    return true;

  // Handle -U.
  if (config->explicitDynamicLookups.contains(sym.getName())) {
    symtab->addDynamicLookup(sym.getName());
    return true;
  }

  // Handle -undefined.
  if (config->undefinedSymbolTreatment ==
          UndefinedSymbolTreatment::dynamic_lookup ||
      config->undefinedSymbolTreatment == UndefinedSymbolTreatment::suppress) {
    symtab->addDynamicLookup(sym.getName());
    return true;
  }

  // We do not return true here, as we still need to print diagnostics.
  if (config->undefinedSymbolTreatment == UndefinedSymbolTreatment::warning)
    symtab->addDynamicLookup(sym.getName());

  return false;
}

namespace {
struct UndefinedDiag {
  struct SectionAndOffset {
    const InputSection *isec;
    uint64_t offset;
  };

  std::vector<SectionAndOffset> codeReferences;
  std::vector<std::string> otherReferences;
};

MapVector<const Undefined *, UndefinedDiag> undefs;
} // namespace

void macho::reportPendingDuplicateSymbols() {
  for (const auto &duplicate : dupSymDiags) {
    if (!config->deadStripDuplicates || duplicate.sym->isLive()) {
      std::string message =
          "duplicate symbol: " + toString(*duplicate.sym) + "\n>>> defined in ";
      if (!duplicate.src1.first.empty())
        message += duplicate.src1.first + "\n>>>            ";
      message += duplicate.src1.second + "\n>>> defined in ";
      if (!duplicate.src2.first.empty())
        message += duplicate.src2.first + "\n>>>            ";
      error(message + duplicate.src2.second);
    }
  }
}

// Check whether the definition name def is a mangled function name that matches
// the reference name ref.
static bool canSuggestExternCForCXX(StringRef ref, StringRef def) {
  llvm::ItaniumPartialDemangler d;
  std::string name = def.str();
  if (d.partialDemangle(name.c_str()))
    return false;
  char *buf = d.getFunctionName(nullptr, nullptr);
  if (!buf)
    return false;
  bool ret = ref == buf;
  free(buf);
  return ret;
}

// Suggest an alternative spelling of an "undefined symbol" diagnostic. Returns
// the suggested symbol, which is either in the symbol table, or in the same
// file of sym.
static const Symbol *getAlternativeSpelling(const Undefined &sym,
                                            std::string &preHint,
                                            std::string &postHint) {
  DenseMap<StringRef, const Symbol *> map;
  if (sym.getFile() && sym.getFile()->kind() == InputFile::ObjKind) {
    // Build a map of local defined symbols.
    for (const Symbol *s : sym.getFile()->symbols)
      if (auto *defined = dyn_cast_or_null<Defined>(s))
        if (!defined->isExternal())
          map.try_emplace(s->getName(), s);
  }

  auto suggest = [&](StringRef newName) -> const Symbol * {
    // If defined locally.
    if (const Symbol *s = map.lookup(newName))
      return s;

    // If in the symbol table and not undefined.
    if (const Symbol *s = symtab->find(newName))
      if (!isa<Undefined>(s))
        return s;

    return nullptr;
  };

  // This loop enumerates all strings of Levenshtein distance 1 as typo
  // correction candidates and suggests the one that exists as a non-undefined
  // symbol.
  StringRef name = sym.getName();
  for (size_t i = 0, e = name.size(); i != e + 1; ++i) {
    // Insert a character before name[i].
    std::string newName = (name.substr(0, i) + "0" + name.substr(i)).str();
    for (char c = '0'; c <= 'z'; ++c) {
      newName[i] = c;
      if (const Symbol *s = suggest(newName))
        return s;
    }
    if (i == e)
      break;

    // Substitute name[i].
    newName = std::string(name);
    for (char c = '0'; c <= 'z'; ++c) {
      newName[i] = c;
      if (const Symbol *s = suggest(newName))
        return s;
    }

    // Transpose name[i] and name[i+1]. This is of edit distance 2 but it is
    // common.
    if (i + 1 < e) {
      newName[i] = name[i + 1];
      newName[i + 1] = name[i];
      if (const Symbol *s = suggest(newName))
        return s;
    }

    // Delete name[i].
    newName = (name.substr(0, i) + name.substr(i + 1)).str();
    if (const Symbol *s = suggest(newName))
      return s;
  }

  // Case mismatch, e.g. Foo vs FOO.
  for (auto &it : map)
    if (name.equals_insensitive(it.first))
      return it.second;
  for (Symbol *sym : symtab->getSymbols())
    if (!isa<Undefined>(sym) && name.equals_insensitive(sym->getName()))
      return sym;

  // The reference may be a mangled name while the definition is not. Suggest a
  // missing extern "C".
  if (name.starts_with("__Z")) {
    std::string buf = name.str();
    llvm::ItaniumPartialDemangler d;
    if (!d.partialDemangle(buf.c_str()))
      if (char *buf = d.getFunctionName(nullptr, nullptr)) {
        const Symbol *s = suggest((Twine("_") + buf).str());
        free(buf);
        if (s) {
          preHint = ": extern \"C\" ";
          return s;
        }
      }
  } else {
    StringRef nameWithoutUnderscore = name;
    nameWithoutUnderscore.consume_front("_");
    const Symbol *s = nullptr;
    for (auto &it : map)
      if (canSuggestExternCForCXX(nameWithoutUnderscore, it.first)) {
        s = it.second;
        break;
      }
    if (!s)
      for (Symbol *sym : symtab->getSymbols())
        if (canSuggestExternCForCXX(nameWithoutUnderscore, sym->getName())) {
          s = sym;
          break;
        }
    if (s) {
      preHint = " to declare ";
      postHint = " as extern \"C\"?";
      return s;
    }
  }

  return nullptr;
}

static void reportUndefinedSymbol(const Undefined &sym,
                                  const UndefinedDiag &locations,
                                  bool correctSpelling) {
  std::string message = "undefined symbol";
  if (config->archMultiple)
    message += (" for arch " + getArchitectureName(config->arch())).str();
  message += ": " + toString(sym);

  const size_t maxUndefinedReferences = 3;
  size_t i = 0;
  for (const std::string &loc : locations.otherReferences) {
    if (i >= maxUndefinedReferences)
      break;
    message += "\n>>> referenced by " + loc;
    ++i;
  }

  for (const UndefinedDiag::SectionAndOffset &loc : locations.codeReferences) {
    if (i >= maxUndefinedReferences)
      break;
    message += "\n>>> referenced by ";
    std::string src = loc.isec->getSourceLocation(loc.offset);
    if (!src.empty())
      message += src + "\n>>>               ";
    message += loc.isec->getLocation(loc.offset);
    ++i;
  }

  size_t totalReferences =
      locations.otherReferences.size() + locations.codeReferences.size();
  if (totalReferences > i)
    message +=
        ("\n>>> referenced " + Twine(totalReferences - i) + " more times")
            .str();

  if (correctSpelling) {
    std::string preHint = ": ", postHint;
    if (const Symbol *corrected =
            getAlternativeSpelling(sym, preHint, postHint)) {
      message +=
          "\n>>> did you mean" + preHint + toString(*corrected) + postHint;
      if (corrected->getFile())
        message += "\n>>> defined in: " + toString(corrected->getFile());
    }
  }

  if (config->undefinedSymbolTreatment == UndefinedSymbolTreatment::error)
    error(message);
  else if (config->undefinedSymbolTreatment ==
           UndefinedSymbolTreatment::warning)
    warn(message);
  else
    assert(false && "diagnostics make sense for -undefined error|warning only");
}

void macho::reportPendingUndefinedSymbols() {
  // Enable spell corrector for the first 2 diagnostics.
  for (const auto &[i, undef] : llvm::enumerate(undefs))
    reportUndefinedSymbol(*undef.first, undef.second, i < 2);

  // This function is called multiple times during execution. Clear the printed
  // diagnostics to avoid printing the same things again the next time.
  undefs.clear();
}

void macho::treatUndefinedSymbol(const Undefined &sym, StringRef source) {
  if (recoverFromUndefinedSymbol(sym))
    return;

  undefs[&sym].otherReferences.push_back(source.str());
}

void macho::treatUndefinedSymbol(const Undefined &sym, const InputSection *isec,
                                 uint64_t offset) {
  if (recoverFromUndefinedSymbol(sym))
    return;

  undefs[&sym].codeReferences.push_back({isec, offset});
}

std::unique_ptr<SymbolTable> macho::symtab;
