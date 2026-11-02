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

namespace lld::macho {

using namespace llvm;
using namespace llvm::MachO;

// Calls addSym for each symbol and enqueue for each section that a live `isec`
// keeps alive.
template <class AddSym, class Enqueue>
static void forEachReferent(const ConcatInputSection &isec, AddSym addSym,
                            Enqueue enqueue) {
  for (const Relocation &r : isec.relocs) {
    if (auto *s = dyn_cast<Symbol *>(r.referent))
      addSym(s);
    else
      enqueue(cast<InputSection *>(r.referent), r.getAddend());
  }
  for (Defined *d : isec.symbols)
    addSym(d);
}

// Calls enqueue for each section that a live symbol keeps alive.
template <class Enqueue>
static void forEachSymbolSection(Symbol *s, Enqueue enqueue) {
  if (auto *d = dyn_cast<Defined>(s)) {
    if (d->isec())
      enqueue(d->isec(), d->value);
    if (d->unwindEntry())
      enqueue(d->unwindEntry(), 0);
  }
}

// Used for adding roots to the liveness graph and marking everything they
// reach.
class MarkLive {
public:
  virtual void enqueue(InputSection *isec, uint64_t off) = 0;
  virtual void addSym(Symbol *s) = 0;
  void markTransitively();
  virtual ~MarkLive() = default;

protected:
  // Marks everything reachable from the sections enqueued so far.
  virtual void drain() = 0;
  // Marks the S_ATTR_LIVE_SUPPORT section `isec` live because it refers to
  // the live `referent`, which may be null.
  virtual void enqueueLiveSupport(ConcatInputSection *isec,
                                  InputSection *referent) = 0;

private:
  bool markLiveSupport();
};

void MarkLive::markTransitively() {
  // Marking S_ATTR_LIVE_SUPPORT sections can make more sections live, which in
  // turn can make more S_ATTR_LIVE_SUPPORT sections live, so iterate. In
  // practice, the second iteration won't mark additional sections live.
  do
    drain();
  while (markLiveSupport());
}

// S_ATTR_LIVE_SUPPORT sections are live if they point _to_ a live section.
// Returns true if any were newly marked.
bool MarkLive::markLiveSupport() {
  bool marked = false;
  for (ConcatInputSection *isec : inputSections) {
    // FIXME: Check if copying all S_ATTR_LIVE_SUPPORT sections into a
    // separate vector and only walking that here is faster.
    if (!(isec->getFlags() & S_ATTR_LIVE_SUPPORT) || isec->live)
      continue;

    for (const Relocation &r : isec->relocs) {
      InputSection *referentIsec = nullptr;
      bool referentLive;
      if (auto *s = dyn_cast<Symbol *>(r.referent)) {
        referentLive = s->isLive();
        if (auto *d = dyn_cast<Defined>(s))
          referentIsec = d->isec();
      } else {
        referentIsec = cast<InputSection *>(r.referent);
        referentLive = referentIsec->isLive(r.getAddend());
      }
      if (referentLive) {
        enqueueLiveSupport(isec, referentIsec);
        marked = true;
        break;
      }
    }
  }
  return marked;
}

struct WhyLiveEntry {
  InputSection *isec;
  // Keep track of the entry that caused us to mark `isec` as live.
  const WhyLiveEntry *prev;

  WhyLiveEntry(InputSection *isec, const WhyLiveEntry *prev)
      : isec(isec), prev(prev) {}
};

// Marks liveness on one thread, recording which section made each section
// live so that -why_live can print the chain. -why_live is rarely used, so
// the normal -dead_strip path uses ParallelMarkLive instead.
class WhyLiveMarkLive : public MarkLive {
public:
  void enqueue(InputSection *isec, uint64_t off) override {
    enqueue(isec, off, nullptr);
  }
  void addSym(Symbol *s) override { addSym(s, nullptr); }

private:
  void enqueue(InputSection *isec, uint64_t off, const WhyLiveEntry *prev);
  void addSym(Symbol *s, const WhyLiveEntry *prev);
  void drain() override;
  void enqueueLiveSupport(ConcatInputSection *isec,
                          InputSection *referent) override {
    enqueue(isec, 0,
            referent ? make<WhyLiveEntry>(referent, nullptr) : nullptr);
  }

