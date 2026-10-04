//===- ConcatOutputSection.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ConcatOutputSection.h"
#include "Config.h"
#include "OutputSegment.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

#include <chrono>
#include <limits>
#include <thread>

#ifdef __APPLE__
#include <condition_variable>
#include <mutex>
#endif

using namespace llvm;
using namespace llvm::MachO;
using namespace lld;
using namespace lld::macho;

MapVector<NamePair, ConcatOutputSection *> macho::concatOutputSections;

// Branch-range extension can be implemented in two ways, either through ...
//
// (1) Branch islands: Single branch instructions (also of limited range),
//     that might be chained in multiple hops to reach the desired
//     destination. On ARM64, as 16 branch islands are needed to hop between
//     opposite ends of a 2 GiB program. LD64 uses branch islands exclusively,
//     even when it needs excessive hops.
//
// (2) Thunks: Instruction(s) to load the destination address into a scratch
//     register, followed by a register-indirect branch. Thunks are
//     constructed to reach any arbitrary address, so need not be
//     chained. Although thunks need not be chained, a program might need
//     multiple thunks to the same destination distributed throughout a large
//     program so that all call sites can have one within range.
//
// The optimal approach is to mix islands for destinations within two hops,
// and use thunks for destinations at greater distance. For now, we only
// implement thunks. TODO: Adding support for branch islands!

DenseMap<ThunkKey, ThunkInfo, ThunkMapKeyInfo> lld::macho::thunkMap;

// Determine whether we need thunks, which depends on the target arch -- RISC
// (i.e., ARM) generally does because it has limited-range branch/call
// instructions, whereas CISC (i.e., x86) generally doesn't. RISC only needs
// thunks for programs so large that branch source & destination addresses
// might differ more than the range of branch instruction(s).
bool TextOutputSection::needsThunks() const {
  if (!target->usesThunks())
    return false;
  // FIXME: It is not enough to just estimate the size of this section. We
  // should compute parent->needsThunks by estimating the size of all __text
  // sections. See https://github.com/llvm/llvm-project/issues/195387
  uint64_t isecAddr = addr;
  for (size_t i = 0, e = inputs.size(); i != e; ++i) {
    ConcatInputSection *isec = inputs[i];
    isecAddr = alignToPowerOf2(isecAddr, isec->align) + isec->getSize();
    // The thunk walk in finalize() names a not-yet-final branch target by its
    // index here. This loop already has every input's line in cache.
    isec->icfEqClass[0] = i;
  }
  // The walk stores these indices as uint32_t and reserves UINT32_MAX.
  assert(inputs.size() < UINT32_MAX);
  // Other sections besides __text might be small enough to pass this
  // test but nevertheless need thunks for calling into other sections.
  // An imperfect heuristic to use in this case is that if a section
  // we've already processed in this segment needs thunks, so do the
  // rest.
  bool needsThunks = parent && parent->needsThunks;

  // Calculate the total size of all branch target sections
  uint64_t branchTargetsSize = in.stubs->getSize();

  // Add the size of __objc_stubs section if it exists
  if (in.objcStubs && in.objcStubs->isNeeded())
    branchTargetsSize += in.objcStubs->getSize();

  if (!needsThunks &&
      isecAddr - addr + branchTargetsSize <=
          std::min(target->backwardBranchRange, target->forwardBranchRange))
    return false;
  // Yes, this program is large enough to need thunks.
  if (parent)
    parent->needsThunks = true;
  return true;
}

void ConcatOutputSection::finalizeOne(ConcatInputSection *isec) {
  size = alignToPowerOf2(size, isec->align);
  fileSize = alignToPowerOf2(fileSize, isec->align);
  isec->outSecOff = size;
  isec->isFinal = true;
  size += isec->getSize();
  fileSize += isec->getFileSize();
}

void ConcatOutputSection::finalizeContents() {
  for (ConcatInputSection *isec : inputs)
    finalizeOne(isec);
}

bool TextOutputSection::isTargetKnownInRange(const ConcatInputSection &isec,
                                             const Relocation &r) const {
  uint64_t callVA = isec.getVA() + r.getOffset();
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  auto *funcSym = cast<Symbol *>(r.getReferent());
  uint64_t funcVA =
      resolveSymbolOffsetVA(funcSym, r.getType(), r.getAddend());
  // Check if the referent is reachable with a simple call instruction.
  return lowVA <= funcVA && funcVA <= highVA;
}

Defined *TextOutputSection::getThunkInRange(const ConcatInputSection &isec,
                                            const Relocation &r,
                                            const ThunkInfo &thunkInfo) const {
  assert(!isTargetKnownInRange(isec, r));
  return getThunkInRange(isec.getVA() + r.getOffset(), thunkInfo);
}

Defined *TextOutputSection::getThunkInRange(uint64_t callVA,
                                            const ThunkInfo &thunkInfo) const {
  if (!thunkInfo.sym)
    return nullptr;
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  uint64_t thunkVA = thunkInfo.isec->getVA();
  if (lowVA <= thunkVA && thunkVA <= highVA)
    return thunkInfo.sym;
  return nullptr;
}

void TextOutputSection::updateBranchTargetToThunk(Relocation &r,
                                                  Defined *thunk) {
  // The thunk itself bakes in the addend, so the call-site reloc must
  // branch to the thunk start with no extra offset.
  r.setTarget(thunk, 0);
  ++thunkCallCount;
}

