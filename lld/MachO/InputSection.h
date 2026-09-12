//===- InputSection.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_INPUT_SECTION_H
#define LLD_MACHO_INPUT_SECTION_H

#include "Config.h"
#include "Relocations.h"
#include "Symbols.h"

#include "lld/Common/LLVM.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/BinaryFormat/MachO.h"

#include <atomic>
#include <memory>

namespace lld {
namespace macho {

class InputFile;
class OutputSection;
class ConcatOutputSection;

class InputSection {
public:
  enum Kind : uint8_t {
    ConcatKind,
    CStringLiteralKind,
    WordLiteralKind,
  };

  Kind kind() const { return sectionKind; }
  virtual ~InputSection() = default;
  virtual uint64_t getSize() const { return data.size(); }
  virtual bool empty() const { return data.empty(); }
  InputFile *getFile() const { return section.file; }
  StringRef getName() const { return section.name; }
  StringRef getSegName() const { return section.segname; }
  uint32_t getFlags() const { return section.flags; }
  uint64_t getFileSize() const;
  // Translates \p off -- an offset relative to this InputSection -- into an
  // offset from the beginning of its parent OutputSection.
  virtual uint64_t getOffset(uint64_t off) const = 0;
  // The offset from the beginning of the file.
  uint64_t getVA(uint64_t off) const;
  // Return a user-friendly string for use in diagnostics.
  // Format: /path/to/object.o:(symbol _func+0x123)
  std::string getLocation(uint64_t off) const;
  // Return the source line corresponding to an address, or the empty string.
  // Format: Source.cpp:123 (/path/to/Source.cpp:123)
  std::string getSourceLocation(uint64_t off) const;
  // Return the relocation at \p off, if it exists. This does a linear search.
  const Relocation *getRelocAt(uint32_t off) const;
  // Whether the data at \p off in this InputSection is live.
  virtual bool isLive(uint64_t off) const = 0;
  virtual void markLive(uint64_t off) = 0;
  virtual InputSection *canonical() { return this; }
  virtual const InputSection *canonical() const { return this; }

protected:
  InputSection(Kind kind, const Section &section, ArrayRef<uint8_t> data,
               uint32_t align)
      : sectionKind(kind), keepUnique(false), hasAltEntry(false), isCold(false),
        align(align), data(data), section(section) {}

  InputSection(const InputSection &rhs)
      : sectionKind(rhs.sectionKind), keepUnique(false), hasAltEntry(false),
        isCold(rhs.isCold), align(rhs.align), data(rhs.data),
        section(rhs.section) {}

  Kind sectionKind;

public:
  // is address assigned?
  bool isFinal = false;
  // keep the address of the symbol(s) in this section unique in the final
  // binary ?
  bool keepUnique : 1;
  // Does this section have symbols at offsets other than zero? (NOTE: only
  // applies to ConcatInputSections.)
  bool hasAltEntry : 1;
  // Is this considered cold? Computed before ICF. Currently reflects whether
  // any symbol in the section has the N_COLD_FUNC nlist flag set. Cold
  // sections are placed at the end of their containing output section to
  // improve locality of non-cold input sections. When a section is given an
  // explicit priority (via order file, --bp-startup-sort, or
  // --bp-compression-sort), this flag is unset so that the priority-based
  // ordering takes precedence over cold partitioning.
  bool isCold : 1;
  uint32_t align = 1;

  OutputSection *parent = nullptr;
  ArrayRef<uint8_t> data;
  std::vector<Relocation> relocs;
  // The symbols that belong to this InputSection, sorted by value. With
  // .subsections_via_symbols, there is typically only one element here.
  llvm::TinyPtrVector<Defined *> symbols;

  const Section &section;

protected:
  const Defined *getContainingSymbol(uint64_t off) const;
};

// ConcatInputSections are combined into (Concat)OutputSections through simple
// concatenation, in contrast with literal sections which may have their
// contents merged before output.
class ConcatInputSection final : public InputSection {
public:
  ConcatInputSection(const Section &section, ArrayRef<uint8_t> data,
                     uint32_t align = 1)
      : InputSection(ConcatKind, section, data, align) {}
  ConcatInputSection(const ConcatInputSection &other)
      : InputSection(other), replacement(other.replacement),
        icfEqClass{other.icfEqClass[0], other.icfEqClass[1]},
        wasCoalesced(other.wasCoalesced),
        live(other.live.load(std::memory_order_relaxed)),
        outSecOff(other.outSecOff) {}

