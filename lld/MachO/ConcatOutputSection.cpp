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

#include <deque>
#include <limits>

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
                                    Relocation &r, ThunkInfo &thunkInfo) {
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
  if (!isa<Defined>(funcSym) || cast<Defined>(funcSym)->isExternal()) {
    // addDefined() would replace an input symbol of the same name in place and
    // silently redirect every branch to it. Lazy symbols are not definitions
    // yet and are replaced harmlessly.
    if (Symbol *clash = symtab->find(thunkName);
        clash && (isa<Defined>(clash) || isa<DylibSymbol>(clash)))
      error("linker-generated thunk name '" + thunkName +
            "' collides with " + toString(*clash) + " from " +
            toString(clash->getFile()));
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
    std::optional<uint64_t> estimatedStubsEnd) const {
  if (!estimatedStubsEnd.has_value())
    return false;
  // Every test here is a pure function of r and its referent; test the
  // recorded bits first so most branches never touch the symbol again.
  if (!addendIsZero)
    return false;
  if (!inStubs && !(in.objcStubs && in.objcStubs->isNeeded() &&
                    ObjCStubsSection::isObjCStubSymbol(
                        cast<Symbol *>(r.getReferent()))))
    return false;
  uint64_t highVA = callVA + target->forwardBranchRange;
  return *estimatedStubsEnd <= highVA;
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
};

struct PendingBranch {
  Relocation *r;
  ConcatInputSection *caller;
  uint64_t callVA;
  uint64_t dest;
  uint32_t targetIdx;
  uint32_t flags;
};

struct DeferredRedirect {
  Relocation *r;
  Defined *thunk;
  uint64_t callVA;
  uint64_t dest;
  uint32_t targetIdx;
  uint32_t flags;
};

bool isVAInBranchRange(uint64_t callVA, uint64_t va) {
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  return lowVA <= va && va <= highVA;
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
  // so the output is unchanged.
  const size_t numInputs = inputs.size();
  // finalOff[i] is inputs[i]->outSecOff once the walk has passed input i.
  std::vector<uint64_t> finalOff(numInputs);

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
  auto makePending = [&](ConcatInputSection *caller, Relocation &r,
                         uint64_t callVA) {
    auto *funcSym = cast<Symbol *>(r.getReferent());
    int64_t addend = r.getAddend();
    bool symInStubs = funcSym->isInStubs();
    PendingBranch b{&r, caller, callVA, 0, fixedTarget,
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
    }
    b.dest = resolveSymbolOffsetVA(funcSym, r.getType(), addend);
    return b;
  };
  auto defer = [](const PendingBranch &b, Defined *thunk) {
    return DeferredRedirect{b.r, thunk, b.callVA, b.dest, b.targetIdx,
                            b.flags};
  };

  // Branches whose target sections are out of range or have not yet been
  // finalized. We may need to emit thunks for them.
  std::deque<PendingBranch> branchesToProcess;
  // Branches whose targets have not yet be finalized, but a thunk for that
  // target exists. We defer processing these branches because it's possible we
  // can still direct call to their targets after they have all been finalized.
  std::vector<DeferredRedirect> deferredBranchRedirects;

  const uint64_t slop = config->slopScale * target->thunkSize;
  // The front of the queue waits until it is forced or its target becomes
  // final. While it waits nothing it depends on changes: thunks are created
  // only for the front, a fixed target's VA is constant, and a target in this
  // section becomes final only when the walk passes it. So a front is
  // evaluated once when it becomes the front (`frontChecked`), and again only
  // at its wake index `frontWake`, the first input after its target.
  bool frontChecked = false;
  size_t frontWake = 0;
  for (size_t idx = 0; idx != numInputs; ++idx) {
    ConcatInputSection *isec = inputs[idx];
    while (!branchesToProcess.empty()) {
      PendingBranch &b = branchesToProcess.front();
      if (!frontChecked || idx >= frontWake) {
        if (isInRange(b.callVA, b.dest, b.targetIdx, b.flags, idx)) {
          branchesToProcess.pop_front();
          frontChecked = false;
          continue;
        }
        if (!frontChecked) {
          auto it = thunkMap.find(*b.r);
          if (it != thunkMap.end()) {
            if (auto *thunk = getThunkInRange(b.callVA, it->second)) {
              deferredBranchRedirects.push_back(defer(b, thunk));
              branchesToProcess.pop_front();
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
      if (nextEnd + slop <= highVA)
        break;

      auto &thunkInfo = thunkMap[*b.r];
      createThunk(*b.caller, *b.r, thunkInfo);
      frontChecked = false;
      branchesToProcess.pop_front();
    }
    finalizeOne(isec);
    finalOff[idx] = isec->outSecOff;

    // Process relocs by ascending address, i.e., ascending offset within isec
    // FIXME: This property does not hold for object files produced by ld64's
    // `-r` mode.
    assert(is_sorted(isec->relocs, [](Relocation &a, Relocation &b) {
      return a.getOffset() > b.getOffset();
    }));
    const uint64_t isecVA = addr + isec->outSecOff;
    for (Relocation &r : reverse(isec->relocs)) {
      if (!target->hasAttr(r.getType(), RelocAttrBits::BRANCH))
        continue;
      PendingBranch b = makePending(isec, r, isecVA + r.getOffset());
      assert(isInRange(b.callVA, b.dest, b.targetIdx, b.flags, idx + 1) ==
             isTargetKnownInRange(*isec, r));
      if (isInRange(b.callVA, b.dest, b.targetIdx, b.flags, idx + 1))
        continue;
      auto it = thunkMap.find(r);
      if (it != thunkMap.end()) {
        if (auto *thunk = getThunkInRange(b.callVA, it->second)) {
          deferredBranchRedirects.push_back(defer(b, thunk));
          continue;
        }
      }
      branchesToProcess.push_back(b);
    }
  }

  llvm::erase_if(branchesToProcess, [&](const PendingBranch &b) {
    return isInRange(b.callVA, b.dest, b.targetIdx, b.flags, numInputs);
  });
  // Count distinct unresolved branch targets that still lack an in-range thunk.
  // We use this as an upper bound on the number of thunks we may still create
  // when estimating where __stubs / __objc_stubs could end up.
  DenseSet<ThunkKey, ThunkMapKeyInfo> branchTargets;
  for (const PendingBranch &b : branchesToProcess) {
    ThunkKey thunkKey(*b.r);
    auto it = thunkMap.find(thunkKey);
    if (it == thunkMap.end() || !getThunkInRange(b.callVA, it->second))
      branchTargets.insert(thunkKey);
  }

  auto estimatedStubsEnd = estimateStubsEndVA(branchTargets.size());
  for (const DeferredRedirect &d : deferredBranchRedirects) {
    if (isInRange(d.callVA, d.dest, d.targetIdx, d.flags, numInputs))
      continue;
    if (isTargetStubsAndInRange(d.callVA, *d.r, d.flags & addendIsZero,
                                d.flags & inStubs, estimatedStubsEnd))
      continue;
    updateBranchTargetToThunk(*d.r, d.thunk);
  }

  for (const PendingBranch &b : branchesToProcess) {
    if (isTargetStubsAndInRange(b.callVA, *b.r, b.flags & addendIsZero,
                                b.flags & inStubs, estimatedStubsEnd))
      continue;
    auto &thunkInfo = thunkMap[*b.r];
    if (auto *thunk = getThunkInRange(b.callVA, thunkInfo)) {
      updateBranchTargetToThunk(*b.r, thunk);
      continue;
    }
    createThunk(*b.caller, *b.r, thunkInfo);
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
