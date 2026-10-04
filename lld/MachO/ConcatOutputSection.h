//===- ConcatOutputSection.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_CONCAT_OUTPUT_SECTION_H
#define LLD_MACHO_CONCAT_OUTPUT_SECTION_H

#include "InputSection.h"
#include "OutputSection.h"
#include "Symbols.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerUnion.h"

namespace lld::macho {

// Linking multiple files will inevitably mean resolving sections in different
// files that are labeled with the same segment and section name. This class
// contains all such sections and writes the data from each section sequentially
// in the final binary.
class ConcatOutputSection : public OutputSection {
public:
  explicit ConcatOutputSection(StringRef name,
                               OutputSection::Kind kind = ConcatKind)
      : OutputSection(kind, name) {}

  const ConcatInputSection *firstSection() const { return inputs.front(); }
  const ConcatInputSection *lastSection() const { return inputs.back(); }
  bool isNeeded() const override { return !inputs.empty(); }

  // These accessors will only be valid after finalizing the section
  uint64_t getSize() const override { return size; }
  uint64_t getFileSize() const override { return fileSize; }

  // Assign values to InputSection::outSecOff. In contrast to TextOutputSection,
  // which does this in its implementation of `finalize()`, we can do this
  // without `finalize()`'s sequential guarantees detailed in the block comment
  // of `OutputSection::finalize()`.
  virtual void finalizeContents();

  // Dense index into the createOutputSections() scatter tables. Valid only for
  // the duration of that function.
  uint32_t denseIndex = 0;

  // The section flag bits that \p input contributes to a non-first input of an
  // output section. Folding these with | is associative and commutative, so it
  // can be accumulated per chunk and merged later.
  static uint32_t flagsContribution(const InputSection *input);

  void writeTo(uint8_t *buf) const override;

  static bool classof(const OutputSection *sec) {
    return sec->kind() == ConcatKind || sec->kind() == TextKind;
  }

  static ConcatOutputSection *getOrCreateForInput(const InputSection *);

  std::vector<ConcatInputSection *> inputs;
  // createOutputSections() lays `inputs` down already partitioned into three
  // runs -- negative explicit priority, ordinary, cold -- each in input order.
  // These are the two boundaries; see sortSegmentsAndSections().
  uint32_t negativePriorityCount = 0;
  uint32_t nonColdCount = 0;

protected:
  size_t size = 0;
  uint64_t fileSize = 0;
  void finalizeOne(ConcatInputSection *);
};

// We maintain one ThunkInfo per real function.
//
// The "active thunk" is represented by the sym/isec pair that
// turns-over during finalize(): as the call-site address advances,
// the active thunk goes out of branch-range, and we create a new
// thunk to take its place.
//
// The remaining members -- bools and counters -- apply to the
// collection of thunks associated with the real function.

struct ThunkInfo {
  // These denote the active thunk:
  Defined *sym = nullptr;             // private-extern symbol for active thunk
  ConcatInputSection *isec = nullptr; // input section for active thunk

  // The following value is cumulative across all thunks on this function
  uint8_t sequence = 0; // how many thunks created so-far?
};

// ConcatOutputSections that contain code (text) require special handling to
// support thunk insertion.
class TextOutputSection : public ConcatOutputSection {
public:
  explicit TextOutputSection(StringRef name)
      : ConcatOutputSection(name, TextKind) {}
  void finalizeContents() override {}
  void finalize() override;
  /// Also numbers \p inputs for the thunk walk: input i gets icfEqClass[0] =
  /// i (ICF is done by now and nothing else reads icfEqClass).
  bool needsThunks() const;
  ArrayRef<ConcatInputSection *> getThunks() const { return thunks; }
  void writeTo(uint8_t *buf) const override;