  uint64_t getOffset(uint64_t off) const override { return outSecOff + off; }
  uint64_t getVA() const { return InputSection::getVA(0); }
  // ConcatInputSections are entirely live or dead, so the offset is irrelevant.
  bool isLive(uint64_t off) const override {
    return live.load(std::memory_order_relaxed);
  }
  void markLive(uint64_t off) override {
    live.store(true, std::memory_order_relaxed);
  }
  bool isCoalescedWeak() const { return wasCoalesced && symbols.empty(); }
  bool shouldOmitFromOutput() const { return !live || isCoalescedWeak(); }
  void writeTo(uint8_t *buf);

  void foldIdentical(ConcatInputSection *redundant,
                     Symbol::ICFFoldKind foldKind = Symbol::ICFFoldKind::Body);
  ConcatInputSection *canonical() override {
    return replacement ? replacement : this;
  }
  const InputSection *canonical() const override {
    return replacement ? replacement : this;
  }

  static bool classof(const InputSection *isec) {
    return isec->kind() == ConcatKind;
  }

  // Points to the surviving section after this one is folded by ICF
  ConcatInputSection *replacement = nullptr;
  // Equivalence-class ID for ICF
  uint32_t icfEqClass[2] = {0, 0};

  // With subsections_via_symbols, most symbols have their own InputSection,
  // and for weak symbols (e.g. from inline functions), only the
  // InputSection from one translation unit will make it to the output,
  // while all copies in other translation units are coalesced into the
  // first and not copied to the output.
  bool wasCoalesced = false;
  std::atomic_bool live = !config->deadStrip;
  // One-based index into UnwindInfoSection::symbols for the symbol at offset
  // zero. Kept in the padding before outSecOff.
  uint32_t unwindSymbolIndex = 0;
  // This variable has two usages. Initially, it represents the input order.
  // After assignAddresses is called, it represents the offset from the
  // beginning of the output section this section was assigned to.
  uint64_t outSecOff = 0;
};

// Initialize a fake InputSection that does not belong to any InputFile.
// The created ConcatInputSection will always have 'live=true'
ConcatInputSection *makeSyntheticInputSection(StringRef segName,
                                              StringRef sectName,
                                              uint32_t flags = 0,
                                              ArrayRef<uint8_t> data = {},
                                              uint32_t align = 1);

// Helper functions to make it easy to sprinkle asserts.

inline bool shouldOmitFromOutput(InputSection *isec) {
  return isa<ConcatInputSection>(isec) &&
         cast<ConcatInputSection>(isec)->shouldOmitFromOutput();
}

inline bool isCoalescedWeak(InputSection *isec) {
  return isa<ConcatInputSection>(isec) &&
         cast<ConcatInputSection>(isec)->isCoalescedWeak();
}

// We allocate a lot of these and binary search on them, so they should be as
// compact as possible. Hence the use of 31 rather than 64 bits for the hash.
struct StringPiece {
  // Offset from the start of the containing input section.
  uint32_t inSecOff;
  // Liveness and the dedup hash share one 32-bit word: bit 0 is the live flag,
  // bits 1-31 are the hash (only set if deduplicating literals). Packing them
  // explicitly lets dead-strip workers set the live bit with a single atomic OR
  // rather than deferring literal writes to a serial merge. `hash` is never
  // written after splitIntoPieces(), so the OR cannot lose it, and the other
  // two fields live in words of their own.
  std::atomic<uint32_t> liveAndHash;
  // Offset from the start of the containing output section.
  uint64_t outSecOff = 0;

  StringPiece(uint64_t off, uint32_t hash)
      : inSecOff(off), liveAndHash((hash << 1) | (config->deadStrip ? 0 : 1)) {}

  // std::atomic is neither copyable nor movable, but StringPiece is stored in a
  // std::vector and passed by value to predicates. Every such copy happens
  // outside markLive(), so a relaxed load is enough.
  StringPiece(const StringPiece &other)
      : inSecOff(other.inSecOff),
        liveAndHash(other.liveAndHash.load(std::memory_order_relaxed)),
        outSecOff(other.outSecOff) {}
  StringPiece &operator=(const StringPiece &other) {
    inSecOff = other.inSecOff;
    liveAndHash.store(other.liveAndHash.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    outSecOff = other.outSecOff;
    return *this;
  }

  bool isLive() const { return liveAndHash.load(std::memory_order_relaxed) & 1; }
  void setLive(bool live) {
    uint32_t word = liveAndHash.load(std::memory_order_relaxed);
    liveAndHash.store(live ? (word | 1) : (word & ~uint32_t(1)),
                      std::memory_order_relaxed);
  }
  // Unlike setLive(), safe to call concurrently on the same piece.
  void markLiveAtomic() { liveAndHash.fetch_or(1, std::memory_order_relaxed); }
  uint32_t getHash() const {
    return liveAndHash.load(std::memory_order_relaxed) >> 1;
  }
};

static_assert(sizeof(StringPiece) == 16, "StringPiece is too big!");

// CStringInputSections are composed of multiple null-terminated string
// literals, which we represent using StringPieces. These literals can be
// deduplicated and tail-merged, so translating offsets between the input and
// outputs sections is more complicated.
//
// NOTE: One significant difference between LLD and ld64 is that we merge all
// cstring literals, even those referenced directly by non-private symbols.
// ld64 is more conservative and does not do that. This was mostly done for
// implementation simplicity; if we find programs that need the more
// conservative behavior we can certainly implement that.
class CStringInputSection final : public InputSection {
public:
  CStringInputSection(const Section &section, ArrayRef<uint8_t> data,
                      uint32_t align, bool dedupLiterals)
      : InputSection(CStringLiteralKind, section, data, align),
        deduplicateLiterals(dedupLiterals) {}

