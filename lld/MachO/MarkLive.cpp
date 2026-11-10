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

#include <memory>
#include <mutex>
#include <thread>
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
  // These add roots to worklist `shard`. We have one shard per thread.
  virtual void enqueue(InputSection *isec, uint64_t off, size_t shard = 0) = 0;
  virtual void addSym(Symbol *s, size_t shard = 0) = 0;
  // Moves the roots in `shard` into `toVisit`. Exposed as a performance
  // optimization; flushing shortly after the worklist is populated means its
  // entries are likely still in cache.
  virtual void flush(size_t shard) = 0;
  // Flushes all the shards into `toVisit` before doing the marking.
  virtual void markTransitively() = 0;
  virtual ~MarkLive() = default;
};

// Sets `flag` and returns true if no other thread set it first. C++17 has no
// std::atomic_ref, so the plain bool is accessed as an atomic.
static bool tryClaim(bool &flag) {
  auto &atomicFlag = reinterpret_cast<std::atomic<bool> &>(flag);
  // Most claims find the flag already set, and the load lets those skip the
  // more expensive atomic exchange.
  return !atomicFlag.load(std::memory_order_relaxed) &&
         !atomicFlag.exchange(true, std::memory_order_relaxed);
}

// Splits `items` into chunks of `chunkSize` and calls fn(worker, chunk) for
// each chunk on all threads. `worker` is below parallel::getThreadCount() and
// no two threads use the same one at once, so fn can use it to index
// per-thread state. Chunks are claimed in increasing order.
template <class T, class Fn>
static void parallelForChunks(ArrayRef<T> items, size_t chunkSize, Fn fn) {
  const size_t numThreads = parallel::getThreadCount();
  // Each worker starts with the chunk at its own index and then claims the
  // chunks after the first numThreads.
  std::atomic<size_t> next{numThreads * chunkSize};
  parallelFor(0, numThreads, [&](size_t worker) {
    for (size_t begin = worker * chunkSize; begin < items.size();
         begin = next.fetch_add(chunkSize, std::memory_order_relaxed))
      fn(worker, items.slice(begin, std::min(chunkSize, items.size() - begin)));
  });
}

namespace {
// A Chase-Lev work-stealing deque of non-null pointers, as in "Correct and
// Efficient Work-Stealing for Weak Memory Models" (Le et al., PPoPP 2013). Its
// owner pushes and pops at the bottom; other threads steal from the top.
template <class T> class StealDeque {
  struct Buffer {
    explicit Buffer(int64_t cap)
        : cap(cap), slots(std::make_unique<std::atomic<T *>[]>(cap)) {}
    T *get(int64_t i) const {
      return slots[i & (cap - 1)].load(std::memory_order_relaxed);
    }
    void put(int64_t i, T *x) {
      slots[i & (cap - 1)].store(x, std::memory_order_relaxed);
    }
    int64_t cap;
    std::unique_ptr<std::atomic<T *>[]> slots;
  };

  std::atomic<int64_t> top{0};
  std::atomic<int64_t> bottom{0};
  std::atomic<Buffer *> buffer;
  // Thieves may still be reading a buffer that push() has outgrown, so every
  // buffer lives as long as the deque.
  std::vector<std::unique_ptr<Buffer>> buffers;

public:
  StealDeque() {
    buffers.push_back(std::make_unique<Buffer>(1024));
    buffer.store(buffers.back().get(), std::memory_order_relaxed);
  }

  void push(T *x) {
    int64_t b = bottom.load(std::memory_order_relaxed);
    int64_t t = top.load(std::memory_order_acquire);
    Buffer *a = buffer.load(std::memory_order_relaxed);
    if (b - t >= a->cap) {
      auto grown = std::make_unique<Buffer>(a->cap * 2);
      for (int64_t i = t; i != b; ++i)
        grown->put(i, a->get(i));
      a = grown.get();
      buffers.push_back(std::move(grown));
      buffer.store(a, std::memory_order_release);
    }
    a->put(b, x);
    bottom.store(b + 1, std::memory_order_release);
  }

  // Returns nullptr if the deque is empty.
  T *pop() {
    int64_t b = bottom.load(std::memory_order_relaxed) - 1;
    Buffer *a = buffer.load(std::memory_order_relaxed);
    bottom.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top.load(std::memory_order_relaxed);
    if (t > b) {
      bottom.store(b + 1, std::memory_order_relaxed);
      return nullptr;
    }
    T *x = a->get(b);
    if (t == b) {
      // The last entry: race any thieves for it.
      if (!top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                       std::memory_order_relaxed))
        x = nullptr;
      bottom.store(b + 1, std::memory_order_relaxed);
    }
    return x;
  }

