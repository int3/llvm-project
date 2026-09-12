//===- MemAlloc.cpp - Memory allocation functions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MemAlloc.h"
#include <new>

#ifdef __linux__

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sys/mman.h>
#include <vector>

// On Linux, buffers of at least 4 KiB are served from thread-local pool chunks
// that are mapped on a 2 MiB boundary and advised MADV_HUGEPAGE, instead of
// going through malloc. Every BumpPtrAllocator slab (through MallocAllocator)
// and every out-of-line DenseMap bucket table is allocated here, so a single
// huge page replaces up to 512 minor faults' worth of first touches, and
// process teardown unmaps huge pages instead of 4 KiB ones. Buffers of at
// least 1 MiB get their own advised mapping and are really unmapped when they
// are deallocated; smaller ones are bump-carved out of a 32 MiB chunk and are
// not reused when deallocated. Buffers under 4 KiB keep the malloc path: they
// are too small to reach a huge page and are the common case for the many
// small allocations that would otherwise strand pool space.

namespace {

constexpr size_t HugePageSize = size_t(2) << 20;
// Smallest request served from the pool. Below this, malloc.
constexpr size_t PoolMinSize = 4096;
// At or above this, a request gets its own advised mapping.
constexpr size_t OwnMappingSize = size_t(1) << 20;
// Pool chunk size and alignment. Chunks are aligned to their own size so the
// chunk header of a carve-out is found by masking its address.
constexpr size_t ChunkSize = size_t(32) << 20;
// Reserved at the start of a chunk for its live-carve-out counter. Keeps the
// carve region 64-byte aligned; the advised range still starts on a 2 MiB
// boundary because the chunk itself does.
constexpr size_t ChunkHeaderSize = 64;

size_t roundUp(size_t Value, size_t Align) {
  return (Value + Align - 1) & ~(Align - 1);
}

/// Map at least \p Len bytes aligned to \p Align (a power of two, a multiple of
/// the huge page size) and advise the whole range for transparent huge pages.
/// Returns null if the mapping fails. The mapped length is
/// roundUp(Len, HugePageSize), which deallocation recomputes from the size.
void *mapAdvised(size_t Len, size_t Align) {
  size_t Size = roundUp(Len, HugePageSize);
  // Over-reserve so the range can be trimmed down to an aligned mapping of
  // exactly Size bytes.
  size_t Reserve = Size + Align;
  void *Raw = mmap(nullptr, Reserve, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (Raw == MAP_FAILED)
    return nullptr;
  uintptr_t Base = reinterpret_cast<uintptr_t>(Raw);
  uintptr_t Aligned = roundUp(Base, Align);
  if (Aligned != Base)
    munmap(Raw, Aligned - Base);
  size_t Tail = Reserve - (Aligned - Base) - Size;
  if (Tail != 0)
    munmap(reinterpret_cast<void *>(Aligned + Size), Tail);
  void *Result = reinterpret_cast<void *>(Aligned);
  madvise(Result, Size, MADV_HUGEPAGE);
  return Result;
}

// Number of carve-outs in a chunk that are still live, plus one while the
// chunk is the thread's open chunk. The chunk is unmapped when this reaches
// zero, so a carve-out that outlives the pool release keeps its chunk alive.
struct ChunkHeader {
  std::atomic<size_t> Live;
};

ChunkHeader *chunkHeaderOf(void *Carveout) {
  return reinterpret_cast<ChunkHeader *>(reinterpret_cast<uintptr_t>(Carveout) &
                                         ~(ChunkSize - 1));
}

void releaseChunkRef(ChunkHeader *Header) {
  if (Header->Live.fetch_sub(1, std::memory_order_acq_rel) == 1)
    munmap(Header, ChunkSize);
}

// One per thread that has allocated from the pool. These are never destroyed;
// releasing the pool only closes the open chunk and resets the bump pointer,
// so the thread_local pointer below never dangles.
struct PoolState {
  char *Cur = nullptr;
  char *End = nullptr;
  ChunkHeader *Open = nullptr;
};

std::mutex &poolMutex() {
  static std::mutex M;
  return M;
}

std::vector<PoolState *> &poolStates() {
  static std::vector<PoolState *> States;
  return States;
}

thread_local PoolState *ThreadPool = nullptr;

PoolState *getPoolState() {
  if (PoolState *P = ThreadPool)
    return P;
  PoolState *P = new PoolState();
  std::lock_guard<std::mutex> Lock(poolMutex());
  poolStates().push_back(P);
  ThreadPool = P;
  return P;
}

bool usePool(size_t Size, size_t Alignment) {
  // An alignment larger than a huge page cannot be promised by an advised
  // mapping, so those requests stay on the malloc path. The decision depends
  // only on the size and alignment, which deallocation is given as well.
  return Size >= PoolMinSize && Alignment <= HugePageSize;
}

void *poolAllocate(size_t Size, size_t Alignment) {
  if (Size >= OwnMappingSize)
    return mapAdvised(Size, HugePageSize);

  PoolState *P = getPoolState();
  uintptr_t Aligned = roundUp(reinterpret_cast<uintptr_t>(P->Cur), Alignment);
  if (P->Cur == nullptr || Aligned + Size > reinterpret_cast<uintptr_t>(P->End)) {
    void *Chunk = mapAdvised(ChunkSize, ChunkSize);
    if (Chunk == nullptr)
      return nullptr;
    ChunkHeader *Header = new (Chunk) ChunkHeader{};
    // One reference for the chunk being open.
    Header->Live.store(1, std::memory_order_relaxed);
    if (P->Open != nullptr)
      releaseChunkRef(P->Open);
    P->Open = Header;
    P->Cur = static_cast<char *>(Chunk) + ChunkHeaderSize;
    P->End = static_cast<char *>(Chunk) + ChunkSize;
    Aligned = roundUp(reinterpret_cast<uintptr_t>(P->Cur), Alignment);
  }
  P->Open->Live.fetch_add(1, std::memory_order_relaxed);
  P->Cur = reinterpret_cast<char *>(Aligned + Size);
  return reinterpret_cast<void *>(Aligned);
}

void poolDeallocate(void *Ptr, size_t Size) {
  if (Size >= OwnMappingSize) {
    munmap(Ptr, roundUp(Size, HugePageSize));
    return;
  }
  // Carve-outs are not reused; the chunk goes away once nothing in it is live.
  releaseChunkRef(chunkHeaderOf(Ptr));
}

} // namespace

void llvm::release_buffer_pool() {
  std::lock_guard<std::mutex> Lock(poolMutex());
  for (PoolState *P : poolStates()) {
    if (P->Open != nullptr)
      releaseChunkRef(P->Open);
    P->Open = nullptr;
    P->Cur = nullptr;
    P->End = nullptr;
  }
}

#else

void llvm::release_buffer_pool() {}

#endif

// These are out of line to have __cpp_aligned_new not affect ABI.

LLVM_ATTRIBUTE_RETURNS_NONNULL LLVM_ATTRIBUTE_RETURNS_NOALIAS void *
llvm::allocate_buffer(size_t Size, size_t Alignment) {
#ifdef __linux__
  if (usePool(Size, Alignment)) {
    void *Pooled = poolAllocate(Size, Alignment);
    if (Pooled == nullptr)
      report_bad_alloc_error("Buffer allocation failed");
    return Pooled;
  }
#endif
  void *Result = ::operator new(Size,
#ifdef __cpp_aligned_new
                                std::align_val_t(Alignment),
#endif
                                std::nothrow);
  if (Result == nullptr) {
    report_bad_alloc_error("Buffer allocation failed");
  }
  return Result;
}

void llvm::deallocate_buffer(void *Ptr, size_t Size, size_t Alignment) {
#ifdef __linux__
  if (usePool(Size, Alignment)) {
    poolDeallocate(Ptr, Size);
    return;
  }
#endif
  ::operator delete(Ptr
#ifdef __cpp_sized_deallocation
                    ,
                    Size
#endif
#ifdef __cpp_aligned_new
                    ,
                    std::align_val_t(Alignment)
#endif
  );
}
