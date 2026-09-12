//===- MemAlloc.cpp - Memory allocation functions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MemAlloc.h"
#include "llvm/Support/MemAllocPool.h"
#include <new>

#if LLVM_ENABLE_HUGEPAGE_POOL

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
// least 2 MiB get their own advised mapping whose pages are really given back
// when they are deallocated; smaller ones are bump-carved out of a 32 MiB chunk
// and are not reused when deallocated. Buffers under 4 KiB keep the malloc path: they
// are too small to reach a huge page and are the common case for the many
// small allocations that would otherwise strand pool space.
//
// Every chunk and every own mapping is carved out of a single large virtual
// region that is reserved on first use, so a pointer can be recognized as a
// pool pointer by one range compare (llvm::pool_owns_block) with no lookup.
// pool_allocate_block/pool_deallocate_block expose that to a program that
// wants to route its own large allocations here (ld64.lld replaces the global
// operator new family with them): a block of at least 64 KiB is 64 KiB-aligned
// and its size is recorded in a side table indexed by (ptr - base) >> 16, so
// it can be freed without the size being supplied.

namespace {

constexpr size_t HugePageSize = size_t(2) << 20;
// Smallest request served from the pool. Below this, malloc.
constexpr size_t PoolMinSize = 4096;
// At or above this, a request gets its own advised mapping. It is the huge page
// size and not smaller: an own mapping is rounded up to a huge page, so a
// buffer below 2 MiB would pay for a whole one, while inside a chunk it pays
// only for the pages it touches.
constexpr size_t OwnMappingSize = HugePageSize;
// Pool chunk size and alignment. Chunks are aligned to their own size so the
// chunk header of a carve-out is found by masking its address.
constexpr size_t ChunkSize = size_t(32) << 20;
// Reserved at the start of a chunk for its live-carve-out counter. Keeps the
// carve region 64-byte aligned; the advised range still starts on a 2 MiB
// boundary because the chunk itself does.
constexpr size_t ChunkHeaderSize = 64;
// Alignment and side-table granularity of the blocks that pool_allocate_block
// serves.
constexpr size_t BlockGranule = llvm::pool_block_min_size;
// Size of the reserved region. Address space only: it is MAP_NORESERVE and
// nothing but the parts actually carved and touched costs memory.
constexpr size_t RegionSize = size_t(64) << 30;
constexpr size_t SideTableSlots = RegionSize / BlockGranule;

size_t roundUp(size_t Value, size_t Align) {
  return (Value + Align - 1) & ~(Align - 1);
}

/// Give the pages of a freed part of the region back to the system. This must
/// not be munmap: the region has to stay mapped from end to end, because a hole
/// punched in it is exactly where the kernel would place the next unhinted
/// mmap, and a malloc chunk landing inside the region would then answer true to
/// pool_owns_block and be freed as if it were a pool block. MADV_DONTNEED zaps
/// the pages (splitting any huge page it straddles), which drops them from RSS
/// the way munmap would, and leaves the mapping and the range test intact.
/// Freed address space is never reused, so the zero-fill on a later touch of
/// these pages is never seen.
void releaseRegionPages(void *Ptr, size_t Len) {
  madvise(Ptr, Len, MADV_DONTNEED);
}

// Number of carve-outs in a chunk that are still live, plus one while the
// chunk is the thread's open chunk. The chunk's pages are given back when this
// reaches zero, so a carve-out that outlives the pool release keeps its chunk
// alive.
struct ChunkHeader {
  std::atomic<size_t> Live;
};

ChunkHeader *chunkHeaderOf(void *Carveout) {
  return reinterpret_cast<ChunkHeader *>(reinterpret_cast<uintptr_t>(Carveout) &
                                         ~(ChunkSize - 1));
}

void releaseChunkRef(ChunkHeader *Header) {
  if (Header->Live.fetch_sub(1, std::memory_order_acq_rel) == 1)
    releaseRegionPages(Header, ChunkSize);
}

std::mutex &poolMutex() {
  static std::mutex M;
  return M;
}

// Region state. Chunks are carved from the bottom of the region upwards in
// ChunkSize steps, so every chunk is ChunkSize-aligned; own mappings are
// carved from the top downwards in HugePageSize steps. The region's whole
// length is advised MADV_HUGEPAGE once, when it is reserved.
//
// Neither cursor ever rewinds. Freeing gives a chunk's or a block's pages back
// but not its address space, and release_buffer_pool() only closes the open
// chunks, so a process that links repeatedly in-process consumes fresh region
// every time: a link that carves a few hundred megabytes exhausts 64 GiB after
// on the order of a hundred links, after which the carvers return null and
// everything falls back to malloc. That is correct but unaccelerated, and it is
// why the standalone linker, which links once, is what this is for. A recycling
// allocator over the region would be the fix if a long-lived library user ever
// needed one; it is not worth the bookkeeping for the linker.
std::atomic<size_t> ChunkCursor{0};
std::atomic<size_t> OwnCursor{RegionSize};
// Size of each block handed out by pool_allocate_block, indexed by
// (ptr - base) / BlockGranule; zero for a slot that holds no such block.
size_t *SideTable = nullptr;
bool RegionUnavailable = false;

/// Reserve the region and the side table, once. The whole region is advised
/// MADV_HUGEPAGE here, in one call, rather than per chunk. Returns false if
/// that failed,
/// in which case the pool is permanently off and callers use malloc.
bool ensureRegion() {
  if (llvm::pool_region_size.load(std::memory_order_acquire) != 0)
    return true;
  std::lock_guard<std::mutex> Lock(poolMutex());
  if (llvm::pool_region_size.load(std::memory_order_relaxed) != 0)
    return true;
  if (RegionUnavailable)
    return false;
  // Over-reserve so the region can be trimmed to a ChunkSize boundary, which
  // is also a huge-page boundary.
  size_t Reserve = RegionSize + ChunkSize;
  void *Raw = mmap(nullptr, Reserve, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (Raw == MAP_FAILED) {
    RegionUnavailable = true;
    return false;
  }
  uintptr_t RawBase = reinterpret_cast<uintptr_t>(Raw);
  uintptr_t Base = roundUp(RawBase, ChunkSize);
  if (Base != RawBase)
    munmap(Raw, Base - RawBase);
  size_t Tail = Reserve - (Base - RawBase) - RegionSize;
  if (Tail != 0)
    munmap(reinterpret_cast<void *>(Base + RegionSize), Tail);
  madvise(reinterpret_cast<void *>(Base), RegionSize, MADV_HUGEPAGE);
  void *Table =
      mmap(nullptr, SideTableSlots * sizeof(size_t), PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (Table == MAP_FAILED) {
    munmap(reinterpret_cast<void *>(Base), RegionSize);
    RegionUnavailable = true;
    return false;
  }
  SideTable = static_cast<size_t *>(Table);
  llvm::pool_region_base.store(Base, std::memory_order_relaxed);
  // Publishing the size last makes the base visible to pool_owns_block.
  llvm::pool_region_size.store(RegionSize, std::memory_order_release);
  return true;
}

char *regionBase() {
  return reinterpret_cast<char *>(
      llvm::pool_region_base.load(std::memory_order_relaxed));
}

size_t &sideTableSlot(void *Ptr) {
  return SideTable[(reinterpret_cast<uintptr_t>(Ptr) -
                    llvm::pool_region_base.load(std::memory_order_relaxed)) /
                   BlockGranule];
}

/// Carve \p Len bytes, rounded up to a huge page, off the top of the region.
///
/// Both carvers take poolMutex(): the two cursors grow towards each other, so
/// each has to test the other to know whether the region is exhausted, and a
/// lock-free pair of cursors cannot do that test and its own move as one step.
/// The carves are rare -- one per 32 MiB chunk and one per own mapping, a few
/// dozen in a link -- so the lock costs nothing measurable, and neither carver
/// allocates while holding it.
void *carveOwnMapping(size_t Len) {
  Len = roundUp(Len, HugePageSize);
  std::lock_guard<std::mutex> Lock(poolMutex());
  size_t Own = OwnCursor.load(std::memory_order_relaxed);
  if (Own < Len)
    return nullptr;
  size_t New = Own - Len;
  if (New < ChunkCursor.load(std::memory_order_relaxed) + ChunkSize)
    return nullptr;
  OwnCursor.store(New, std::memory_order_relaxed);
  return regionBase() + New;
}

/// Carve one ChunkSize-aligned chunk off the bottom of the region and
/// initialize its header, with one reference held for it being open.
ChunkHeader *carveChunk() {
  size_t Old;
  {
    std::lock_guard<std::mutex> Lock(poolMutex());
    Old = ChunkCursor.load(std::memory_order_relaxed);
    size_t New = Old + ChunkSize;
    if (New > OwnCursor.load(std::memory_order_relaxed))
      return nullptr;
    ChunkCursor.store(New, std::memory_order_relaxed);
  }
  ChunkHeader *Header = new (regionBase() + Old) ChunkHeader{};
  Header->Live.store(1, std::memory_order_relaxed);
  return Header;
}

// One per thread that has allocated from the pool. These are never destroyed;
// releasing the pool only closes the open chunks and resets the bump pointers,
// so the thread_local pointer below never dangles. Buffers (allocate_buffer)
// and blocks (pool_allocate_block) bump through separate chunks: blocks are
// 64 KiB-aligned, and interleaving them with unaligned buffer carve-outs would
// strand up to 64 KiB of touched huge page per block.
struct PoolState {
  char *Cur = nullptr;
  char *End = nullptr;
  ChunkHeader *Open = nullptr;
  char *BlockCur = nullptr;
  char *BlockEnd = nullptr;
  ChunkHeader *BlockOpen = nullptr;
};

std::vector<PoolState *> &poolStates() {
  static std::vector<PoolState *> States;
  return States;
}

thread_local PoolState *ThreadPool = nullptr;

PoolState *getPoolState() {
  if (PoolState *P = ThreadPool)
    return P;
  PoolState *P = new PoolState();
  // Published before the lock so that an allocation made while registering
  // this state (the vector's own growth) finds it instead of recursing into
  // poolMutex.
  ThreadPool = P;
  std::lock_guard<std::mutex> Lock(poolMutex());
  poolStates().push_back(P);
  return P;
}

bool usePool(size_t Size, size_t Alignment) {
  // An alignment larger than a huge page cannot be promised by an advised
  // mapping, so those requests stay on the malloc path.
  return Size >= PoolMinSize && Alignment <= HugePageSize;
}

void *poolAllocate(size_t Size, size_t Alignment) {
  if (!ensureRegion())
    return nullptr;
  if (Size >= OwnMappingSize)
    return carveOwnMapping(Size);

  PoolState *P = getPoolState();
  uintptr_t Aligned = roundUp(reinterpret_cast<uintptr_t>(P->Cur), Alignment);
  if (P->Cur == nullptr ||
      Aligned + Size > reinterpret_cast<uintptr_t>(P->End)) {
    ChunkHeader *Header = carveChunk();
    if (Header == nullptr)
      return nullptr;
    if (P->Open != nullptr)
      releaseChunkRef(P->Open);
    P->Open = Header;
    P->Cur = reinterpret_cast<char *>(Header) + ChunkHeaderSize;
    P->End = reinterpret_cast<char *>(Header) + ChunkSize;
    Aligned = roundUp(reinterpret_cast<uintptr_t>(P->Cur), Alignment);
  }
  P->Open->Live.fetch_add(1, std::memory_order_relaxed);
  P->Cur = reinterpret_cast<char *>(Aligned + Size);
  return reinterpret_cast<void *>(Aligned);
}

void poolDeallocate(void *Ptr, size_t Size) {
  if (Size >= OwnMappingSize) {
    releaseRegionPages(Ptr, roundUp(Size, HugePageSize));
    return;
  }
  // Carve-outs are not reused; the chunk goes away once nothing in it is live.
  releaseChunkRef(chunkHeaderOf(Ptr));
}

} // namespace

void *llvm::pool_allocate_block(size_t Size, size_t Alignment) {
  if (Size < BlockGranule || Alignment > BlockGranule)
    return nullptr;
  if (!ensureRegion())
    return nullptr;

  void *Result;
  if (Size >= OwnMappingSize) {
    // A huge-page-aligned own mapping is 64 KiB-aligned as well.
    Result = carveOwnMapping(Size);
  } else {
    PoolState *P = getPoolState();
    uintptr_t Aligned =
        roundUp(reinterpret_cast<uintptr_t>(P->BlockCur), BlockGranule);
    if (P->BlockCur == nullptr ||
        Aligned + Size > reinterpret_cast<uintptr_t>(P->BlockEnd)) {
      ChunkHeader *Header = carveChunk();
      if (Header == nullptr)
        return nullptr;
      if (P->BlockOpen != nullptr)
        releaseChunkRef(P->BlockOpen);
      P->BlockOpen = Header;
      P->BlockCur = reinterpret_cast<char *>(Header) + ChunkHeaderSize;
      P->BlockEnd = reinterpret_cast<char *>(Header) + ChunkSize;
      Aligned = roundUp(reinterpret_cast<uintptr_t>(P->BlockCur), BlockGranule);
    }
    P->BlockOpen->Live.fetch_add(1, std::memory_order_relaxed);
    P->BlockCur = reinterpret_cast<char *>(Aligned + Size);
    Result = reinterpret_cast<void *>(Aligned);
  }
  if (Result == nullptr)
    return nullptr;
  sideTableSlot(Result) = Size;
  return Result;
}

void llvm::pool_deallocate_block(void *Ptr) {
  size_t &Slot = sideTableSlot(Ptr);
  size_t Size = Slot;
  Slot = 0;
  // A pool pointer with no recorded size was not handed out by
  // pool_allocate_block. That cannot happen for a correctly paired
  // allocation, and leaking is the safe response.
  if (Size == 0)
    return;
  if (Size < OwnMappingSize) {
    // A block owns its granule-rounded range exclusively, because the next
    // block in the stream starts at the next granule, so zapping all of it
    // frees the alignment tail too and cannot disturb a live neighbour. The
    // chunk itself lives on until its last block is freed.
    releaseRegionPages(Ptr, roundUp(Size, BlockGranule));
  }
  poolDeallocate(Ptr, Size);
}

void llvm::release_buffer_pool() {
  std::lock_guard<std::mutex> Lock(poolMutex());
  for (PoolState *P : poolStates()) {
    if (P->Open != nullptr)
      releaseChunkRef(P->Open);
    P->Open = nullptr;
    P->Cur = nullptr;
    P->End = nullptr;
    if (P->BlockOpen != nullptr)
      releaseChunkRef(P->BlockOpen);
    P->BlockOpen = nullptr;
    P->BlockCur = nullptr;
    P->BlockEnd = nullptr;
  }
}

namespace llvm {
LLVM_ABI std::atomic<uintptr_t> pool_region_base{0};
LLVM_ABI std::atomic<uintptr_t> pool_region_size{0};
} // namespace llvm

#else

void llvm::release_buffer_pool() {}

#endif

// These are out of line to have __cpp_aligned_new not affect ABI.

LLVM_ATTRIBUTE_RETURNS_NONNULL LLVM_ATTRIBUTE_RETURNS_NOALIAS void *
llvm::allocate_buffer(size_t Size, size_t Alignment) {
#if LLVM_ENABLE_HUGEPAGE_POOL
  if (usePool(Size, Alignment)) {
    if (void *Pooled = poolAllocate(Size, Alignment))
      return Pooled;
    // The region could not be reserved or is exhausted; fall through to the
    // malloc path, which deallocate_buffer recognizes by the range test.
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
#if LLVM_ENABLE_HUGEPAGE_POOL
  // The range test, rather than usePool, so that a buffer that fell back to
  // malloc is freed the way it was allocated.
  if (pool_owns_block(Ptr)) {
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