  // Returns nullptr if the deque is empty or another thread took the entry.
  T *steal() {
    int64_t t = top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = bottom.load(std::memory_order_acquire);
    if (t >= b)
      return nullptr;
    T *x = buffer.load(std::memory_order_acquire)->get(t);
    if (!top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                     std::memory_order_relaxed))
      return nullptr;
    return x;
  }

  bool mayHaveWork() const {
    return top.load(std::memory_order_relaxed) <
           bottom.load(std::memory_order_relaxed);
  }
};
} // namespace

// Marks liveness on as many threads as are available. -why_live output is
// deterministic only with --threads=1.
template <bool RecordWhyLive> class MarkLiveImpl : public MarkLive {
public:
  // -why_live is a rarely used option, so we don't want support for that flag
  // to slow down the main -dead_strip code path. As such, we employ templates
  // to avoid the usage of WhyLiveEntry in the main code path. This saves us
  // from needless allocations and pointer indirections.
  using WorklistEntry =
      std::conditional_t<RecordWhyLive, WhyLiveEntry, InputSection>;
  using Worklist = SmallVector<WorklistEntry *, 0>;

  MarkLiveImpl() : shards(parallel::getThreadCount()) {}
  void enqueue(InputSection *isec, uint64_t off, size_t shard) override {
    enqueue(isec, off, nullptr, shards[shard]);
  }
  void addSym(Symbol *s, size_t shard) override {
    addSym(s, nullptr, shards[shard]);
  }
  void flush(size_t shard) override;
  void markTransitively() override;

private:
  // Several threads call these at once, each passing its own worklist to
  // receive the sections it claims.
  void enqueue(InputSection *isec, uint64_t off, const WorklistEntry *prev,
               Worklist &worklist) const;
  // Most calls find `s` already claimed, so keep that check out from behind a
  // call.
  LLVM_ATTRIBUTE_ALWAYS_INLINE void addSym(Symbol *s, const WorklistEntry *prev,
                                           Worklist &worklist) const;
  void visitSymbol(Symbol *s, const WorklistEntry *prev,
                   Worklist &worklist) const;
  void visit(WorklistEntry *entry, Worklist &worklist) const;
  void drain();
  const InputSection *getInputSection(const WorklistEntry *) const;
  WorklistEntry *makeEntry(InputSection *, const WorklistEntry *prev) const;

  std::vector<Worklist> shards;
  std::mutex toVisitMutex;
  // Sections that have been marked live and that the next drain() will visit.
  // Literal sections cannot contain references to other sections, so we only
  // store ConcatInputSections here.
  Worklist toVisit;
};