void TextOutputSection::createThunk(const ConcatInputSection &isec,
                                    Relocation &r, ThunkInfo &thunkInfo,
                                    function_ref<void()> beforeSymbolReplacement) {
  assert(getThunkInRange(isec, r, thunkInfo) == nullptr);
  assert(isec.isFinal);
  uint64_t highVA =
      isec.getVA() + r.getOffset() + target->forwardBranchRange;
  if (addr + size > highVA) {
    // There were too many consecutive branch instructions for `slop`
    // below. If you hit this: For the current algorithm, just bumping up
    // slop below and trying again is probably simplest. (See also PR51578
    // comment 5).
    fatal(Twine(__FUNCTION__) +
          ": FIXME: thunk range overrun. Consider increasing the "
          "slop-scale with `--slop-scale=<unsigned_int>`.");
  }
  thunkInfo.isec = makeSyntheticInputSection(isec.getSegName(), isec.getName());
  thunkInfo.isec->parent = this;
  assert(thunkInfo.isec->live);

  std::string addendSuffix;
  if (r.getAddend() != 0)
    addendSuffix = "+" + std::to_string(r.getAddend());
  size_t thunkSize = target->thunkSize;
  auto *funcSym = cast<Symbol *>(r.getReferent());
  StringRef thunkName =
      saver().save(funcSym->getName() + addendSuffix + ".thunk." +
                   std::to_string(thunkInfo.sequence++));
  bool inSymtab =
      !isa<Defined>(funcSym) || cast<Defined>(funcSym)->isExternal();
  if (inSymtab) {
    // addDefined() would replace an input symbol of the same name in place and
    // silently redirect every branch to it. Lazy symbols are not definitions
    // yet and are replaced harmlessly.
    if (Symbol *clash = symtab->find(thunkName)) {
      if (isa<Defined>(clash) || isa<DylibSymbol>(clash)) {
        error("linker-generated thunk name '" + thunkName +
              "' collides with " + toString(*clash) + " from " +
              toString(clash->getFile()));
        // The link fails; keep the input symbol, so that the branches the walk
        // has already classified against it stay valid, and give the thunk a
        // symbol of its own.
        inSymtab = false;
      } else {
        if (beforeSymbolReplacement)
          beforeSymbolReplacement();
        thunkReplacedSymbol = true;
      }
    }
  }
  if (inSymtab) {
    thunkInfo.sym = symtab->addDefined(
        thunkName, /*file=*/nullptr, thunkInfo.isec, /*value=*/0, thunkSize,
        /*isWeakDef=*/false, /*isPrivateExtern=*/true,
        /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
        /*isWeakDefCanBeHidden=*/false);
  } else {
    thunkInfo.sym = make<Defined>(
        thunkName, /*file=*/nullptr, thunkInfo.isec, /*value=*/0, thunkSize,
        /*isWeakDef=*/false, /*isExternal=*/false, /*isPrivateExtern=*/true,
        /*includeInSymtab=*/true, /*isReferencedDynamically=*/false,
        /*noDeadStrip=*/false, /*isWeakDefCanBeHidden=*/false);
  }
  thunkInfo.sym->used = true;
  target->populateThunk(thunkInfo.isec, funcSym, r.getAddend());
  updateBranchTargetToThunk(r, thunkInfo.sym);
  finalizeOne(thunkInfo.isec);
  thunks.push_back(thunkInfo.isec);
}

std::optional<uint64_t>
TextOutputSection::estimateStubsEndVA(unsigned numPotentialThunks) const {
  if (!parent)
    return std::nullopt;

  auto sections =
      ArrayRef(parent->getSections())
          .drop_until([&](const OutputSection *osec) { return osec == this; });

  // Walk backwards to find the last stubs section
  while (!sections.empty()) {
    auto *osec = sections.back();
    if (osec->isNeeded() && (osec == in.stubs || osec == in.objcStubs))
      break;
    sections.consume_back();
  }
  if (sections.empty())
    return std::nullopt;

  assert(inputs.empty() || inputs.back()->isFinal);
  uint64_t estimatedStubsEnd =
      addr + size + numPotentialThunks * target->thunkSize;
  for (auto *osec : sections) {
    if (osec == this)
      continue;
    if (!osec->isNeeded())
      continue;
    // Check if we will emit any more sections before the last stubs section
    if (osec != in.stubs && osec != in.stubHelper && osec != in.objcStubs)
      return std::nullopt;
    estimatedStubsEnd =
        alignToPowerOf2(estimatedStubsEnd, osec->align) + osec->getSize();
  }
  return estimatedStubsEnd;
}

bool TextOutputSection::isTargetStubsAndInRange(
    uint64_t callVA, const Relocation &r, bool addendIsZero, bool inStubs,
    bool inObjCStubs, std::optional<uint64_t> estimatedStubsEnd) const {
  if (!estimatedStubsEnd.has_value())
    return false;
  // Every test here is a pure function of r, its referent and callVA. Test the
  // recorded bits and the range first, so that most branches never touch the
  // symbol or its name again.
  if (!addendIsZero)
    return false;
  uint64_t highVA = callVA + target->forwardBranchRange;
  if (*estimatedStubsEnd > highVA)
    return false;
  if (inStubs || inObjCStubs)
    return true;
  return in.objcStubs && in.objcStubs->isNeeded() &&
         ObjCStubsSection::isObjCStubSymbol(cast<Symbol *>(r.getReferent()));
}

namespace {
// The thunk walk's view of one branch relocation, taken when its call site is
// final. Its fate is a function of these fields, of the final offsets of the
// sections the walk has passed, and of thunkMap.
//
// A target in this section that is not final yet is kept as (targetIdx,
// dest = value + addend) and becomes final exactly when the walk passes
// inputs[targetIdx]. Every other target (an earlier section of this output
// section, another output section, a stub, an absolute symbol) has a VA that
// cannot change for the rest of the walk, so resolveSymbolOffsetVA() is
// evaluated once and kept in `dest`.
constexpr uint32_t fixedTarget = UINT32_MAX;
enum : uint32_t {
  // While the target is not final, Defined::getVA() returns outOfRangeVA; this
  // bit caches whether outOfRangeVA + addend is in range of the call.
  notFinalInRange = 1,
  addendIsZero = 2,
  inStubs = 4,
  // The referent is defined in __objc_stubs, so it is an Objective-C stub
  // symbol (ObjCStubsSection::addEntry() makes exactly those).
  inObjCStubs = 8,
};

struct PendingBranch {
  Relocation *r;
  uint64_t callVA;
  uint64_t dest;
  uint32_t targetIdx;
  uint32_t callerIdx;
  // ThunkMapKeyInfo::getHashValue(ThunkKey(*r)), computed when the branch was
  // first looked up in thunkMap.
  uint32_t keyHash;
  uint32_t flags;
};
static_assert(sizeof(PendingBranch) == 40 || sizeof(void *) != 8);

struct DeferredRedirect {
  Relocation *r;
  Defined *thunk;
  uint64_t callVA;
  uint64_t dest;
  uint32_t targetIdx;
  uint32_t flags;
};

inline void prefetchLine(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p);
#else
  (void)p;
#endif
}

