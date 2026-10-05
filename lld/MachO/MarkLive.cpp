//===- MarkLive.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MarkLive.h"
#include "Config.h"
#include "OutputSegment.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "UnwindInfoSection.h"

#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

#include <algorithm>
#include <vector>

namespace lld::macho {

using namespace llvm;
using namespace llvm::MachO;

struct WhyLiveEntry {
  InputSection *isec;
  // Keep track of the entry that caused us to mark `isec` as live.
  const WhyLiveEntry *prev;

  WhyLiveEntry(InputSection *isec, const WhyLiveEntry *prev)
      : isec(isec), prev(prev) {}
};

// Type-erased interface to MarkLiveImpl. Used for adding roots to the liveness
// graph.
class MarkLive {
public:
  virtual void enqueue(InputSection *isec, uint64_t off) = 0;
  virtual void addSym(Symbol *s) = 0;
  virtual void markTransitively() = 0;
  virtual ~MarkLive() = default;
};

template <bool RecordWhyLive> class MarkLiveImpl : public MarkLive {
public:
  // -why_live is a rarely used option, so we don't want support for that flag
  // to slow down the main -dead_strip code path. As such, we employ templates
  // to avoid the usage of WhyLiveEntry in the main code path. This saves us
  // from needless allocations and pointer indirections.
  using WorklistEntry =
      std::conditional_t<RecordWhyLive, WhyLiveEntry, InputSection>;

  void enqueue(InputSection *isec, uint64_t off) override {
    enqueue(isec, off, nullptr);
  }
  void addSym(Symbol *s) override { addSym(s, nullptr); }
  void markTransitively() override;

private:
  void enqueue(InputSection *isec, uint64_t off, const WorklistEntry *prev);
  void addSym(Symbol *s, const WorklistEntry *prev);
  const InputSection *getInputSection(const WorklistEntry *) const;
  WorklistEntry *makeEntry(InputSection *, const WorklistEntry *prev) const;

  // We build up a worklist of sections which have been marked as live. We
  // only push into the worklist when we discover an unmarked section, and we
  // mark as we push, so sections never appear twice in the list. Literal
  // sections cannot contain references to other sections, so we only store
  // ConcatInputSections in our worklist.
  SmallVector<WorklistEntry *, 256> worklist;
};

template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::enqueue(
    InputSection *isec, uint64_t off,
    const typename MarkLiveImpl<RecordWhyLive>::WorklistEntry *prev) {
  if (isec->isLive(off))
    return;
  isec->markLive(off);
  if (auto s = dyn_cast<ConcatInputSection>(isec)) {
    assert(!s->isCoalescedWeak());
    worklist.push_back(makeEntry(s, prev));
  }
}

static void printWhyLive(const Symbol *s, const WhyLiveEntry *prev) {
  std::string out = toString(*s) + " from " + toString(s->getFile());
  int indent = 2;
  for (const WhyLiveEntry *entry = prev; entry;
       entry = entry->prev, indent += 2) {
    const TinyPtrVector<Defined *> &symbols = entry->isec->symbols;
    // With .subsections_with_symbols set, most isecs will have exactly one
    // entry in their symbols vector, so we just print the first one.
    if (!symbols.empty())
      out += "\n" + std::string(indent, ' ') + toString(*symbols.front()) +
             " from " + toString(symbols.front()->getFile());
  }
  message(out);
}

template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::addSym(
    Symbol *s,
    const typename MarkLiveImpl<RecordWhyLive>::WorklistEntry *prev) {
  if (s->used.load(std::memory_order_relaxed))
    return;
  s->used.store(true, std::memory_order_relaxed);
  if constexpr (RecordWhyLive)
    if (!config->whyLive.empty() && config->whyLive.match(s->getName()))
      printWhyLive(s, prev);
  if (auto *d = dyn_cast<Defined>(s)) {
    if (d->isec())
      enqueue(d->isec(), d->value, prev);
    if (d->unwindEntry())
      enqueue(d->unwindEntry(), 0, prev);
  }
}

template <bool RecordWhyLive>
const InputSection *MarkLiveImpl<RecordWhyLive>::getInputSection(
    const MarkLiveImpl<RecordWhyLive>::WorklistEntry *entry) const {
  if constexpr (RecordWhyLive)
    return entry->isec;
  else
    return entry;
}

template <bool RecordWhyLive>
typename MarkLiveImpl<RecordWhyLive>::WorklistEntry *
MarkLiveImpl<RecordWhyLive>::makeEntry(
    InputSection *isec,
    const MarkLiveImpl<RecordWhyLive>::WorklistEntry *prev) const {
  if constexpr (RecordWhyLive) {
    if (!isec) {
      assert(!prev);
      return nullptr;
    }
    return make<WhyLiveEntry>(isec, prev);
  } else {
    return isec;
  }
}

template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::markTransitively() {
  if constexpr (RecordWhyLive) {
    do {
      while (!worklist.empty()) {
        WorklistEntry *entry = worklist.pop_back_val();
        auto *isec = cast<ConcatInputSection>(getInputSection(entry));
        assert(isec->live && "We mark as live when pushing onto the worklist!");
        for (const Relocation &r : isec->relocs) {
          if (auto *s = r.getReferent().dyn_cast<Symbol *>())
            addSym(s, entry);
          else
            enqueue(cast<InputSection *>(r.getReferent()), r.getAddend(), entry);
        }
        for (Defined *d : getInputSection(entry)->symbols)
          addSym(d, entry);
      }

      for (ConcatInputSection *isec : inputSections) {
        if (!(isec->getFlags() & S_ATTR_LIVE_SUPPORT) || isec->live)
          continue;
        for (const Relocation &r : isec->relocs) {
          if (auto *s = r.getReferent().dyn_cast<Symbol *>()) {
            if (s->isLive()) {
              InputSection *referentIsec = nullptr;
              if (auto *d = dyn_cast<Defined>(s))
                referentIsec = d->isec();
              enqueue(isec, 0, makeEntry(referentIsec, nullptr));
            }
          } else {
            auto *referentIsec = cast<InputSection *>(r.getReferent());
            if (referentIsec->isLive(r.getAddend()))
              enqueue(isec, 0, makeEntry(referentIsec, nullptr));
          }
        }
      }
    } while (!worklist.empty());
  } else {

    // Every liveness bit a worker touches is now set atomically: concat sections
    // and symbols through their own atomic flags, literal pieces through
    // InputSection::markLive(), whose CStringInputSection and
    // WordLiteralInputSection implementations OR into an atomic word. There is
    // no serial literal merge left.
    auto enqueueParallel = [](InputSection *isec, uint64_t off,
                              SmallVectorImpl<WorklistEntry *> &next) {
      if (auto *concat = dyn_cast<ConcatInputSection>(isec)) {
        if (!concat->live.load(std::memory_order_relaxed) &&
            !concat->live.exchange(true, std::memory_order_relaxed)) {
          assert(!concat->isCoalescedWeak());
          next.push_back(concat);
        }
        return;
      }
      isec->markLive(off);
    };
    auto addSymParallel = [&](Symbol *s,
                              SmallVectorImpl<WorklistEntry *> &next) {
      if (s->used.load(std::memory_order_relaxed) ||
          s->used.exchange(true, std::memory_order_relaxed))
        return;
      if (auto *d = dyn_cast<Defined>(s)) {
        if (d->isec())
          enqueueParallel(d->isec(), d->value, next);
        if (d->unwindEntry())
          enqueueParallel(d->unwindEntry(), 0, next);
      }
    };
    auto visit = [&](ConcatInputSection *isec,
                     SmallVectorImpl<WorklistEntry *> &next) {
      for (const Relocation &r : isec->relocs) {
        if (auto *s = r.getReferent().dyn_cast<Symbol *>())
          addSymParallel(s, next);
        else
          enqueueParallel(cast<InputSection *>(r.getReferent()), r.getAddend(),
                          next);
      }
      for (Defined *d : isec->symbols)
        addSymParallel(d, next);
    };

    while (true) {
      while (!worklist.empty()) {
        SmallVector<WorklistEntry *, 256> frontier;
        frontier.swap(worklist);
        const size_t workers =
            std::min(frontier.size(), llvm::parallel::getThreadCount());
        if (workers == 1 || frontier.size() < 1024) {
          for (WorklistEntry *entry : frontier)
            visit(cast<ConcatInputSection>(entry), worklist);
          continue;
        }

        SmallVector<SmallVector<WorklistEntry *, 0>, 0> nextWorklists(workers);
        {
          llvm::parallel::TaskGroup tasks;
          for (size_t worker = 0; worker != workers; ++worker) {
            const size_t begin = frontier.size() * worker / workers;
            const size_t end = frontier.size() * (worker + 1) / workers;
            tasks.spawn([&, worker, begin, end] {
              for (size_t i = begin; i != end; ++i)
                visit(cast<ConcatInputSection>(frontier[i]),
                      nextWorklists[worker]);
            });
          }
        }
        for (SmallVectorImpl<WorklistEntry *> &next : nextWorklists)
          worklist.append(next.begin(), next.end());
      }

      // Keep LIVE_SUPPORT and its fixed point serial: each scan observes all
      // literal writes from the preceding frontier and earlier support entries.
      // Gathering retained the candidate subsequence in inputSections order.
      for (ConcatInputSection *isec : liveSupportSections) {
        if (isec->live)
          continue;
        for (const Relocation &r : isec->relocs) {
          bool live;
          if (auto *s = r.getReferent().dyn_cast<Symbol *>())
            live = s->isLive();
          else
            live = cast<InputSection *>(r.getReferent())->isLive(r.getAddend());
          if (live) {
            enqueue(isec, 0, nullptr);
            // Subsequent referents can only enqueue this same section again.
            // Its outgoing references are visited in the next frontier drain.
            break;
          }
        }
      }
      if (worklist.empty())
        break;
    }
  }
}

// Set live bit on for each reachable chunk. Unmarked (unreachable)
// InputSections will be ignored by Writer, so they will be excluded
// from the final output.
void markLive() {
  TimeTraceScope timeScope("markLive");
  MarkLive *marker;
  if (config->whyLive.empty())
    marker = make<MarkLiveImpl<false>>();
  else
    marker = make<MarkLiveImpl<true>>();
  // Add GC roots.
  if (config->entry)
    marker->addSym(config->entry);
  // Deciding whether a symbol is a root is a pure read of its name and flags,
  // and on links with an -exported_symbols_list the name hashing dominates this
  // scope. So select roots on workers into per-chunk lists and then addSym()
  // them serially in symVector order, which is exactly the order the serial loop
  // used. -why_live prints as it marks, so it keeps the serial path.
  ArrayRef<Symbol *> symbols = symtab->getSymbols();
  // In dylibs and bundles and in executables with -export_dynamic, all
  // external functions are GC roots.
  const bool externsAreRoots =
      config->outputType != MH_EXECUTE || config->exportDynamic;
  auto isRoot = [externsAreRoots](const Symbol *sym) {
    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return false;
    // -exported_symbol(s_list)
    //
    // NOTE: Even though exporting private externs is an ill-defined
    // operation, we are purposely not checking for privateExtern in
    // order to follow ld64's behavior of treating all exported private
    // extern symbols as live, irrespective of whether they are autohide.
    if (!config->exportedSymbols.empty() &&
        config->exportedSymbols.match(defined->getName()))
      return true;
    // public symbols explicitly marked .no_dead_strip
    if (defined->referencedDynamically || defined->noDeadStrip)
      return true;
    // FIXME: When we implement these flags, make symbols from them GC
    // roots:
    // * -reexported_symbol(s_list)
    // * -alias_list
    // * -init
    return externsAreRoots && !defined->privateExtern;
  };
  const size_t rootChunks =
      config->whyLive.empty() && symbols.size() >= 2048 &&
              llvm::parallel::strategy.ThreadsRequested != 1
          ? llvm::parallel::getThreadCount()
          : 1;
  if (rootChunks <= 1) {
    for (Symbol *sym : symbols)
      if (isRoot(sym))
        marker->addSym(sym);
  } else {
    std::vector<std::vector<Symbol *>> chunkRoots(rootChunks);
    llvm::parallelFor(0, rootChunks, [&](size_t chunk) {
      const size_t begin = symbols.size() * chunk / rootChunks;
      const size_t end = symbols.size() * (chunk + 1) / rootChunks;
      std::vector<Symbol *> &roots = chunkRoots[chunk];
      roots.reserve((end - begin) / 8 + 1);
      for (size_t i = begin; i != end; ++i)
        if (isRoot(symbols[i]))
          roots.push_back(symbols[i]);
    });
    for (const std::vector<Symbol *> &roots : chunkRoots)
      for (Symbol *sym : roots)
        marker->addSym(sym);
  }
  // -u symbols
  for (Symbol *sym : config->explicitUndefineds)
    marker->addSym(sym);
  // Local symbols explicitly marked .no_dead_strip. parseSymbols() recorded
  // their nlist indices, so this touches only the roots instead of every symbol
  // slot of every object file.
  for (const InputFile *file : inputFiles)
    if (auto *objFile = dyn_cast<ObjFile>(file))
      for (uint32_t index : objFile->noDeadStripLocals) {
        auto *defined = cast<Defined>(objFile->symbols[index]);
        assert(!defined->isExternal() && defined->noDeadStrip);
        marker->addSym(defined);
      }
  if (auto *stubBinder =
          dyn_cast_or_null<DylibSymbol>(symtab->find("dyld_stub_binder")))
    marker->addSym(stubBinder);
  // Sections marked no_dead_strip, and mod_init_funcs / mod_term_funcs
  // sections. gatherInputSections() collected these on its workers, in
  // `inputSections` order, so the enqueue order is unchanged.
  for (ConcatInputSection *isec : deadStripSectionRoots)
    marker->enqueue(isec, 0);

  for (ConcatInputSection *isec : in.initOffsets->inputs())
    marker->enqueue(isec, 0);

  marker->markTransitively();
}

} // namespace lld::macho
