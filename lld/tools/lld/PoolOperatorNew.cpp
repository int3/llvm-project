//===- PoolOperatorNew.cpp - Route large allocations to the pool ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On Linux, replace the global operator new/delete family for the lld
// executable so that allocations of 64 KiB and more come from the huge-page
// pool that already backs llvm::allocate_buffer (see MemAlloc.cpp). The pool
// serves llvm::BumpPtrAllocator slabs and DenseMap tables; the large
// std::vector and SmallVector buffers that the linker grows on its critical
// path (the symbol vector, the LINKEDIT builders, the relocation and
// section vectors) go through operator new instead, and they are the
// remaining source of 4 KiB first-touch faults.
//
// Recognizing a pool pointer on deallocation needs no size and no header: all
// pool memory lives inside one reserved virtual region, so llvm::
// pool_owns_block is a range compare, and the block's size comes from the
// pool's side table. Unsized, sized, aligned and nothrow deallocation
// therefore all work. Everything below 64 KiB, and anything more aligned than
// 64 KiB, goes to malloc exactly as before; malloc and free called from C code
// are untouched, and a pointer that malloc produced is always returned to
// free, whichever operator frees it.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MemAllocPool.h"

#if LLVM_ENABLE_HUGEPAGE_POOL

#include "llvm/Support/ErrorHandling.h"
#include <cstdlib>
#include <new>

namespace {

// The default alignment malloc already guarantees.
constexpr size_t MallocAlignment = alignof(std::max_align_t);

void *allocateOrNull(size_t Size, size_t Alignment) {
  if (Size >= llvm::pool_block_min_size &&
      Alignment <= llvm::pool_block_min_size)
    if (void *Pooled = llvm::pool_allocate_block(Size, Alignment))
      return Pooled;
  if (Alignment <= MallocAlignment)
    return std::malloc(Size != 0 ? Size : 1);
  // aligned_alloc wants a size that is a multiple of the alignment.
  size_t Rounded = Size != 0 ? (Size + Alignment - 1) & ~(Alignment - 1)
                             : Alignment;
  return std::aligned_alloc(Alignment, Rounded);
}

void *allocate(size_t Size, size_t Alignment) {
  if (void *Result = allocateOrNull(Size, Alignment))
    return Result;
  // LLVM is built without exceptions, so a failed allocation reports instead
  // of throwing std::bad_alloc.
  llvm::report_bad_alloc_error("Allocation failed");
}

void deallocate(void *Ptr) {
  if (Ptr == nullptr)
    return;
  if (llvm::pool_owns_block(Ptr)) {
    llvm::pool_deallocate_block(Ptr);
    return;
  }
  std::free(Ptr);
}

} // namespace

void *operator new(size_t Size) { return allocate(Size, MallocAlignment); }
void *operator new[](size_t Size) { return allocate(Size, MallocAlignment); }
void *operator new(size_t Size, std::align_val_t Align) {
  return allocate(Size, static_cast<size_t>(Align));
}
void *operator new[](size_t Size, std::align_val_t Align) {
  return allocate(Size, static_cast<size_t>(Align));
}
void *operator new(size_t Size, const std::nothrow_t &) noexcept {
  return allocateOrNull(Size, MallocAlignment);
}
void *operator new[](size_t Size, const std::nothrow_t &) noexcept {
  return allocateOrNull(Size, MallocAlignment);
}
void *operator new(size_t Size, std::align_val_t Align,
                   const std::nothrow_t &) noexcept {
  return allocateOrNull(Size, static_cast<size_t>(Align));
}
void *operator new[](size_t Size, std::align_val_t Align,
                     const std::nothrow_t &) noexcept {
  return allocateOrNull(Size, static_cast<size_t>(Align));
}

void operator delete(void *Ptr) noexcept { deallocate(Ptr); }
void operator delete[](void *Ptr) noexcept { deallocate(Ptr); }
void operator delete(void *Ptr, size_t) noexcept { deallocate(Ptr); }
void operator delete[](void *Ptr, size_t) noexcept { deallocate(Ptr); }
void operator delete(void *Ptr, std::align_val_t) noexcept { deallocate(Ptr); }
void operator delete[](void *Ptr, std::align_val_t) noexcept {
  deallocate(Ptr);
}
void operator delete(void *Ptr, size_t, std::align_val_t) noexcept {
  deallocate(Ptr);
}
void operator delete[](void *Ptr, size_t, std::align_val_t) noexcept {
  deallocate(Ptr);
}
void operator delete(void *Ptr, const std::nothrow_t &) noexcept {
  deallocate(Ptr);
}
void operator delete[](void *Ptr, const std::nothrow_t &) noexcept {
  deallocate(Ptr);
}
void operator delete(void *Ptr, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  deallocate(Ptr);
}
void operator delete[](void *Ptr, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  deallocate(Ptr);
}

#endif // LLVM_ENABLE_HUGEPAGE_POOL