// The FIFO of branches that may still need a thunk: a power-of-two ring that
// supports looking ahead of the front (to prefetch what the front will read).
class PendingQueue {
public:
  bool empty() const { return count == 0; }
  size_t size() const { return count; }
  PendingBranch &front() { return buf[head]; }
  const PendingBranch &operator[](size_t i) const {
    return buf[(head + i) & mask];
  }
  void pop_front() {
    head = (head + 1) & mask;
    --count;
  }
  void push_back(const PendingBranch &b) {
    if (count == cap)
      grow();
    buf[(head + count) & mask] = b;
    ++count;
  }

private:
  void grow() {
    size_t newCap = cap ? cap * 2 : 4096;
    // Not value-initialized: pages are touched only as entries are written.
    std::unique_ptr<PendingBranch[]> n(new PendingBranch[newCap]);
    for (size_t i = 0; i != count; ++i)
      n[i] = buf[(head + i) & mask];
    buf = std::move(n);
    cap = newCap;
    mask = newCap - 1;
    head = 0;
  }
  std::unique_ptr<PendingBranch[]> buf;
  size_t cap = 0, mask = 0, head = 0, count = 0;
};

// finalOff[i] is inputs[i]->outSecOff once the walk has passed input i. The
// front and the end loops read it at random, so it is kept 32 bits wide (half
// the cache footprint) unless an offset ever exceeds that.
class FinalOffsets {
public:
  explicit FinalOffsets(size_t n) : narrow(n) {}
  void set(size_t i, uint64_t off) {
    if (LLVM_LIKELY(!isWide && off <= UINT32_MAX))
      narrow[i] = off;
    else
      setWide(i, off);
  }
  uint64_t operator[](size_t i) const { return isWide ? wide[i] : narrow[i]; }
  void prefetch(size_t i) const {
    prefetchLine(isWide ? static_cast<const void *>(&wide[i]) : &narrow[i]);
  }

private:
  void setWide(size_t i, uint64_t off) {
    if (!isWide) {
      wide.assign(narrow.begin(), narrow.end());
      narrow = std::vector<uint32_t>();
      isWide = true;
    }
    wide[i] = off;
  }
  std::vector<uint32_t> narrow;
  std::vector<uint64_t> wide;
  bool isWide = false;
};

// The branch relocations of the next few inputs, in scan order, and how many
// each input has.
class ScanAhead {
public:
  static constexpr size_t numCounts = 64;
  uint32_t counts[numCounts] = {};
  size_t size() const { return count; }
  Relocation *operator[](size_t i) const { return buf[(head + i) & mask]; }
  Relocation *pop() {
    Relocation *r = buf[head];
    head = (head + 1) & mask;
    --count;
    return r;
  }
  void push(Relocation *r) {
    if (count == buf.size())
      grow();
    buf[(head + count) & mask] = r;
    ++count;
  }

private:
  void grow() {
    std::vector<Relocation *> n(buf.empty() ? 1024 : buf.size() * 2);
    for (size_t i = 0; i != count; ++i)
      n[i] = buf[(head + i) & mask];
    buf = std::move(n);
    mask = buf.size() - 1;
    head = 0;
  }
  std::vector<Relocation *> buf;
  size_t mask = 0, head = 0, count = 0;
};

bool isVAInBranchRange(uint64_t callVA, uint64_t va) {
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  return lowVA <= va && va <= highVA;
}

// Immutable branch facts. A TEXT destination remains an index even when it
// points backwards: only the consumer reads finality and final offsets.
struct BranchRecord {
  Relocation *r;
  uint64_t dest;
  uint32_t targetIdx;
  uint32_t keyHash;
  uint32_t flags;
  uint32_t offset;
  // Keep the normalized key, not just its hash: reconstructing it on the
  // consumer would repeat the symbol -> canonical section chase.
  ThunkKey key;
};
static_assert(sizeof(BranchRecord) == 56 || sizeof(void *) != 8);

// Bounded, section-ordered producer/consumer stream. State is per chunk (not
// per ring slot), so a late worker cannot claim a slot after its generation
// has been consumed and reused. Slot ownership transfers with release/acquire.
class BranchStream {
public:
  static constexpr size_t chunkSize = 4096, ringSize = 16;
  struct Chunk {
    std::vector<BranchRecord> records;
    uint32_t first[chunkSize + 1];
  };
  using Clock = std::chrono::steady_clock;

  BranchStream(TextOutputSection *osec, const InputSection *objcStubsIsec)
      : osec(osec), objcStubsIsec(objcStubsIsec),
        numChunks(divideCeil(osec->inputs.size(), chunkSize)),
        states(new std::atomic<unsigned char>[numChunks]),
        slots(new Chunk[std::min(numChunks, ringSize)]),
        tracing(timeTraceProfilerEnabled()) {
    for (size_t c = 0; c != numChunks; ++c)
      states[c].store(unclaimed, std::memory_order_relaxed);
    tasks = std::make_unique<parallel::TaskGroup>();
    assert(tasks->isParallel());
    size_t n = parallel::getThreadCount();
    size_t producers = std::min(numChunks, n > 2 ? n - 2 : 1);
    for (size_t i = 0; i != producers; ++i)
      tasks->spawn([this] { run(); });
  }
  ~BranchStream() { stop(); }