// Marks `isec` (at `off`) live. Only the thread whose claim succeeds queues a
// section.
template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::enqueue(InputSection *isec, uint64_t off,
                                          const WorklistEntry *prev,
                                          Worklist &worklist) const {
  if (auto *s = dyn_cast<ConcatInputSection>(isec)) {
    if (!tryClaim(s->live))
      return;
    assert(!s->isCoalescedWeak());
    worklist.push_back(makeEntry(s, prev));
  } else {
    isec->markLive(off);
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
void MarkLiveImpl<RecordWhyLive>::addSym(Symbol *s, const WorklistEntry *prev,
                                         Worklist &worklist) const {
  if (tryClaim(s->used))
    visitSymbol(s, prev, worklist);
}

// Marks what `s` keeps alive. Called by the thread that claimed `s`. This is
// separate from addSym() because the class-stub edge makes it recursive, and
// GCC won't always-inline a function that calls itself.
template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::visitSymbol(Symbol *s,
                                              const WorklistEntry *prev,
                                              Worklist &worklist) const {
  if constexpr (RecordWhyLive)
    if (!config->whyLive.empty() && config->whyLive.match(s->getName()))
      printWhyLive(s, prev);
  if (auto *classSym = in.objcStubs->lookupClassSymbol(s))
    addSym(classSym, prev, worklist);
  if (auto *d = dyn_cast<Defined>(s)) {
    if (d->isec())
      enqueue(d->isec(), d->value, prev, worklist);
    if (d->unwindEntry())
      enqueue(d->unwindEntry(), 0, prev, worklist);
  }
}

template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::visit(WorklistEntry *entry,
                                        Worklist &worklist) const {
  // Entries that get placed onto the worklist always contain
  // ConcatInputSections. `WhyLiveEntry::prev` may point to entries that
  // contain other types of InputSections (due to S_ATTR_LIVE_SUPPORT), but
  // those entries should never be pushed onto the worklist.
  auto *isec = cast<ConcatInputSection>(getInputSection(entry));
  assert(isec->isLive(0) && "We mark as live when pushing onto the worklist!");

  // Mark all symbols listed in the relocation table for this section.
  for (const Relocation &r : isec->relocs) {
    if (auto *s = dyn_cast<Symbol *>(r.referent))
      addSym(s, entry, worklist);
    else
      enqueue(cast<InputSection *>(r.referent), r.getAddend(), entry, worklist);
  }
  for (Defined *d : isec->symbols)
    addSym(d, entry, worklist);
}

// Visits the sections in toVisit and everything they reach. Each worker takes
// chunks of toVisit and visits them depth-first from its own deque, to take
// advantage of memory locality. Once toVisit is used up, a worker whose deque
// is empty steals the oldest entries of other workers' deques. With one thread
// nothing is stolen, so this is a single depth-first walk.
template <bool RecordWhyLive> void MarkLiveImpl<RecordWhyLive>::drain() {
  const size_t numThreads = parallel::getThreadCount();
  Worklist roots = std::move(toVisit);
  toVisit.clear();
  size_t chunkSize = std::clamp<size_t>(roots.size() / (4 * numThreads), 1, 64);
  std::atomic<size_t> nextRoot{0};
  std::vector<StealDeque<WorklistEntry>> deques(numThreads);
  // Workers that may hold work: those taking chunks of roots, and those
  // visiting what they stole. Only these push onto a deque, and each empties
  // its own deque before it stops counting itself, so once this is zero there
  // is nothing left to visit.
  std::atomic<size_t> active{0};

  parallelFor(0, numThreads, [&](size_t shard) {
    StealDeque<WorklistEntry> &deque = deques[shard];
    Worklist found;
    // Visits `entry`, if any, and then everything in our deque.
    auto visitAll = [&](WorklistEntry *entry) {
      for (; entry; entry = deque.pop()) {
        visit(entry, found);
        for (WorklistEntry *e : found)
          deque.push(e);
        found.clear();
      }
    };

    active.fetch_add(1);
    while (true) {
      size_t begin = nextRoot.fetch_add(chunkSize, std::memory_order_relaxed);
      if (begin >= roots.size())
        break;
      size_t end = std::min(begin + chunkSize, roots.size());
      for (size_t i = begin; i != end; ++i)
        deque.push(roots[i]);
      visitAll(deque.pop());
    }
    active.fetch_sub(1);

    while (active.load() != 0) {
      bool stole = false;
      for (size_t i = 1; i != numThreads; ++i) {
        StealDeque<WorklistEntry> &victim = deques[(shard + i) % numThreads];
        if (!victim.mayHaveWork())
          continue;
        // Count ourselves before taking the entry, so that no one sees zero
        // while we hold it.
        active.fetch_add(1);
        if (WorklistEntry *entry = victim.steal()) {
          visitAll(entry);
          stole = true;
        }
        active.fetch_sub(1);
      }
      if (!stole)
        std::this_thread::yield();
    }
  });
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
    return makeThreadLocal<WhyLiveEntry>(isec, prev);
  } else {
    return isec;
  }
}

template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::flush(size_t shard) {
  Worklist &worklist = shards[shard];
  if (worklist.empty())
    return;
  std::lock_guard<std::mutex> lock(toVisitMutex);
  toVisit.append(worklist.begin(), worklist.end());
  worklist.clear();
}

template <bool RecordWhyLive>
void MarkLiveImpl<RecordWhyLive>::markTransitively() {
  for (size_t shard = 0; shard != shards.size(); ++shard)
    flush(shard);
  do {
    // Mark things reachable from GC roots as live.
    drain();

    // S_ATTR_LIVE_SUPPORT sections are live if they point _to_ a live
    // section. Process them in a second pass.
    parallelForChunks(
        ArrayRef(inputSections), 256,
        [&](size_t shard, ArrayRef<ConcatInputSection *> chunk) {
          for (ConcatInputSection *isec : chunk) {
            // FIXME: Check if copying all S_ATTR_LIVE_SUPPORT sections into a
            // separate vector and only walking that here is faster.
            if (!(isec->getFlags() & S_ATTR_LIVE_SUPPORT) || isec->live)
              continue;

            for (const Relocation &r : isec->relocs) {
              InputSection *referentIsec = nullptr;
              if (auto *s = dyn_cast<Symbol *>(r.referent)) {
                if (!s->isLive())
                  continue;
                if (auto *d = dyn_cast<Defined>(s))
                  referentIsec = d->isec();
              } else {
                referentIsec = cast<InputSection *>(r.referent);
                if (!referentIsec->isLive(r.getAddend()))
                  continue;
              }
              enqueue(isec, 0, makeEntry(referentIsec, nullptr), shards[shard]);
              break;
            }
          }
          flush(shard);
        });

    // S_ATTR_LIVE_SUPPORT could have marked additional sections live,
    // which in turn could mark additional S_ATTR_LIVE_SUPPORT sections live.
    // Iterate. In practice, the second iteration won't mark additional
    // S_ATTR_LIVE_SUPPORT sections live.
  } while (!toVisit.empty());
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
  parallelForChunks(
      symtab->getSymbols(), 256, [&](size_t shard, ArrayRef<Symbol *> chunk) {
        for (Symbol *sym : chunk) {
          auto *defined = dyn_cast<Defined>(sym);
          if (!defined)
            continue;
          // -exported_symbol(s_list)
          if (!config->exportedSymbols.empty() &&
              config->exportedSymbols.match(defined->getName())) {
            // NOTE: Even though exporting private externs is an ill-defined
            // operation, we are purposely not checking for privateExtern in
            // order to follow ld64's behavior of treating all exported private
            // extern symbols as live, irrespective of whether they are
            // autohide.
            marker->addSym(defined, shard);
            continue;
          }

          // public symbols explicitly marked .no_dead_strip
          if (defined->referencedDynamically || defined->noDeadStrip) {
            marker->addSym(defined, shard);
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
          if (externsAreRoots && !defined->privateExtern)
            marker->addSym(defined, shard);
        }
        marker->flush(shard);
      });
  // -u symbols
  for (Symbol *sym : config->explicitUndefineds)
    marker->addSym(sym);
  // local symbols explicitly marked .no_dead_strip
  ArrayRef<InputFile *> files = inputFiles.getArrayRef();
  parallelForChunks(files, 1, [&](size_t shard, ArrayRef<InputFile *> chunk) {
    for (InputFile *file : chunk)
      if (auto *objFile = dyn_cast<ObjFile>(file))
        for (Symbol *sym : objFile->symbols)
          if (auto *defined = dyn_cast_or_null<Defined>(sym))
            if (!defined->isExternal() && defined->noDeadStrip)
              marker->addSym(defined, shard);
    marker->flush(shard);
  });
  if (auto *stubBinder =
          dyn_cast_or_null<DylibSymbol>(symtab->find("dyld_stub_binder")))
    marker->addSym(stubBinder);
  parallelForChunks(
      ArrayRef(inputSections), 256,
      [&](size_t shard, ArrayRef<ConcatInputSection *> chunk) {
        for (ConcatInputSection *isec : chunk) {
          // Sections marked no_dead_strip
          if (isec->getFlags() & S_ATTR_NO_DEAD_STRIP) {
            marker->enqueue(isec, 0, shard);
            continue;
          }

          // mod_init_funcs, mod_term_funcs sections
          if (sectionType(isec->getFlags()) == S_MOD_INIT_FUNC_POINTERS ||
              sectionType(isec->getFlags()) == S_MOD_TERM_FUNC_POINTERS) {
            assert(!config->emitInitOffsets ||
                   sectionType(isec->getFlags()) != S_MOD_INIT_FUNC_POINTERS);
            marker->enqueue(isec, 0, shard);
          }
        }
        marker->flush(shard);
      });

  for (ConcatInputSection *isec : in.initOffsets->inputs())
    marker->enqueue(isec, 0);

  marker->markTransitively();
}

} // namespace lld::macho
