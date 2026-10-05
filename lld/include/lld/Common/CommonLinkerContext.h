//===- CommonLinkerContext.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Entry point for all global state in lldCommon. The objective is for LLD to be
// used "as a library" in a thread-safe manner.
//
// Instead of program-wide globals or function-local statics, we prefer
// aggregating all "global" states into a heap-based structure
// (CommonLinkerContext). This also achieves deterministic initialization &
// shutdown for all "global" states.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_COMMONLINKINGCONTEXT_H
#define LLD_COMMON_COMMONLINKINGCONTEXT_H

#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/ThreadSafeAllocator.h"

#include <memory>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace lld {
struct SpecificAllocBase;
class CommonLinkerContext {
public:
  CommonLinkerContext();
  virtual ~CommonLinkerContext();

  static void destroy();

  llvm::BumpPtrAllocator bAlloc;
  // Relocations are initially decoded in parallel. Keep their rare out-of-line
  // payloads in a link-owned allocator rather than lazily registering a
  // SpecificAlloc from a worker.
  llvm::ThreadSafeAllocator<llvm::BumpPtrAllocator> relocationAlloc;
  // Small Mach-O relocation vector buffers. Fixed 16 KiB slabs avoid growth
  // into oversized worker-local allocations. Seed the slots before parsing;
  // slot zero is the serial caller, or the non-worker driver in parallel mode.
  // Members outlive the SpecificAlloc destruction in ~CommonLinkerContext,
  // including vectors embedded in input sections, and are released during
  // normal context destruction.
  // These slabs replace small malloc buffers. Keep them outside allocate_buffer's
  // huge-page pool: moving them there increases THP demand and compaction stalls.
  struct RelocationSlabAllocator {
    void *Allocate(size_t size, size_t alignment) {
      assert(alignment <= alignof(std::max_align_t));
      return llvm::safe_malloc(size);
    }
    void Deallocate(const void *ptr, size_t, size_t) {
      std::free(const_cast<void *>(ptr));
    }
  };
  using RelocationBufferSlabs = llvm::BumpPtrAllocatorImpl<
      RelocationSlabAllocator, 16384, 16384, SIZE_MAX>;
  std::unique_ptr<RelocationBufferSlabs[]> relocationBufferSlabs;
  size_t relocationBufferSlotCount = 0;
  llvm::StringSaver saver{bAlloc};
  llvm::UniqueStringSaver uniqueSaver{bAlloc};
  llvm::DenseMap<void *, SpecificAllocBase *> instances;

  ErrorHandler e;
};

// Retrieve the global state. Currently only one state can exist per process,
// but in the future we plan on supporting an arbitrary number of LLD instances
// in a single process.
CommonLinkerContext &commonContext();

template <typename T = CommonLinkerContext> T &context() {
  return static_cast<T &>(commonContext());
}

bool hasContext();

inline llvm::BumpPtrAllocator &bAlloc() { return context().bAlloc; }
inline llvm::StringSaver &saver() { return context().saver; }
inline llvm::UniqueStringSaver &uniqueSaver() { return context().uniqueSaver; }
} // namespace lld

#endif
