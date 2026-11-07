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
#include "SyntheticSections.h"
#include "UnwindInfoSection.h"

#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

#include <mutex>

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

// Calls addSym for each symbol and enqueue for each section that a live symbol
// keeps alive.
template <class AddSym, class Enqueue>
static void forEachSymbolReferent(Symbol *s, AddSym addSym, Enqueue enqueue) {
  if (ObjCStubsSection::isObjCClassStubSymbol(s))
    if (auto *classSym = in.objcStubs->lookupClassSymbol(s))
      addSym(classSym);
  if (auto *d = dyn_cast<Defined>(s)) {
    if (d->isec())
      enqueue(d->isec(), d->value);
    if (d->unwindEntry())
      enqueue(d->unwindEntry(), 0);
  }
}

// Returns true if the global symbol `defined` is a GC root.
static bool isRootSymbol(Defined *defined) {
  // -exported_symbol(s_list)
  // NOTE: Even though exporting private externs is an ill-defined operation,
  // we are purposely not checking for privateExtern in order to follow ld64's
  // behavior of treating all exported private extern symbols as live,
  // irrespective of whether they are autohide.
  if (!config->exportedSymbols.empty() &&
      config->exportedSymbols.match(defined->getName()))
    return true;

  // public symbols explicitly marked .no_dead_strip
  if (defined->referencedDynamically || defined->noDeadStrip)
    return true;

  // FIXME: When we implement these flags, make symbols from them GC roots:
  // * -reexported_symbol(s_list)
  // * -alias_list
  // * -init

  // In dylibs and bundles and in executables with -export_dynamic, all
  // external functions are GC roots.
  bool externsAreRoots =
      config->outputType != MH_EXECUTE || config->exportDynamic;
  return externsAreRoots && !defined->privateExtern;
}

// Calls addSym for each local symbol of `file` explicitly marked
// .no_dead_strip.
template <class AddSym>
static void forEachLocalRoot(const InputFile *file, AddSym addSym) {
  if (auto *objFile = dyn_cast<ObjFile>(file))
    for (Symbol *sym : objFile->symbols)
      if (auto *defined = dyn_cast_or_null<Defined>(sym))
        if (!defined->isExternal() && defined->noDeadStrip)
          addSym(defined);
}

// Returns true if `isec` is a GC root.
static bool isRootSection(const ConcatInputSection *isec) {
  // Sections marked no_dead_strip
  if (isec->getFlags() & S_ATTR_NO_DEAD_STRIP)
    return true;
  // mod_init_funcs, mod_term_funcs sections
  uint32_t type = sectionType(isec->getFlags());
  assert(!config->emitInitOffsets || type != S_MOD_INIT_FUNC_POINTERS);
  return type == S_MOD_INIT_FUNC_POINTERS || type == S_MOD_TERM_FUNC_POINTERS;
}

// Used for adding roots to the liveness graph and marking everything they
// reach.
class MarkLive {
public:
  virtual void enqueue(InputSection *isec, uint64_t off) = 0;
  virtual void addSym(Symbol *s) = 0;
  // Adds every GC root.
  virtual void addRoots();
  void markTransitively();
  virtual ~MarkLive() = default;

protected:
  // Marks everything reachable from the sections enqueued so far.
  virtual void drain() = 0;
  // Marks the S_ATTR_LIVE_SUPPORT section `isec` live because it refers to the
  // live `referent`, which may be null.
  virtual void enqueueLiveSupport(ConcatInputSection *isec,
                                  InputSection *referent) = 0;
  // Marks the S_ATTR_LIVE_SUPPORT sections that refer to a live section.
  // Returns true if any were newly marked.
  virtual bool markLiveSupport();
};

// Returns true if `isec` is an S_ATTR_LIVE_SUPPORT section that is not yet
// live, and refers to a live section; sets `referent` to that section, which
// may be null.
static bool isLiveSupported(const ConcatInputSection *isec,
                            InputSection *&referent) {
  if (!(isec->getFlags() & S_ATTR_LIVE_SUPPORT) || isec->live)
    return false;
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
      referent = referentIsec;
      return true;
    }
  }
  return false;
}

// Adds the roots one at a time, in a fixed order, on this thread.
void MarkLive::addRoots() {
  if (config->entry)
    addSym(config->entry);
  for (Symbol *sym : symtab->getSymbols())
    if (auto *defined = dyn_cast<Defined>(sym))
      if (isRootSymbol(defined))
        addSym(defined);
  // -u symbols
  for (Symbol *sym : config->explicitUndefineds)
    addSym(sym);
  // local symbols explicitly marked .no_dead_strip
  for (const InputFile *file : inputFiles)
    forEachLocalRoot(file, [&](Defined *defined) { addSym(defined); });
  if (auto *stubBinder =
          dyn_cast_or_null<DylibSymbol>(symtab->find("dyld_stub_binder")))
    addSym(stubBinder);
  for (ConcatInputSection *isec : inputSections)
    if (isRootSection(isec))
      enqueue(isec, 0);
  for (ConcatInputSection *isec : in.initOffsets->inputs())
    enqueue(isec, 0);
}

