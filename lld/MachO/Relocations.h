//===- Relocations.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_RELOCATIONS_H
#define LLD_MACHO_RELOCATIONS_H

#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace lld::macho {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class Symbol;
class InputSection;

// Keep the existing referent union nestable without forcing Relocations.h to
// include complete Symbol and InputSection definitions merely to calculate its
// spare tag bits. Both pointee types are checked to be at least 4-byte aligned
// where they are complete in Relocations.cpp.
using RelocationReferent = llvm::PointerUnion<Symbol *, InputSection *>;

} // namespace lld::macho

namespace llvm {
template <> struct PointerLikeTypeTraits<lld::macho::RelocationReferent> {
  static void *getAsVoidPointer(lld::macho::RelocationReferent referent) {
    return referent.getOpaqueValue();
  }
  static lld::macho::RelocationReferent getFromVoidPointer(void *pointer) {
    return lld::macho::RelocationReferent::getFromOpaqueValue(pointer);
  }
  static constexpr int NumLowBitsAvailable = 1;
};
} // namespace llvm

namespace lld::macho {

enum class RelocAttrBits {
  _0 = 0,              // invalid
  BYTE1 = 1 << 0,      // 1 byte datum
  BYTE2 = 1 << 1,      // 2 byte datum
  BYTE4 = 1 << 2,      // 4 byte datum
  BYTE8 = 1 << 3,      // 8 byte datum
  PCREL = 1 << 4,      // Value is PC-relative offset
  ABSOLUTE = 1 << 5,   // Value is an absolute address or fixed offset
  EXTERN = 1 << 6,     // Can have an external symbol
  LOCAL = 1 << 7,      // Can have a local symbol
  ADDEND = 1 << 8,     // *_ADDEND paired prefix reloc
  SUBTRAHEND = 1 << 9, // *_SUBTRACTOR paired prefix reloc
  BRANCH = 1 << 10,    // Value is branch target
  GOT = 1 << 11,       // References a symbol in the Global Offset Table
  TLV = 1 << 12,       // References a thread-local symbol
  LOAD = 1 << 13,      // Relaxable indirect load
  POINTER = 1 << 14,   // Non-relaxable indirect load (pointer is taken)
  UNSIGNED = 1 << 15,  // *_UNSIGNED relocs
  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue*/ (1 << 16) - 1),
};
// Note: SUBTRACTOR always pairs with UNSIGNED (a delta between two symbols).

struct RelocAttrs {
  llvm::StringRef name;
  RelocAttrBits bits;
  bool hasAttr(RelocAttrBits b) const { return (bits & b) == b; }
};

struct Relocation {
  using Referent = RelocationReferent;

private:
  // Most addends fit in 21 bits. Keep the uncommon wide addends with their
  // referent in an immutable, shared payload; copies can then stay trivial and
  // mutations allocate only when they actually diverge.
  struct Extended {
    Referent referent;
    int64_t addend;
  };
  static_assert(sizeof(void *) != 8 || sizeof(Extended) == 16);
  static_assert(alignof(Extended) >= alignof(int64_t));
  using Target = llvm::PointerUnion<Referent, const Extended *>;

  // type: 8 bits, pcrel: 1 bit, length: 2 bits, inline addend: 21 bits.
  uint32_t metadata = llvm::MachO::GENERIC_RELOC_INVALID;
  // The offset from the start of the subsection that this relocation belongs
  // to.
  uint32_t offset = 0;
  Target target = Referent(nullptr);

  static constexpr uint32_t typeMask = 0xff;
  static constexpr uint32_t pcrelMask = 1u << 8;
  static constexpr uint32_t lengthMask = 3u << 9;
  static constexpr unsigned addendShift = 11;
  static constexpr uint32_t addendMask = (1u << 21) - 1;

  static uint32_t packMetadata(uint8_t type, bool pcrel, uint8_t length,
                               int64_t addend) {
    assert(length < 4);
    assert(llvm::isInt<21>(addend));
    return type | (static_cast<uint32_t>(pcrel) << 8) |
           (static_cast<uint32_t>(length) << 9) |
           ((static_cast<uint32_t>(addend) & addendMask) << addendShift);
  }

  bool hasExtendedTarget() const {
    return llvm::isa<const Extended *>(target);
  }
  const Extended *getExtendedTarget() const {
    return llvm::cast<const Extended *>(target);
  }

public:

  Relocation() = default;

  Relocation(uint8_t type, bool pcrel, uint8_t length, uint32_t offset,
             int64_t addend,
             Referent referent)
      : metadata(packMetadata(type, pcrel, length, /*addend=*/0)),
        offset(offset) {
    setTarget(referent, addend);
  }