  // We build up a worklist of sections which have been marked as live. We
  // only push into the worklist when we discover an unmarked section, and we
  // mark as we push, so sections never appear twice in the list. Literal
  // sections cannot contain references to other sections, so we only store
  // ConcatInputSections in our worklist.
  SmallVector<WhyLiveEntry *, 256> worklist;
};

void WhyLiveMarkLive::enqueue(InputSection *isec, uint64_t off,
                              const WhyLiveEntry *prev) {
  if (isec->isLive(off))
    return;
  isec->markLive(off);
  if (auto s = dyn_cast<ConcatInputSection>(isec)) {
    assert(!s->isCoalescedWeak());
    worklist.push_back(make<WhyLiveEntry>(s, prev));
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

void WhyLiveMarkLive::addSym(Symbol *s, const WhyLiveEntry *prev) {
  if (s->used)
    return;
  s->used = true;
  if (config->whyLive.match(s->getName()))
    printWhyLive(s, prev);
  forEachSymbolSection(
      s, [&](InputSection *isec, uint64_t off) { enqueue(isec, off, prev); });
}

void WhyLiveMarkLive::drain() {
  while (!worklist.empty()) {
    WhyLiveEntry *entry = worklist.pop_back_val();
    // Entries that get placed onto the worklist always contain
    // ConcatInputSections. `WhyLiveEntry::prev` may point to entries that
    // contain other types of InputSections (due to S_ATTR_LIVE_SUPPORT), but
    // those entries should never be pushed onto the worklist.
    auto *isec = cast<ConcatInputSection>(entry->isec);
    assert(isec->live && "We mark as live when pushing onto the worklist!");
    forEachReferent(
        *isec, [&](Symbol *s) { addSym(s, entry); },
        [&](InputSection *referent, uint64_t off) {
          enqueue(referent, off, entry);
        });
  }
}

// Set by markLive() when the link runs on one thread.
static bool singleThreaded;

// Sets `flag` and returns true if no other thread set it first. C++17 has no
// std::atomic_ref, so the plain bool is accessed as an atomic.
static bool tryClaim(bool &flag) {
  auto &atomicFlag = reinterpret_cast<std::atomic<bool> &>(flag);
  // Most claims find the flag already set, and the plain load lets those skip
  // the more expensive atomic exchange.
  return !atomicFlag.load(std::memory_order_relaxed) &&
         !atomicFlag.exchange(true, std::memory_order_relaxed);
}

using SectionQueue = SmallVector<ConcatInputSection *, 0>;

// A thread visits the sections it claims directly, up to this depth of
// recursion, which keeps chains of references on one thread. Deeper sections
// go to the next level of the traversal.
static constexpr int maxInlineDepth = 3;

static void visitParallel(ConcatInputSection &isec, int depth,
                          SectionQueue &queue);

// Marks `isec` (at `off`) live. Safe to call from several threads at once.
static void enqueueParallel(InputSection *isec, uint64_t off, int depth,
                            SectionQueue &queue) {
  if (auto *s = dyn_cast<ConcatInputSection>(isec)) {
    if (!tryClaim(s->live))
      return;
    assert(!s->isCoalescedWeak());
    if (depth < maxInlineDepth)
      visitParallel(*s, depth + 1, queue);
    else
      queue.push_back(s);
  } else if (auto *s = dyn_cast<CStringInputSection>(isec)) {
    s->getStringPiece(off).markLiveAtomic();
  } else {
    // Word literals are marked with an atomic store.
    isec->markLive(off);
  }
}

static void addSymParallel(Symbol *s, int depth, SectionQueue &queue) {
  if (!tryClaim(s->used))
    return;
  forEachSymbolSection(s, [&](InputSection *isec, uint64_t off) {
    enqueueParallel(isec, off, depth, queue);
  });
}

static void visitParallel(ConcatInputSection &isec, int depth,
                          SectionQueue &queue) {
  forEachReferent(
      isec, [&](Symbol *s) { addSymParallel(s, depth, queue); },
      [&](InputSection *referent, uint64_t off) {
        enqueueParallel(referent, off, depth, queue);
      });
}

// Marks liveness level by level, on as many threads as are available.
class ParallelMarkLive : public MarkLive {
public:
  void enqueue(InputSection *isec, uint64_t off) override {
    enqueueParallel(isec, off, maxInlineDepth, worklist);
  }
  void addSym(Symbol *s) override {
    addSymParallel(s, maxInlineDepth, worklist);
  }

private:
  void drain() override;
  void enqueueLiveSupport(ConcatInputSection *isec, InputSection *) override {
    enqueue(isec, 0);
  }

  // Sections that have been marked live but not yet visited.
  SectionQueue worklist;
};

// Visits the worklist one level at a time. Workers claim chunks of a level and
// collect the sections they claim but don't visit in their own queues, which
// together become the next level. Small levels are visited on this thread. The
// join orders all live-bit updates before the next level and the live-support
// scan.
//
// With one thread, the worklist is instead used as a stack, without inline
// visits, as WhyLiveMarkLive does; that order is faster than going level by
// level.
void ParallelMarkLive::drain() {
  if (singleThreaded) {
    while (!worklist.empty()) {
      ConcatInputSection *isec = worklist.pop_back_val();
      // Visiting at maxInlineDepth queues every section it reaches.
      visitParallel(*isec, maxInlineDepth, worklist);
    }
    return;
  }
  while (!worklist.empty()) {
    SectionQueue level = std::move(worklist);
    worklist.clear();
    if (level.size() < 1024) {
      for (ConcatInputSection *isec : level)
        visitParallel(*isec, 0, worklist);
      continue;
    }
    const size_t numThreads = parallel::getThreadCount();
    auto queues = std::make_unique<SectionQueue[]>(numThreads);
    std::atomic<size_t> next{0};
    parallelFor(0, numThreads, [&](size_t shard) {
      for (;;) {
        size_t begin = next.fetch_add(64, std::memory_order_relaxed);
        if (begin >= level.size())
          break;
        size_t end = std::min(begin + 64, level.size());
        for (size_t i = begin; i != end; ++i)
          visitParallel(*level[i], 0, queues[shard]);
      }
    });
    for (size_t shard = 0; shard != numThreads; ++shard)
      worklist.append(queues[shard].begin(), queues[shard].end());
  }
}

// Set live bit on for each reachable chunk. Unmarked (unreachable)
// InputSections will be ignored by Writer, so they will be excluded
// from the final output.
void markLive() {
  TimeTraceScope timeScope("markLive");
  singleThreaded = parallel::strategy.ThreadsRequested == 1;
  MarkLive *marker;
  if (config->whyLive.empty())
    marker = make<ParallelMarkLive>();
  else
    marker = make<WhyLiveMarkLive>();
  // Add GC roots.
  if (config->entry)
    marker->addSym(config->entry);
  for (Symbol *sym : symtab->getSymbols()) {
    if (auto *defined = dyn_cast<Defined>(sym)) {
      // -exported_symbol(s_list)
      if (!config->exportedSymbols.empty() &&
          config->exportedSymbols.match(defined->getName())) {
        // NOTE: Even though exporting private externs is an ill-defined
        // operation, we are purposely not checking for privateExtern in
        // order to follow ld64's behavior of treating all exported private
        // extern symbols as live, irrespective of whether they are autohide.
        marker->addSym(defined);
        continue;
      }

      // public symbols explicitly marked .no_dead_strip
      if (defined->referencedDynamically || defined->noDeadStrip) {
        marker->addSym(defined);
        continue;
      }

      // FIXME: When we implement these flags, make symbols from them GC
      // roots:
      // * -reexported_symbol(s_list)
      // * -alias_list
      // * -init

      // In dylibs and bundles and in executables with -export_dynamic,
      // all external functions are GC roots.
      bool externsAreRoots =
          config->outputType != MH_EXECUTE || config->exportDynamic;
      if (externsAreRoots && !defined->privateExtern) {
        marker->addSym(defined);
        continue;
      }
    }
  }
  // -u symbols
  for (Symbol *sym : config->explicitUndefineds)
    marker->addSym(sym);
  // local symbols explicitly marked .no_dead_strip
  for (const InputFile *file : inputFiles)
    if (auto *objFile = dyn_cast<ObjFile>(file))
      for (Symbol *sym : objFile->symbols)
        if (auto *defined = dyn_cast_or_null<Defined>(sym))
          if (!defined->isExternal() && defined->noDeadStrip)
            marker->addSym(defined);
  if (auto *stubBinder =
          dyn_cast_or_null<DylibSymbol>(symtab->find("dyld_stub_binder")))
    marker->addSym(stubBinder);
  for (ConcatInputSection *isec : inputSections) {
    // Sections marked no_dead_strip
    if (isec->getFlags() & S_ATTR_NO_DEAD_STRIP) {
      marker->enqueue(isec, 0);
      continue;
    }

    // mod_init_funcs, mod_term_funcs sections
    if (sectionType(isec->getFlags()) == S_MOD_INIT_FUNC_POINTERS ||
        sectionType(isec->getFlags()) == S_MOD_TERM_FUNC_POINTERS) {
      assert(!config->emitInitOffsets ||
             sectionType(isec->getFlags()) != S_MOD_INIT_FUNC_POINTERS);
      marker->enqueue(isec, 0);
      continue;
    }
  }

  for (ConcatInputSection *isec : in.initOffsets->inputs())
    marker->enqueue(isec, 0);

  marker->markTransitively();
}

} // namespace lld::macho