  uint64_t getOffset(uint64_t off) const override;
  bool isLive(uint64_t off) const override {
    return getStringPiece(off).isLive();
  }
  // Safe to call concurrently: getStringPiece() only reads, and the live bit is
  // set with an atomic OR.
  void markLive(uint64_t off) override {
    getStringPiece(off).markLiveAtomic();
  }
  // Find the StringPiece that contains this offset.
  StringPiece &getStringPiece(uint64_t off);
  const StringPiece &getStringPiece(uint64_t off) const;
  // Split at each null byte.
  void splitIntoPieces();

  LLVM_ATTRIBUTE_ALWAYS_INLINE
  StringRef getStringRef(size_t i) const {
    size_t begin = pieces[i].inSecOff;
    // The endpoint should be *at* the null terminator, not after. This matches
    // the behavior of StringRef(const char *Str).
    size_t end =
        ((pieces.size() - 1 == i) ? data.size() : pieces[i + 1].inSecOff) - 1;
    return toStringRef(data.slice(begin, end - begin));
  }

  StringRef getStringRefAtOffset(uint64_t off) const {
    return getStringRef(getStringPieceIndex(off));
  }

  // Returns i'th piece as a CachedHashStringRef. This function is very hot when
  // string merging is enabled, so we want to inline.
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  llvm::CachedHashStringRef getCachedHashStringRef(size_t i) const {
    assert(deduplicateLiterals);
    return {getStringRef(i), pieces[i].getHash()};
  }

  static bool classof(const InputSection *isec) {
    return isec->kind() == CStringLiteralKind;
  }

  bool deduplicateLiterals = false;
  std::vector<StringPiece> pieces;

private:
  size_t getStringPieceIndex(uint64_t off) const;
};

class WordLiteralInputSection final : public InputSection {
public:
  WordLiteralInputSection(const Section &section, ArrayRef<uint8_t> data,
                          uint32_t align);
  uint64_t getOffset(uint64_t off) const override;
  bool isLive(uint64_t off) const override {
    size_t i = off >> power2LiteralSize;
    return (liveWords[i / 64].load(std::memory_order_relaxed) >> (i % 64)) & 1;
  }
  // Safe to call concurrently on the same section.
  void markLive(uint64_t off) override {
    size_t i = off >> power2LiteralSize;
    liveWords[i / 64].fetch_or(uint64_t(1) << (i % 64),
                               std::memory_order_relaxed);
  }