  uint8_t getType() const { return metadata & typeMask; }
  bool isPCRel() const { return metadata & pcrelMask; }
  uint8_t getLength() const { return (metadata & lengthMask) >> 9; }
  uint32_t getOffset() const { return offset; }
  int64_t getAddend() const {
    if (hasExtendedTarget())
      return getExtendedTarget()->addend;
    return llvm::SignExtend64<21>(metadata >> addendShift);
  }
  Referent getReferent() const {
    if (hasExtendedTarget())
      return getExtendedTarget()->referent;
    return llvm::cast<Referent>(target);
  }

  void setType(uint8_t type) {
    metadata = (metadata & ~typeMask) | type;
  }
  void setPCRel(bool pcrel) {
    metadata = (metadata & ~pcrelMask) |
               (static_cast<uint32_t>(pcrel) << 8);
  }
  void setLength(uint8_t length) {
    assert(length < 4);
    metadata = (metadata & ~lengthMask) | (static_cast<uint32_t>(length) << 9);
  }
  void setOffset(uint32_t value) { offset = value; }
  void setAddend(int64_t addend) { setTarget(getReferent(), addend); }
  void setReferent(Referent referent) { setTarget(referent, getAddend()); }
  void setTarget(Referent referent, int64_t addend);

  InputSection *getReferentInputSection() const;

  // Must point to an offset within a CStringInputSection or a
  // ConcatInputSection.
  llvm::StringRef getReferentString() const;
};

static_assert(std::is_trivially_copyable_v<Relocation>);
static_assert(std::is_trivially_destructible_v<Relocation>);

void initRelocationBufferSlabs();
void *allocateSmallRelocationBuffer(size_t bytes);

// Stateless and link-scoped: vectors may move/swap between workers, and frees
// need neither the allocating thread nor a live linker context. All allocator
// instances are interchangeable within a link; buffers must not escape it.
// Only requests strictly below 4 KiB use the slabs. Growth beyond that boundary
// uses the same std::allocator allocation and sized deallocation as before.
template <class T> class RelocationAllocator {
  static_assert(std::is_same_v<T, Relocation>);
  static_assert(4096 % sizeof(T) == 0);

public:
  using value_type = T;
  using is_always_equal = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  T *allocate(size_t n) {
    if (n == 0)
      return nullptr;
    if (n < 4096 / sizeof(T))
      return static_cast<T *>(allocateSmallRelocationBuffer(n * sizeof(T)));
    return std::allocator<T>().allocate(n);
  }
  void deallocate(T *p, size_t n) noexcept {
    if (n >= 4096 / sizeof(T))
      std::allocator<T>().deallocate(p, n);
  }
  bool operator==(const RelocationAllocator &) const noexcept { return true; }
  bool operator!=(const RelocationAllocator &) const noexcept { return false; }
};
using RelocationVector = std::vector<Relocation, RelocationAllocator<Relocation>>;
static_assert(std::is_empty_v<RelocationAllocator<Relocation>>);
static_assert(sizeof(RelocationVector) == sizeof(std::vector<Relocation>));

bool validateSymbolRelocation(const Symbol *, const InputSection *,
                              const Relocation &);

/*
 * v: The value the relocation is attempting to encode
 * bits: The number of bits actually available to encode this relocation
 */
void reportRangeError(void *loc, const Relocation &, const llvm::Twine &v,
                      uint8_t bits, int64_t min, uint64_t max);

struct SymbolDiagnostic {
  const Symbol *symbol;
  llvm::StringRef reason;
};

void reportRangeError(void *loc, SymbolDiagnostic, const llvm::Twine &v,
                      uint8_t bits, int64_t min, uint64_t max);

template <typename Diagnostic>
inline void checkInt(void *loc, Diagnostic d, int64_t v, int bits) {
  if (v != llvm::SignExtend64(v, bits))
    reportRangeError(loc, d, llvm::Twine(v), bits, llvm::minIntN(bits),
                     llvm::maxIntN(bits));
}

template <typename Diagnostic>
inline void checkUInt(void *loc, Diagnostic d, uint64_t v, int bits) {
  if ((v >> bits) != 0)
    reportRangeError(loc, d, llvm::Twine(v), bits, 0, llvm::maxUIntN(bits));
}

inline void writeAddress(uint8_t *loc, uint64_t addr, uint8_t length) {
  switch (length) {
  case 2:
    llvm::support::endian::write32le(loc, addr);
    break;
  case 3:
    llvm::support::endian::write64le(loc, addr);
    break;
  default:
    llvm_unreachable("invalid r_length");
  }
}

InputSection *offsetToInputSection(uint64_t *);

extern const RelocAttrs invalidRelocAttrs;

} // namespace lld::Macho

#endif