  Chunk &get(size_t c) {
    unsigned char expected = unclaimed;
    if (states[c].compare_exchange_strong(expected, producing,
                                          std::memory_order_acq_rel)) {
      fill(c);
      ++chunksInline;
      publish(states[c], ready);
    } else if (expected != ready) {
      auto start = tracing ? Clock::now() : Clock::time_point();
      waitUntil([&] {
        return states[c].load(std::memory_order_acquire) == ready;
      });
      if (tracing)
        consumerWaitNs += elapsed(start);
    }
    return slots[c % ringSize];
  }
  void release(size_t c) {
    // Keep the terminal state so a worker holding an old ticket skips it.
    states[c].store(consumed, std::memory_order_release);
    publish(released, c + 1);
  }
  void stop() {
    publish(stopped, true);
    tasks.reset();
  }
  void report() const {
    timeTraceAddInstantEvent("Thunk walk B", [&] {
      return "chunksByWorkers=" + std::to_string(chunksByWorkers.load()) +
             " chunksInline=" + std::to_string(chunksInline) +
             " consumerWaitMs=" + std::to_string(consumerWaitNs / 1e6) +
             " producerMs=" + std::to_string(producerNs.load() / 1e6);
    });
  }

private:
  enum : unsigned char { unclaimed, producing, ready, consumed };
  static uint64_t elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                               start).count();
  }
  // On Darwin sched_yield enters swtch_pri. Producers can spend the whole
  // serial thunk walk waiting for ring space, so repeating that syscall burns
  // kernel CPU. Spin briefly, then park. Publishing under the same mutex as
  // the predicate check prevents a notification from racing with wait entry.
  template <class Predicate> void waitUntil(Predicate done) {
    unsigned spins = 0;
#ifdef __APPLE__
    while (!done()) {
      if (++spins == 1000) {
        std::unique_lock<std::mutex> lock(waitMutex);
        changed.wait(lock, done);
        return;
      }
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }
#else
    while (!done()) {
      if (++spins >= 1000)
        std::this_thread::yield();
      else
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
#endif
  }
  template <class T, class U> void publish(std::atomic<T> &state, U value) {
#ifdef __APPLE__
    {
      std::lock_guard<std::mutex> lock(waitMutex);
      state.store(value, std::memory_order_release);
    }
    // Waiters have different predicates (chunk readiness, ring space, stop).
    // Waking just one could leave the next eligible ticket asleep forever.
    changed.notify_all();
#else
    state.store(value, std::memory_order_release);
#endif
  }
  void run() {
    while (!stopped.load(std::memory_order_acquire)) {
      size_t c = nextChunk.fetch_add(1, std::memory_order_relaxed);
      if (c >= numChunks)
        return;
      waitUntil([&] {
        return c < released.load(std::memory_order_acquire) + ringSize ||
               stopped.load(std::memory_order_acquire);
      });
      if (stopped.load(std::memory_order_acquire))
        return;
      unsigned char expected = unclaimed;
      if (!states[c].compare_exchange_strong(expected, producing,
                                             std::memory_order_acq_rel))
        continue;
      auto start = tracing ? Clock::now() : Clock::time_point();
      fill(c);
      if (tracing)
        producerNs.fetch_add(elapsed(start), std::memory_order_relaxed);
      chunksByWorkers.fetch_add(1, std::memory_order_relaxed);
      publish(states[c], ready);
    }
  }
  void fill(size_t c) {
    Chunk &chunk = slots[c % ringSize];
    chunk.records.clear();
    size_t begin = c * chunkSize;
    size_t end = std::min(begin + chunkSize, osec->inputs.size());
    for (size_t i = begin; i != end; ++i) {
      chunk.first[i - begin] = chunk.records.size();
      for (Relocation &r : reverse(osec->inputs[i]->relocs)) {
        if (!target->hasAttr(r.getType(), RelocAttrBits::BRANCH))
          continue;
        auto *sym = cast<Symbol *>(r.getReferent());
        int64_t addend = r.getAddend();
        bool stub = sym->isInStubs();
        ThunkKey key(r);
        BranchRecord rec{&r, 0, fixedTarget,
                         ThunkMapKeyInfo::getHashValue(key),
                         (addend == 0 ? addendIsZero : 0u) |
                             (stub ? inStubs : 0u), r.getOffset(), key};
        if (auto *d = dyn_cast<Defined>(sym);
            d && (addend != 0 || !stub) && !d->isAbsolute()) {
          InputSection *isec = d->isec();
          if (isec->parent == osec) {
            auto *text = cast<ConcatInputSection>(isec);
            rec.targetIdx = text->icfEqClass[0];
            if (rec.targetIdx < osec->inputs.size() &&
                osec->inputs[rec.targetIdx] == text) {
              rec.dest = d->value + addend;
              chunk.records.push_back(rec);
              continue;
            }
            // A generated thunk can become a target after a lazy-symbol
            // replacement. Its creation joined the producers first.
            assert(stopped.load(std::memory_order_acquire));
            rec.targetIdx = fixedTarget;
          }
          if (isec == objcStubsIsec)
            rec.flags |= inObjCStubs;
        }
        // Other output sections cannot change while this walk is running.
        rec.dest = resolveSymbolOffsetVA(sym, r.getType(), addend);
        chunk.records.push_back(rec);
      }
    }
    chunk.first[end - begin] = chunk.records.size();
  }

  TextOutputSection *osec;
  const InputSection *objcStubsIsec;
  size_t numChunks;
  std::unique_ptr<std::atomic<unsigned char>[]> states;
  std::unique_ptr<Chunk[]> slots;
  bool tracing;
  alignas(64) std::atomic<size_t> nextChunk{0};
  alignas(64) std::atomic<size_t> released{0};
  alignas(64) std::atomic<bool> stopped{false};
  std::atomic<uint64_t> producerNs{0};
  std::atomic<size_t> chunksByWorkers{0};
  size_t chunksInline = 0;
  uint64_t consumerWaitNs = 0;
  std::unique_ptr<parallel::TaskGroup> tasks;
#ifdef __APPLE__
  std::mutex waitMutex;
  std::condition_variable changed;
#endif
};

// Prefetch distances, in queue entries, for the relocation (and the final
// offset of the target) and, closer to use, for the relocation's referent.
constexpr size_t farAhead = 16;
constexpr size_t nearAhead = 8;

void prefetchReferent(const Relocation *r) {
  if (auto *sym = dyn_cast_if_present<Symbol *>(r->getReferent()))
    prefetchLine(sym);
}
} // namespace