void MarkLive::markTransitively() {
  // Marking S_ATTR_LIVE_SUPPORT sections can make more sections live, which in
  // turn can make more S_ATTR_LIVE_SUPPORT sections live, so iterate. In
  // practice, the second iteration doesn't typically mark additional sections
  // live.
  do
    drain();
  while (markLiveSupport());
}

// S_ATTR_LIVE_SUPPORT sections are live if they point _to_ a live section.
bool MarkLive::markLiveSupport() {
  bool marked = false;
  for (ConcatInputSection *isec : inputSections) {
    // FIXME: Check if copying all S_ATTR_LIVE_SUPPORT sections into a separate
    // vector and only walking that here is faster.
    InputSection *referent;
    if (isLiveSupported(isec, referent)) {
      enqueueLiveSupport(isec, referent);
      marked = true;
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

// Marks liveness on one thread, recording which section made each section live
// so that -why_live can print the chain. -why_live is rarely used, so the
// normal -dead_strip path uses ParallelMarkLive instead.
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
  forEachSymbolReferent(
      s, [&](Symbol *sym) { addSym(sym, prev); },
      [&](InputSection *isec, uint64_t off) { enqueue(isec, off, prev); });
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

// Sets `flag` and returns true if no other thread set it first. C++17 has no
// std::atomic_ref, so the plain bool is accessed as an atomic.
static bool tryClaim(bool &flag) {
  auto &atomicFlag = reinterpret_cast<std::atomic<bool> &>(flag);
  // Most claims find the flag already set, and the load lets those skip the
  // more expensive atomic exchange.
  return !atomicFlag.load(std::memory_order_relaxed) &&
         !atomicFlag.exchange(true, std::memory_order_relaxed);
}

using SectionQueue = SmallVector<ConcatInputSection *, 0>;

// Marks liveness level by level, on as many threads as are available.
class ParallelMarkLive : public MarkLive {
public:
  void addRoots() override;
  void enqueue(InputSection *isec, uint64_t off) override {
    enqueue(isec, off, worklist);
  }
  void addSym(Symbol *s) override { addSym(s, worklist); }

private:
  // Several threads call these at once, each passing its own queue to receive
  // the sections it claims.
  void enqueue(InputSection *isec, uint64_t off, SectionQueue &queue) const;
  void addSym(Symbol *s, SectionQueue &queue) const;
  void enqueueSymbolSections(Symbol *s, SectionQueue &queue) const;
  void visit(ConcatInputSection &isec, SectionQueue &queue) const;

  void drain() override;
  void enqueueLiveSupport(ConcatInputSection *isec, InputSection *) override {
    enqueue(isec, 0);
  }
  bool markLiveSupport() override;

  // Sections that have been marked live but not yet visited.
  SectionQueue worklist;
};

// Marks `isec` (at `off`) live. Only the thread whose claim succeeds queues a
// section.
void ParallelMarkLive::enqueue(InputSection *isec, uint64_t off,
                               SectionQueue &queue) const {
  if (auto *s = dyn_cast<ConcatInputSection>(isec)) {
    if (!tryClaim(s->live))
      return;
    assert(!s->isCoalescedWeak());
    queue.push_back(s);
  } else if (auto *s = dyn_cast<CStringInputSection>(isec)) {
    s->getStringPiece(off).markLiveAtomic();
  } else {
    // WordLiteralInputSection::markLive() is already thread-safe.
    isec->markLive(off);
  }
}

// Marks what `s` keeps alive, but not `s` itself.
void ParallelMarkLive::enqueueSymbolSections(Symbol *s,
                                             SectionQueue &queue) const {
  forEachSymbolReferent(
      s, [&](Symbol *sym) { addSym(sym, queue); },
      [&](InputSection *isec, uint64_t off) { enqueue(isec, off, queue); });
}

void ParallelMarkLive::addSym(Symbol *s, SectionQueue &queue) const {
  if (tryClaim(s->used))
    enqueueSymbolSections(s, queue);
}

void ParallelMarkLive::visit(ConcatInputSection &isec,
                             SectionQueue &queue) const {
  forEachReferent(
      isec,
      [&](Symbol *s) {
        // Same as addSym(s, queue), written out so that the claim check, which
        // usually fails, doesn't sit behind a call.
        if (tryClaim(s->used))
          enqueueSymbolSections(s, queue);
      },
      [&](InputSection *referent, uint64_t off) {
        enqueue(referent, off, queue);
      });
}

// Calls fn(i, queue) for each i in [0, size). With at least 16 chunks of
// `chunkSize` items, the chunks are spread over all threads; each thread
// passes its own queue and appends it to `out` when it is done. Otherwise fn
// runs on this thread and is passed `out`.
template <class Fn>
static void forEachQueued(size_t size, size_t chunkSize, SectionQueue &out,
                          Fn fn) {
  if (size < 16 * chunkSize) {
    for (size_t i = 0; i != size; ++i)
      fn(i, out);
    return;
  }
  const size_t numThreads = parallel::getThreadCount();
  std::mutex outMutex;
  // Each thread starts with the chunk at its own index and then claims the
  // chunks after the first numThreads.
  std::atomic<size_t> next{numThreads * chunkSize};
  parallelFor(0, numThreads, [&](size_t shard) {
    SectionQueue queue;
    for (size_t begin = shard * chunkSize; begin < size;
         begin = next.fetch_add(chunkSize, std::memory_order_relaxed)) {
      size_t end = std::min(begin + chunkSize, size);
      for (size_t i = begin; i != end; ++i)
        fn(i, queue);
    }
    std::lock_guard<std::mutex> lock(outMutex);
    out.append(queue.begin(), queue.end());
  });
}

// Visits the worklist one level at a time. Workers take chunks of a level,
// small enough that every worker gets some, and visit them depth-first to take
// advantage of memory locality. Once one worker has run out of chunks, the
// others finish their current visit and move the rest of their stacks to the
// worklist for the next level, so that the level ends and all workers share the
// rest in the next one. With one thread, this reduces to a single depth-first
// walk. The last join makes every live bit visible to the live-support scan,
// which reads them with plain loads.
void ParallelMarkLive::drain() {
  const size_t numThreads = parallel::getThreadCount();
  while (!worklist.empty()) {
    SectionQueue level = std::move(worklist);
    worklist.clear();
    std::mutex worklistMutex;
    std::atomic<bool> anyWorkerIdle{false};
    size_t chunkSize =
        std::clamp<size_t>(level.size() / (4 * numThreads), 1, 64);
    // Each worker starts with the chunk at its own index and then claims the
    // chunks after the first numThreads.
    std::atomic<size_t> next{numThreads * chunkSize};
    parallelFor(0, numThreads, [&](size_t shard) {
      SectionQueue stack;
      for (size_t begin = shard * chunkSize; begin < level.size();
           begin = next.fetch_add(chunkSize, std::memory_order_relaxed)) {
        size_t end = std::min(begin + chunkSize, level.size());
        stack.append(level.begin() + begin, level.begin() + end);
        while (!stack.empty()) {
          visit(*stack.pop_back_val(), stack);
          if (anyWorkerIdle.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(worklistMutex);
            worklist.append(stack.begin(), stack.end());
            stack.clear();
          }
        }
      }
      anyWorkerIdle.store(true, std::memory_order_relaxed);
    });
  }
}

// Finds the S_ATTR_LIVE_SUPPORT sections to mark on all threads, without
// marking any, and then marks them. A section that refers only to another one
// found in the same scan is not found until the next scan, after the drain.
// markTransitively() scans until nothing more is found, so the final live set
// is the same as with MarkLive::markLiveSupport.
bool ParallelMarkLive::markLiveSupport() {
  SectionQueue found;
  forEachQueued(inputSections.size(), 256, found,
                [&](size_t i, SectionQueue &queue) {
                  InputSection *referent;
                  if (isLiveSupported(inputSections[i], referent))
                    queue.push_back(inputSections[i]);
                });
  for (ConcatInputSection *isec : found)
    enqueue(isec, 0);
  return !found.empty();
}

// Adds the same roots as MarkLive::addRoots, scanning the symbol table, the
// object files' local symbols and the input sections on all threads. Each
// thread claims the roots it finds and queues their sections; the order of
// the worklist varies, but the set of live sections and symbols does not.
void ParallelMarkLive::addRoots() {
  if (config->entry)
    addSym(config->entry);
  ArrayRef<Symbol *> symbols = symtab->getSymbols();
  forEachQueued(symbols.size(), 256, worklist,
                [&](size_t i, SectionQueue &queue) {
                  if (auto *defined = dyn_cast<Defined>(symbols[i]))
                    if (isRootSymbol(defined))
                      addSym(defined, queue);
                });
  // -u symbols
  for (Symbol *sym : config->explicitUndefineds)
    addSym(sym);
  // local symbols explicitly marked .no_dead_strip
  forEachQueued(
      inputFiles.size(), 4, worklist, [&](size_t i, SectionQueue &queue) {
        forEachLocalRoot(inputFiles[i],
                         [&](Defined *defined) { addSym(defined, queue); });
      });
  if (auto *stubBinder =
          dyn_cast_or_null<DylibSymbol>(symtab->find("dyld_stub_binder")))
    addSym(stubBinder);
  forEachQueued(inputSections.size(), 256, worklist,
                [&](size_t i, SectionQueue &queue) {
                  if (isRootSection(inputSections[i]))
                    enqueue(inputSections[i], 0, queue);
                });
  for (ConcatInputSection *isec : in.initOffsets->inputs())
    enqueue(isec, 0);
}

// Set live bit on for each reachable chunk. Unmarked (unreachable)
// InputSections will be ignored by Writer, so they will be excluded
// from the final output.
void markLive() {
  TimeTraceScope timeScope("markLive");
  MarkLive *marker;
  if (config->whyLive.empty())
    marker = make<ParallelMarkLive>();
  else
    marker = make<WhyLiveMarkLive>();
  marker->addRoots();
  marker->markTransitively();
}

} // namespace lld::macho
