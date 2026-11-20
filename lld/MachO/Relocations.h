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
#include "llvm/Support/MemAlloc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

namespace lld::macho {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class Symbol;
class InputSection;

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
  AUTH = 1 << 16,      // *_AUTHENTICATED_POINTER relocs
  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue*/ (1 << 17) - 1),
};
// Note: SUBTRACTOR always pairs with UNSIGNED (a delta between two symbols).

struct RelocAttrs {
  llvm::StringRef name;
  RelocAttrBits bits;
  bool hasAttr(RelocAttrBits b) const { return (bits & b) == b; }
};

// Matches ptrauth_key in <ptrauth.h>.
enum PtrAuthKey : uint8_t {
  PtrAuthKeyIA,
  PtrAuthKeyIB,
  PtrAuthKeyDA,
  PtrAuthKeyDB
};

// The signing schema of an authenticated pointer.
struct AuthInfo {
  uint16_t diversity;
  PtrAuthKey key;
  bool addrDiv;

  bool operator==(const AuthInfo &other) const {
    return diversity == other.diversity && key == other.key &&
           addrDiv == other.addrDiv;
  }
  bool operator!=(const AuthInfo &other) const { return !(*this == other); }
};

struct AuthReloc {
  int32_t addend;
  AuthInfo info;
};
static_assert(sizeof(AuthReloc) == sizeof(int64_t),
              "AuthReloc must match int64_t size");

class Relocation {
public:
  uint8_t type = llvm::MachO::GENERIC_RELOC_INVALID;
  bool pcrel = false;
  uint8_t length = 0;
  bool hasAuth = false;
  // The offset from the start of the subsection that this relocation belongs
  // to.
  uint32_t offset = 0;
  llvm::PointerUnion<Symbol *, InputSection *> referent = nullptr;

  Relocation() = default;

  Relocation(uint8_t type, bool pcrel, uint8_t length, uint32_t offset,
             int64_t addend,
             llvm::PointerUnion<Symbol *, InputSection *> referent)
      : type(type), pcrel(pcrel), length(length), offset(offset),
        referent(referent), addend(addend) {}

  const AuthInfo *getAuthInfo() const {
    return hasAuth ? &authData.info : nullptr;
  }
  void setAuthInfo(AuthInfo info) {
    assert(!hasAuth);
    authData = {static_cast<int32_t>(addend), info};
    hasAuth = true;
  }
  int64_t getAddend() const {
    return hasAuth ? static_cast<int64_t>(authData.addend) : addend;
  }
  void setAddend(int64_t a) {
    if (hasAuth)
      authData.addend = static_cast<int32_t>(a);
    else
      addend = a;
  }

  InputSection *getReferentInputSection() const;

  // Must point to an offset within a CStringInputSection or a
  // ConcatInputSection.
  llvm::StringRef getReferentString() const;

private:
  // Adding this offset to the address of the referent symbol or subsection
  // gives the destination that this relocation refers to.
  union {
    int64_t addend = 0;
    AuthReloc authData; // Valid when hasAuth.
  };
};

// Borrow relocation storage from the input Section to avoid allocating per
// subsection. Growing a borrowed slice moves it to a buffer of its own.
class RelocArray {
public:
  using value_type = Relocation;
  using iterator = Relocation *;
  using const_iterator = const Relocation *;

  RelocArray() = default;
  RelocArray(const RelocArray &) = delete;
  RelocArray &operator=(const RelocArray &) = delete;
  ~RelocArray() {
    if (cap)
      free(ptr);
  }

  iterator begin() { return ptr; }
  iterator end() { return ptr + len; }
  const_iterator begin() const { return ptr; }
  const_iterator end() const { return ptr + len; }
  size_t size() const { return len; }
  bool empty() const { return len == 0; }
  Relocation &operator[](size_t i) {
    assert(i < len);
    return ptr[i];
  }
  const Relocation &operator[](size_t i) const {
    assert(i < len);
    return ptr[i];
  }
  Relocation &front() { return (*this)[0]; }
  const Relocation &front() const { return (*this)[0]; }
  Relocation &back() { return (*this)[len - 1]; }

  void push_back(const Relocation &r) {
    // r may be an element of this array.
    Relocation copy = r;
    // A slice has no capacity: the array after it belongs to other subsections.
    if (len >= cap)
      grow();
    new (ptr + len++) Relocation(copy);
  }
  template <class... Args> Relocation &emplace_back(Args &&...args) {
    push_back(Relocation(std::forward<Args>(args)...));
    return back();
  }
  template <class It> void insert(iterator pos, It first, It last) {
    assert(pos == end() && "only appending is supported");
    for (; first != last; ++first)
      push_back(*first);
  }
  iterator erase(iterator it) { return erase(it, it + 1); }
  iterator erase(iterator first, iterator last) {
    len = std::move(last, end(), first) - ptr;
    return first;
  }

  // The parser reverses the Section buffer to put slices in subsection order.
  // Preserve the order within each slice; separately grown arrays are
  // unaffected.
  void restoreOrderAfterBufferReverse(Relocation *begin, Relocation *end) {
    if (cap || empty())
      return;
    ptr = begin + (end - (ptr + len));
    std::reverse(ptr, ptr + len);
  }

  void appendInPlace(Relocation *slot) {
    assert(cap == 0 && "slice has its own buffer");
    assert(len == 0 || ptr + len == slot);
    if (len == 0)
      ptr = slot;
    ++len;
  }

private:
  void grow() {
    uint32_t newCap = std::max<uint32_t>(4, 2 * len);
    auto *buf = static_cast<Relocation *>(
        llvm::safe_malloc(newCap * sizeof(Relocation)));
    std::uninitialized_copy(ptr, ptr + len, buf);
    if (cap)
      free(ptr);
    ptr = buf;
    cap = newCap;
  }

  Relocation *ptr = nullptr;
  uint32_t len = 0;
  // Zero while this is a slice of the Section's array.
  uint32_t cap = 0;
};

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