void TextOutputSection::finalize() {
  if (!needsThunks()) {
    for (ConcatInputSection *isec : inputs)
      finalizeOne(isec);
    return;
  }
  TimeTraceScope timeScope("Thunk walk");

  // This is one forward pass over `inputs`. Each branch is resolved once into a
  // flat record when its section is finalized; afterwards the walk reads the
  // record instead of chasing relocation -> symbol -> section again. Every
  // decision below is taken at the same point, in the same order and from the
  // same values as the pointer-chasing formulation (isTargetKnownInRange()),
  // so the output is unchanged. What remains is memory latency, so the loops
  // prefetch what the records a few entries ahead will read.
  const size_t numInputs = inputs.size();
  FinalOffsets finalOff(numInputs);
  const InputSection *objcStubsIsec = in.objcStubs ? in.objcStubs->isec
                                                   : nullptr;

  std::unique_ptr<BranchStream> stream;
  if (parallel::strategy.ThreadsRequested != 1 &&
      parallel::getThreadCount() > 1 && numInputs)
    stream = std::make_unique<BranchStream>(this, objcStubsIsec);
  // A lazy/undefined symbol can be overwritten by a generated thunk name.
  // Join before that write; subsequent records are re-resolved on the consumer.
  auto stopProducers = [&] {
    if (stream)
      stream->stop();
  };

  // Whether the branch's target is within range, given that the first
  // `numFinal` inputs are final.
  auto isInRange = [&](uint64_t callVA, uint64_t tgt, uint32_t targetIdx,
                       uint32_t flags, size_t numFinal) {
    if (targetIdx == fixedTarget)
      return isVAInBranchRange(callVA, tgt);
    if (targetIdx < numFinal)
      return isVAInBranchRange(callVA, addr + finalOff[targetIdx] + tgt);
    return (flags & notFinalInRange) != 0;
  };
  auto makePending = [&](uint32_t callerIdx, Relocation &r, uint64_t callVA) {
    auto *funcSym = cast<Symbol *>(r.getReferent());
    int64_t addend = r.getAddend();
    bool symInStubs = funcSym->isInStubs();
    PendingBranch b{&r,
                    callVA,
                    0,
                    fixedTarget,
                    callerIdx,
                    0,
                    (addend == 0 ? addendIsZero : 0u) |
                        (symInStubs ? inStubs : 0u)};
    // Mirrors resolveSymbolOffsetVA(): a branch with an addend, or to a symbol
    // without a stub, resolves through Defined::getVA().
    if (auto *d = dyn_cast<Defined>(funcSym);
        d && (addend != 0 || !symInStubs) && !d->isAbsolute()) {
      InputSection *tisec = d->isec();
      if (!tisec->isFinal && tisec->parent == this) {
        // A live, not yet final section of this output section is in
        // `inputs` after the caller; needsThunks() numbered it.
        auto *cisec = cast<ConcatInputSection>(tisec);
        b.targetIdx = cisec->icfEqClass[0];
        assert(b.targetIdx < numInputs && inputs[b.targetIdx] == cisec);
        b.dest = d->value + addend;
        if (isVAInBranchRange(callVA, TargetInfo::outOfRangeVA + addend))
          b.flags |= notFinalInRange;
        return b;
      }
      if (tisec == objcStubsIsec)
        b.flags |= inObjCStubs;
    }
    b.dest = resolveSymbolOffsetVA(funcSym, r.getType(), addend);
    return b;
  };
  // A queued branch's key hash is computed once, at its first lookup. Its key
  // can change only if createThunk() replaced its referent in place (a lazy or
  // undefined symbol with a thunk's name); then hash afresh.
  auto lookupKey = [&](const ThunkKey &key, uint32_t hash) {
    return HashedThunkKey{key, LLVM_UNLIKELY(thunkReplacedSymbol)
                                   ? ThunkMapKeyInfo::getHashValue(key)
                                   : hash};
  };
  auto findThunk = [&](const ThunkKey &key, uint32_t hash) {
    return thunkMap.find_as(lookupKey(key, hash));
  };

  // Branches whose target sections are out of range or have not yet been
  // finalized. We may need to emit thunks for them.
  PendingQueue branchesToProcess;
  // Branches whose targets have not yet be finalized, but a thunk for that
  // target exists. We defer processing these branches because it's possible we
  // can still direct call to their targets after they have all been finalized.
  // Kept as two lists, each in ascending call address order: those found by
  // the scan, and those found when they reached the front of the queue.
  std::vector<DeferredRedirect> deferredAtScan, deferredAtFront;

  const uint64_t slop = config->slopScale * target->thunkSize;
  // The front of the queue waits until it is forced or its target becomes
  // final. While it waits nothing it depends on changes: thunks are created
  // only for the front, a fixed target's VA is constant, and a target in this
  // section becomes final only when the walk passes it. So a front is
  // evaluated once when it becomes the front (`frontChecked`), and again only
  // at its wake index `frontWake`, the first input after its target.
  bool frontChecked = false;
  size_t frontWake = 0;
  // Pops come in bursts once a blocking front resolves. `popped` counts pops;
  // entries up to (absolute position) farNext and nearNext have been
  // prefetched. After a wait the window is reissued, since a long wait has
  // evicted it.
  size_t popped = 0, farNext = 0, nearNext = 0;
  bool windowStale = true;
  // The scan below visits each input's branch relocations and reads each
  // one's referent and, for a definition, its section: dependent misses on
  // scattered memory (one relocation vector per input). A look-ahead pass
  // collects the branch relocations in scan order about `referentAhead`
  // branches early and prefetches their referents; the scan then reads that
  // list instead of all relocations, prefetching the referent's section
  // `sectionAhead` branches early. Relocations are prefetched `relocsAhead`
  // inputs before they are collected. Nothing the look-ahead reads changes
  // before the scan reaches it: the walk only rewrites relocations of inputs
  // it has passed.
  constexpr size_t relocsAhead = 8, referentAhead = 48, sectionAhead = 16;
  ScanAhead ahead;
  size_t collectNext = 0;
  auto prefetchRelocs = [&](size_t j) {
    if (j >= numInputs)
      return;
    const auto &relocs = inputs[j]->relocs;
    const char *p = reinterpret_cast<const char *>(relocs.data());
    const char *e =
        reinterpret_cast<const char *>(relocs.data() + relocs.size());
    for (; p < e; p += 64)
      prefetchLine(p);
  };
  auto collectBranches = [&](size_t j) {
    uint32_t n = 0;
    for (Relocation &r : reverse(inputs[j]->relocs)) {
      if (!target->hasAttr(r.getType(), RelocAttrBits::BRANCH))
        continue;
      if (auto *sym = dyn_cast_if_present<Symbol *>(r.getReferent()))
        prefetchLine(sym);
      ahead.push(&r);
      ++n;
    }
    ahead.counts[j % ScanAhead::numCounts] = n;
  };
  if (!stream)
    for (size_t j = 0; j != relocsAhead; ++j)
      prefetchRelocs(j);
  BranchStream::Chunk *chunk = nullptr;
  for (size_t idx = 0; idx != numInputs; ++idx) {
    ConcatInputSection *isec = inputs[idx];
    if (stream && idx % BranchStream::chunkSize == 0)
      chunk = &stream->get(idx / BranchStream::chunkSize);
    while (!stream && collectNext != numInputs &&
           (collectNext <= idx ||
            (ahead.size() < referentAhead &&
             collectNext - idx < ScanAhead::numCounts))) {
      prefetchRelocs(collectNext + relocsAhead);
      collectBranches(collectNext++);
    }
    auto popFront = [&] {
      branchesToProcess.pop_front();
      ++popped;
      if (windowStale) {
        farNext = popped + 1;
        nearNext = popped + 1 + nearAhead;
        windowStale = false;
      }
      farNext = std::max(farNext, popped + 1);
      nearNext = std::max(nearNext, popped + 1);
      size_t avail = branchesToProcess.size();
      for (size_t end = popped + std::min(avail, farAhead + 1);
           farNext < end; ++farNext) {
        const PendingBranch &e = branchesToProcess[farNext - popped];
        if (e.targetIdx == fixedTarget)
          prefetchLine(e.r);
        else if (e.targetIdx < idx)
          finalOff.prefetch(e.targetIdx);
      }
      for (size_t end = popped + std::min(avail, nearAhead + 1);
           nearNext < end; ++nearNext) {
        const PendingBranch &e = branchesToProcess[nearNext - popped];
        if (e.targetIdx == fixedTarget)
          prefetchReferent(e.r);
      }
    };
    while (!branchesToProcess.empty()) {
      PendingBranch &b = branchesToProcess.front();
      if (!frontChecked || idx >= frontWake) {
        if (isInRange(b.callVA, b.dest, b.targetIdx, b.flags, idx)) {
          popFront();
          frontChecked = false;
          continue;
        }
        if (!frontChecked) {
          ThunkKey key(*b.r);
          auto it = findThunk(key, b.keyHash);
          if (it != thunkMap.end()) {
            if (auto *thunk = getThunkInRange(b.callVA, it->second)) {
              deferredAtFront.push_back({b.r, thunk, b.callVA, b.dest,
                                         b.targetIdx, b.flags});
              popFront();
              continue;
            }
          }
          frontChecked = true;
        }
        frontWake = b.targetIdx != fixedTarget && b.targetIdx >= idx
                        ? size_t(b.targetIdx) + 1
                        : std::numeric_limits<size_t>::max();
      }
      uint64_t highVA = b.callVA + target->forwardBranchRange;
      uint64_t nextEnd =
          alignToPowerOf2(addr + size, isec->align) + isec->getSize();
      // If we were to emit this section, would we have enough space for more
      // thunks? If we do, then we can delay processing this thunk so we may
      // finalize more potencial target sections. Otherwise we must emit thunks
      // until we have enough space.
      if (nextEnd + slop <= highVA) {
        windowStale = true;
        break;
      }

      ThunkKey key(*b.r);
      auto &thunkInfo =
          thunkMap.insert_as({key, ThunkInfo()}, lookupKey(key, b.keyHash))
              .first->second;
      createThunk(*inputs[b.callerIdx], *b.r, thunkInfo, stopProducers);
      frontChecked = false;
      popFront();
    }
    finalizeOne(isec);
    finalOff.set(idx, isec->outSecOff);

    // Process relocs by ascending address, i.e., ascending offset within isec
    // FIXME: This property does not hold for object files produced by ld64's
    // `-r` mode.
    assert(is_sorted(isec->relocs, [](Relocation &a, Relocation &b) {
      return a.getOffset() > b.getOffset();
    }));
    const uint64_t isecVA = addr + isec->outSecOff;
    uint32_t recordIdx = stream ? chunk->first[idx % BranchStream::chunkSize] : 0;
    uint32_t numBranches = stream
        ? chunk->first[idx % BranchStream::chunkSize + 1] - recordIdx
        : ahead.counts[idx % ScanAhead::numCounts];
    for (uint32_t n = numBranches; n; --n) {
      if (!stream && ahead.size() > sectionAhead) {
        auto *sym = dyn_cast_if_present<Symbol *>(
            ahead[sectionAhead]->getReferent());
        if (auto *d = dyn_cast_if_present<Defined>(sym))
          prefetchLine(d->originalIsec);
      }
      const BranchRecord *rec = stream ? &chunk->records[recordIdx++] : nullptr;
      Relocation &r = rec ? *rec->r : *ahead.pop();
      PendingBranch b;
      if (rec && !thunkReplacedSymbol) {
        b = {&r, isecVA + rec->offset, rec->dest, rec->targetIdx,
             static_cast<uint32_t>(idx), rec->keyHash, rec->flags};
        if (b.targetIdx != fixedTarget) {
          if (b.targetIdx <= idx) {
            // Match makePending: a finalized target stays a fixed VA in the
            // FIFO, avoiding another finalOff lookup at the front/end.
            b.dest += addr + finalOff[b.targetIdx];
            b.targetIdx = fixedTarget;
          } else if (isVAInBranchRange(
                         b.callVA, TargetInfo::outOfRangeVA + rec->key.addend)) {
            b.flags |= notFinalInRange;
          }
        }
      } else {
        b = makePending(idx, r, isecVA + r.getOffset());
      }
      assert(isInRange(b.callVA, b.dest, b.targetIdx, b.flags, idx + 1) ==
             isTargetKnownInRange(*isec, r));
      if (isInRange(b.callVA, b.dest, b.targetIdx, b.flags, idx + 1))
        continue;
      ThunkKey key = rec && !thunkReplacedSymbol ? rec->key : ThunkKey(r);
      if (!rec || thunkReplacedSymbol)
        b.keyHash = ThunkMapKeyInfo::getHashValue(key);
      auto it = findThunk(key, b.keyHash);
      if (it != thunkMap.end()) {
        if (auto *thunk = getThunkInRange(b.callVA, it->second)) {
          deferredAtScan.push_back(
              {b.r, thunk, b.callVA, b.dest, b.targetIdx, b.flags});
          continue;
        }
      }
      branchesToProcess.push_back(b);
    }
    if (stream && (idx % BranchStream::chunkSize == BranchStream::chunkSize - 1 ||
                   idx + 1 == numInputs))
      stream->release(idx / BranchStream::chunkSize);
  }
  stopProducers();
  if (stream)
    stream->report();

  // Drop the branches whose targets are now in range, keeping queue order.
  std::vector<PendingBranch> remaining;
  {
    size_t n = branchesToProcess.size();
    for (size_t i = 0; i != n; ++i) {
      if (i + farAhead < n) {
        const PendingBranch &e = branchesToProcess[i + farAhead];
        if (e.targetIdx != fixedTarget)
          finalOff.prefetch(e.targetIdx);
      }
      const PendingBranch &b = branchesToProcess[i];
      if (!isInRange(b.callVA, b.dest, b.targetIdx, b.flags, numInputs))
        remaining.push_back(b);
    }
    branchesToProcess = PendingQueue();
  }
  // Whatever each remaining branch's key lookup reads: the relocation, then
  // its referent.
  auto prefetchKeyAhead = [&](size_t i) {
    size_t n = remaining.size();
    if (i + farAhead < n)
      prefetchLine(remaining[i + farAhead].r);
    if (i + nearAhead < n)
      prefetchReferent(remaining[i + nearAhead].r);
  };

  // Count distinct unresolved branch targets that still lack an in-range thunk.
  // We use this as an upper bound on the number of thunks we may still create
  // when estimating where __stubs / __objc_stubs could end up.
  DenseSet<ThunkKey, ThunkMapKeyInfo> branchTargets;
  for (size_t i = 0, n = remaining.size(); i != n; ++i) {
    prefetchKeyAhead(i);
    const PendingBranch &b = remaining[i];
    ThunkKey thunkKey(*b.r);
    auto it = findThunk(thunkKey, b.keyHash);
    if (it == thunkMap.end() || !getThunkInRange(b.callVA, it->second))
      branchTargets.insert_as(thunkKey, lookupKey(thunkKey, b.keyHash));
  }

  auto estimatedStubsEnd = estimateStubsEndVA(branchTargets.size());
  // Whether isTargetStubsAndInRange() will have to read the referent's name.
  auto needsNameCheck = [&](uint64_t callVA, uint32_t flags) {
    return estimatedStubsEnd && (flags & addendIsZero) &&
           !(flags & (inStubs | inObjCStubs)) &&
           *estimatedStubsEnd <= callVA + target->forwardBranchRange;
  };

  // Resolve the deferred redirects. Each is independent of the others
  // (distinct relocations; thunkCallCount is a count), so the two lists are
  // merged into one ascending call address stream for the relocation writes.
  {
    auto prefetchDeferred = [&](const std::vector<DeferredRedirect> &v,
                                size_t i) {
      if (i + farAhead < v.size()) {
        const DeferredRedirect &e = v[i + farAhead];
        if (e.targetIdx != fixedTarget)
          finalOff.prefetch(e.targetIdx);
        prefetchLine(e.r);
      }
      if (i + nearAhead < v.size()) {
        const DeferredRedirect &e = v[i + nearAhead];
        if (needsNameCheck(e.callVA, e.flags))
          prefetchReferent(e.r);
      }
    };
    auto resolve = [&](const DeferredRedirect &d) {
      if (isInRange(d.callVA, d.dest, d.targetIdx, d.flags, numInputs))
        return;
      if (isTargetStubsAndInRange(d.callVA, *d.r, d.flags & addendIsZero,
                                  d.flags & inStubs, d.flags & inObjCStubs,
                                  estimatedStubsEnd))
        return;
      updateBranchTargetToThunk(*d.r, d.thunk);
    };
    size_t i = 0, j = 0;
    const size_t ni = deferredAtScan.size(), nj = deferredAtFront.size();
    while (i != ni || j != nj) {
      if (j == nj ||
          (i != ni && deferredAtScan[i].callVA <= deferredAtFront[j].callVA)) {
        prefetchDeferred(deferredAtScan, i);
        resolve(deferredAtScan[i++]);
      } else {
        prefetchDeferred(deferredAtFront, j);
        resolve(deferredAtFront[j++]);
      }
    }
  }

  for (size_t i = 0, n = remaining.size(); i != n; ++i) {
    prefetchKeyAhead(i);
    const PendingBranch &b = remaining[i];
    if (isTargetStubsAndInRange(b.callVA, *b.r, b.flags & addendIsZero,
                                b.flags & inStubs, b.flags & inObjCStubs,
                                estimatedStubsEnd))
      continue;
    ThunkKey key(*b.r);
    auto &thunkInfo =
        thunkMap.insert_as({key, ThunkInfo()}, lookupKey(key, b.keyHash))
            .first->second;
    if (auto *thunk = getThunkInRange(b.callVA, thunkInfo)) {
      updateBranchTargetToThunk(*b.r, thunk);
      continue;
    }
    createThunk(*inputs[b.callerIdx], *b.r, thunkInfo);
  }

  if (!thunks.empty())
    log(name + ": Created " + Twine(thunks.size()) + " (" +
        Twine(thunks.size() * target->thunkSize / 1024) +
        " KB) thunks and updated " + Twine(thunkCallCount) + " branch targets");
}