  static bool classof(const OutputSection *sec) {
    return sec->kind() == TextKind;
  }

private:
  std::vector<ConcatInputSection *> thunks;
  /// \return true if the target in \p r is in range from the location in \p
  /// isec. If the target isec is not finalized, \return false.
  bool isTargetKnownInRange(const ConcatInputSection &isec,
                            const Relocation &r) const;
  /// If there exists a thunk in range of the target in \p r, \return that
  /// thunk.
  Defined *getThunkInRange(const ConcatInputSection &isec, const Relocation &r,
                           const ThunkInfo &thunkInfo) const;
  /// The same, for a branch at \p callVA.
  Defined *getThunkInRange(uint64_t callVA, const ThunkInfo &thunkInfo) const;
  /// Update \p r to target \p thunk which is guaranteed to be in range.
  void updateBranchTargetToThunk(Relocation &r, Defined *thunk);
  /// Create a new thunk and update \p r to target the new thunk.
  void createThunk(const ConcatInputSection &isec, Relocation &r,
                   ThunkInfo &thunkInfo);
  /// \return the largest possible stub section end VA or \p std::nullopt if we
  /// can't estimate this yet. Used to determine if stub symbol targets are in
  /// range.
  std::optional<uint64_t> estimateStubsEndVA(unsigned numPotentialThunks) const;
  /// \return true if the target in \p r is in __stubs or __objc_stubs and in
  /// range from \p callVA. \p estimatedStubsEnd is the estimated VA of the end
  /// of the last stubs section. \p addendIsZero and \p inStubs are r's addend
  /// test and its referent's isInStubs(), recorded when the branch was queued;
  /// \p inObjCStubs is set when the referent is known to be defined in
  /// __objc_stubs (and so is an Objective-C stub symbol).
  bool isTargetStubsAndInRange(uint64_t callVA, const Relocation &r,
                               bool addendIsZero, bool inStubs,
                               bool inObjCStubs,
                               std::optional<uint64_t> estimatedStubsEnd) const;
  /// The number of relocations updated to point to thunks.
  size_t thunkCallCount = 0;
  /// Set when createThunk() replaced an existing symbol of the thunk's name
  /// in place, which can change a queued branch's key.
  bool thunkReplacedSymbol = false;
};

NamePair maybeRenameSection(NamePair key);

// Output sections are added to output segments in iteration order
// of ConcatOutputSection, so must have deterministic iteration order.
extern llvm::MapVector<NamePair, ConcatOutputSection *> concatOutputSections;

// Branch-extension thunks are keyed by both the target referent and the
// branch relocation's addend.  Two call sites that branch to the same
// symbol with different addends (e.g. `bl _func` and `bl _func+8`) target
// distinct addresses and therefore need distinct thunks.
//
// After ICF, multiple Defined symbols may point to the same (isec, value)
// yet remain as distinct Symbol pointers. The constructor stores those symbols
// as (isec, value) so ICF-folded copies still share a thunkMap entry when their
// addends match. Non-Defined symbols retain their Symbol identity.
struct ThunkKey {
  llvm::PointerUnion<InputSection *, Symbol *> referent;
  uint64_t value;
  int64_t addend;

  ThunkKey(Symbol *sym, int64_t addend) : value(0), addend(addend) {
    if (const auto *d = dyn_cast<Defined>(sym)) {
      referent = d->isec();
      value = d->value;
    } else {
      referent = sym;
    }
  }
  ThunkKey(Relocation &r)
      : ThunkKey(cast<Symbol *>(r.getReferent()), r.getAddend()) {}

  bool operator==(const ThunkKey &other) const {
    return referent == other.referent && value == other.value &&
           addend == other.addend;
  }
};

// A thunkMap lookup key whose hash was computed earlier (the thunk walk hashes
// each queued branch's key once, when it first probes the map).
struct HashedThunkKey {
  const ThunkKey &key;
  unsigned hash;
};

struct ThunkMapKeyInfo {
  static unsigned getHashValue(const ThunkKey &k) {
    return llvm::hash_combine(k.referent.getOpaqueValue(), k.value, k.addend);
  }
  static bool isEqual(const ThunkKey &lhs, const ThunkKey &rhs) {
    return lhs == rhs;
  }
  static unsigned getHashValue(const HashedThunkKey &k) { return k.hash; }
  static bool isEqual(const HashedThunkKey &lhs, const ThunkKey &rhs) {
    return lhs.key == rhs;
  }
};

extern llvm::DenseMap<ThunkKey, ThunkInfo, ThunkMapKeyInfo> thunkMap;

} // namespace lld::macho

#endif