  static bool classof(const InputSection *isec) {
    return isec->kind() == WordLiteralKind;
  }

private:
  unsigned power2LiteralSize;
  // The liveness of data[off] is tracked by bit (off >> power2LiteralSize) of
  // liveWords. Dead-strip workers set those bits atomically, so no serial merge
  // of literal liveness is needed.
  std::unique_ptr<std::atomic<uint64_t>[]> liveWords;
};

inline uint8_t sectionType(uint32_t flags) {
  return flags & llvm::MachO::SECTION_TYPE;
}

inline bool isZeroFill(uint32_t flags) {
  return llvm::MachO::isVirtualSection(sectionType(flags));
}

inline bool isThreadLocalVariables(uint32_t flags) {
  return sectionType(flags) == llvm::MachO::S_THREAD_LOCAL_VARIABLES;
}

// These sections contain the data for initializing thread-local variables.
inline bool isThreadLocalData(uint32_t flags) {
  return sectionType(flags) == llvm::MachO::S_THREAD_LOCAL_REGULAR ||
         sectionType(flags) == llvm::MachO::S_THREAD_LOCAL_ZEROFILL;
}

inline bool isDebugSection(uint32_t flags) {
  return (flags & llvm::MachO::SECTION_ATTRIBUTES_USR) ==
         llvm::MachO::S_ATTR_DEBUG;
}

inline bool isWordLiteralSection(uint32_t flags) {
  return sectionType(flags) == llvm::MachO::S_4BYTE_LITERALS ||
         sectionType(flags) == llvm::MachO::S_8BYTE_LITERALS ||
         sectionType(flags) == llvm::MachO::S_16BYTE_LITERALS;
}

bool isCodeSection(const InputSection *);
bool isCfStringSection(const InputSection *);
bool isClassRefsSection(const InputSection *);
bool isSelRefsSection(const InputSection *);
bool isEhFrameSection(const InputSection *);
bool isGccExceptTabSection(const InputSection *);

extern std::vector<ConcatInputSection *> inputSections;
// This is used as a counter for specyfing input order for input sections
extern int inputSectionsOrder;

namespace section_names {

constexpr const char authGot[] = "__auth_got";
constexpr const char authPtr[] = "__auth_ptr";
constexpr const char binding[] = "__binding";
constexpr const char bitcodeBundle[] = "__bundle";
constexpr const char cString[] = "__cstring";
constexpr const char cfString[] = "__cfstring";
constexpr const char cgProfile[] = "__cg_profile";
constexpr const char chainFixups[] = "__chainfixups";
constexpr const char codeSignature[] = "__code_signature";
constexpr const char common[] = "__common";
constexpr const char compactUnwind[] = "__compact_unwind";
constexpr const char data[] = "__data";
constexpr const char debugAbbrev[] = "__debug_abbrev";
constexpr const char debugInfo[] = "__debug_info";
constexpr const char debugLine[] = "__debug_line";
constexpr const char debugStr[] = "__debug_str";
constexpr const char debugStrOffs[] = "__debug_str_offs";
constexpr const char ehFrame[] = "__eh_frame";
constexpr const char gccExceptTab[] = "__gcc_except_tab";
constexpr const char export_[] = "__export";
constexpr const char dataInCode[] = "__data_in_code";
constexpr const char functionStarts[] = "__func_starts";
constexpr const char got[] = "__got";
constexpr const char header[] = "__mach_header";
constexpr const char indirectSymbolTable[] = "__ind_sym_tab";
constexpr const char initOffsets[] = "__init_offsets";
constexpr const char const_[] = "__const";
constexpr const char lazySymbolPtr[] = "__la_symbol_ptr";
constexpr const char lazyBinding[] = "__lazy_binding";
constexpr const char literals[] = "__literals";
constexpr const char functionMap[] = "__llvm_merge";
constexpr const char moduleInitFunc[] = "__mod_init_func";
constexpr const char moduleTermFunc[] = "__mod_term_func";
constexpr const char nonLazySymbolPtr[] = "__nl_symbol_ptr";
constexpr const char objcCatList[] = "__objc_catlist";
constexpr const char objcClassList[] = "__objc_classlist";
constexpr const char objcMethList[] = "__objc_methlist";
constexpr const char objcClassRefs[] = "__objc_classrefs";
constexpr const char objcConst[] = "__objc_const";
constexpr const char objCImageInfo[] = "__objc_imageinfo";
constexpr const char objcStubs[] = "__objc_stubs";
constexpr const char objcSelrefs[] = "__objc_selrefs";
constexpr const char objcMethname[] = "__objc_methname";
constexpr const char objcNonLazyCatList[] = "__objc_nlcatlist";
constexpr const char objcNonLazyClassList[] = "__objc_nlclslist";
constexpr const char objcProtoList[] = "__objc_protolist";
constexpr const char outlinedHashTree[] = "__llvm_outline";
constexpr const char pageZero[] = "__pagezero";
constexpr const char pointers[] = "__pointers";
constexpr const char rebase[] = "__rebase";
constexpr const char staticInit[] = "__StaticInit";
constexpr const char stringTable[] = "__string_table";
constexpr const char stubHelper[] = "__stub_helper";
constexpr const char stubs[] = "__stubs";
constexpr const char swift[] = "__swift";
constexpr const char symbolTable[] = "__symbol_table";
constexpr const char textCoalNt[] = "__textcoal_nt";
constexpr const char text[] = "__text";
constexpr const char threadVars[] = "__thread_vars";
constexpr const char unwindInfo[] = "__unwind_info";
constexpr const char weakBinding[] = "__weak_binding";
constexpr const char zeroFill[] = "__zerofill";
constexpr const char addrSig[] = "__llvm_addrsig";

} // namespace section_names

void addInputSection(InputSection *inputSection);
// The caller starts \p osec null for each input Section. All input sections
// passed with it must belong to that Section.
void addInputSection(InputSection *inputSection, ConcatOutputSection *&osec);

uint64_t resolveSymbolOffsetVA(const Symbol *sym, uint8_t type, int64_t offset);
} // namespace macho

std::string toString(const macho::InputSection *);

} // namespace lld

#endif