void ConcatOutputSection::writeTo(uint8_t *buf) const {
  for (ConcatInputSection *isec : inputs)
    isec->writeTo(buf + isec->outSecOff);
}

static std::optional<uint64_t>
getArm64RelocationWriteSize(const Relocation &reloc) {
  switch (reloc.getType()) {
  case ARM64_RELOC_UNSIGNED:
    if (config->emitChainedFixups)
      return 8;
    [[fallthrough]];
  case ARM64_RELOC_SUBTRACTOR:
  case ARM64_RELOC_POINTER_TO_GOT:
    if (reloc.getLength() == 2)
      return 4;
    if (reloc.getLength() == 3)
      return 8;
    return std::nullopt;
  case ARM64_RELOC_BRANCH26:
  case ARM64_RELOC_PAGE21:
  case ARM64_RELOC_GOT_LOAD_PAGE21:
  case ARM64_RELOC_TLVP_LOAD_PAGE21:
  case ARM64_RELOC_PAGEOFF12:
  case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
  case ARM64_RELOC_TLVP_LOAD_PAGEOFF12:
    return 4;
  default:
    return std::nullopt;
  }
}

static bool canWriteTextInputsInParallel(
    ArrayRef<ConcatInputSection *> inputs,
    ArrayRef<ConcatInputSection *> thunks, uint64_t outputFileSize) {
  if (config->arch() != AK_arm64 && config->arch() != AK_arm64e)
    return false;

  uint64_t previousEnd = 0;
  auto checkSpan = [&](const ConcatInputSection *isec) {
    uint64_t fileSize = isec->getFileSize();
    if (isec->outSecOff < previousEnd ||
        isec->outSecOff > outputFileSize ||
        fileSize > outputFileSize - isec->outSecOff)
      return false;
    previousEnd = isec->outSecOff + fileSize;
    return true;
  };

  size_t i = 0, ie = inputs.size();
  size_t t = 0, te = thunks.size();
  while (i < ie || t < te) {
    if (i < ie && (t == te || inputs[i]->empty() ||
                   inputs[i]->outSecOff < thunks[t]->outSecOff)) {
      const ConcatInputSection *isec = inputs[i++];
      if (!checkSpan(isec))
        return false;

      for (size_t relocIndex = 0; relocIndex < isec->relocs.size();
           ++relocIndex) {
        const Relocation &reloc = isec->relocs[relocIndex];
        if (target->hasAttr(reloc.getType(), RelocAttrBits::SUBTRAHEND)) {
          if (++relocIndex == isec->relocs.size())
            return false;
          const Relocation &minuend = isec->relocs[relocIndex];
          if (!target->hasAttr(minuend.getType(), RelocAttrBits::UNSIGNED) ||
              minuend.getOffset() != reloc.getOffset())
            return false;
        }

        uint64_t fileSize = isec->getFileSize();
        std::optional<uint64_t> writeSize =
            getArm64RelocationWriteSize(reloc);
        if (!writeSize || reloc.getOffset() > fileSize ||
            *writeSize > fileSize - reloc.getOffset())
          return false;
      }
      continue;
    }

    if (!checkSpan(thunks[t++]))
      return false;
  }

  return true;
}

