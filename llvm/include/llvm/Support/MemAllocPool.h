//===- MemAllocPool.h - Huge-page buffer pool internals ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// Direct access to the huge-page pool that backs llvm::allocate_buffer on
/// Linux, for a program that wants to route its own large allocations there.
/// The pool lives in llvm/lib/Support/MemAlloc.cpp; this header is separate
/// from MemAlloc.h so that the atomics the range test needs are not pulled
/// into every translation unit that allocates.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_MEMALLOCPOOL_H
#define LLVM_SUPPORT_MEMALLOCPOOL_H

#include "llvm/Support/Compiler.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

/// Whether the huge-page buffer pool is compiled in. Linux only, because it is
/// transparent huge pages that make it worth anything. LP64 only, because the
/// pool reserves tens of gigabytes of address space, which an ILP32 process does
/// not have. Never under ASan, which cannot see through the pool's mappings and
/// would stop poisoning the buffers and bucket tables that go through it.
#if defined(__linux__) && defined(__LP64__) && !LLVM_ADDRESS_SANITIZER_BUILD
#define LLVM_ENABLE_HUGEPAGE_POOL 1
#else
#define LLVM_ENABLE_HUGEPAGE_POOL 0
#endif

namespace llvm {

#if LLVM_ENABLE_HUGEPAGE_POOL

/// Smallest allocation pool_allocate_block will serve. Blocks of at least this
/// size are aligned to it, which is what makes the side-table index exact and
/// therefore what makes an unsized deallocation possible.
constexpr size_t pool_block_min_size = size_t(64) << 10;

/// Bounds of the single virtual region every pool block lives in. Both are
/// zero until the region is reserved, which happens on the first pool
/// allocation; pool_region_size is published last with a release store, so a
/// reader that sees a nonzero size also sees the base.
LLVM_ABI extern std::atomic<uintptr_t> pool_region_base;
LLVM_ABI extern std::atomic<uintptr_t> pool_region_size;

/// Whether \p Ptr was returned by the pool. One range compare: pool chunks and
/// the mappings owned by individual large blocks are all carved out of the one
/// reserved region, so no metadata lookup is needed to answer this.
inline bool pool_owns_block(const void *Ptr) {
  // Acquire on the size pins the base load below it.
  uintptr_t Size = pool_region_size.load(std::memory_order_acquire);
  uintptr_t Base = pool_region_base.load(std::memory_order_relaxed);
  return reinterpret_cast<uintptr_t>(Ptr) - Base < Size;
}

/// Allocate \p Size bytes with at least \p Alignment from the huge-page pool.
/// Returns null, rather than reporting an error, when the request is not one
/// the pool serves (\p Size below pool_block_min_size, \p Alignment above it)
/// or when the region cannot be reserved or is exhausted: the caller is
/// expected to fall back to malloc. The size is recorded in a side table, so
/// pool_deallocate_block needs only the pointer.
LLVM_ABI void *pool_allocate_block(size_t Size, size_t Alignment);

/// Free a block returned by pool_allocate_block. \p Ptr must be non-null and
/// must satisfy pool_owns_block.
LLVM_ABI void pool_deallocate_block(void *Ptr);

#endif // LLVM_ENABLE_HUGEPAGE_POOL

} // namespace llvm

#endif // LLVM_SUPPORT_MEMALLOCPOOL_H