void TextOutputSection::writeTo(uint8_t *buf) const {
  if (canWriteTextInputsInParallel(inputs, thunks, getFileSize())) {
    parallelForEach(inputs, [buf](ConcatInputSection *isec) {
      isec->writeTo(buf + isec->outSecOff);
    });
    for (ConcatInputSection *thunk : thunks)
      thunk->writeTo(buf + thunk->outSecOff);
    return;
  }

  // Merge input sections from thunk & ordinary vectors
  size_t i = 0, ie = inputs.size();
  size_t t = 0, te = thunks.size();
  while (i < ie || t < te) {
    while (i < ie && (t == te || inputs[i]->empty() ||
                      inputs[i]->outSecOff < thunks[t]->outSecOff)) {
      inputs[i]->writeTo(buf + inputs[i]->outSecOff);
      ++i;
    }
    while (t < te && (i == ie || thunks[t]->outSecOff < inputs[i]->outSecOff)) {
      thunks[t]->writeTo(buf + thunks[t]->outSecOff);
      ++t;
    }
  }
}

uint32_t ConcatOutputSection::flagsContribution(const InputSection *input) {
  switch (sectionType(input->getFlags())) {
  default /*type-unspec'ed*/:
    // FIXME: Add additional logic here when supporting emitting obj files.
    break;
  case S_4BYTE_LITERALS:
  case S_8BYTE_LITERALS:
  case S_16BYTE_LITERALS:
  case S_CSTRING_LITERALS:
  case S_ZEROFILL:
  case S_LAZY_SYMBOL_POINTERS:
  case S_MOD_TERM_FUNC_POINTERS:
  case S_THREAD_LOCAL_REGULAR:
  case S_THREAD_LOCAL_ZEROFILL:
  case S_THREAD_LOCAL_VARIABLES:
  case S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
  case S_THREAD_LOCAL_VARIABLE_POINTERS:
  case S_NON_LAZY_SYMBOL_POINTERS:
  case S_SYMBOL_STUBS:
    return input->getFlags();
  }
  return 0;
}

ConcatOutputSection *
ConcatOutputSection::getOrCreateForInput(const InputSection *isec) {
  NamePair names = maybeRenameSection({isec->getSegName(), isec->getName()});
  ConcatOutputSection *&osec = concatOutputSections[names];
  if (!osec) {
    if (isec->getSegName() == segment_names::text &&
        isec->getName() != section_names::gccExceptTab &&
        isec->getName() != section_names::ehFrame)
      osec = make<TextOutputSection>(names.second);
    else
      osec = make<ConcatOutputSection>(names.second);
  }
  return osec;
}

NamePair macho::maybeRenameSection(NamePair key) {
  auto newNames = config->sectionRenameMap.find(key);
  if (newNames != config->sectionRenameMap.end())
    return newNames->second;
  return key;
}
