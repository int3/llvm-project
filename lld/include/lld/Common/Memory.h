//===- Memory.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines arena allocators.
//
// Almost all large objects, such as files, sections or symbols, are
// used for the entire lifetime of the linker once they are created.
// This usage characteristic makes arena allocator an attractive choice
// where the entire linker is one arena. With an arena, newly created
// objects belong to the arena and freed all at once when everything is done.
// Arena allocators are efficient and easy to understand.
// Most objects are allocated using the arena allocators defined by this file.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_MEMORY_H
#define LLD_COMMON_MEMORY_H

#include "llvm/Support/Allocator.h"
#include "llvm/Support/Compiler.h"

#include <cstdint>

namespace lld {
// Both declared in CommonLinkerContext.h, which includes this header.
class CommonLinkerContext;
CommonLinkerContext &commonContext();
// Bumped by every CommonLinkerContext constructor and destructor, so that a
// cached arena pointer is invalidated whenever the set of live contexts
// changes -- including a make<T>() after the context that owns the arena has
// been destroyed.
extern uint64_t commonContextGeneration;

// A base class only used by the CommonLinkerContext to keep track of the
// SpecificAlloc<> instances.
struct SpecificAllocBase {
  virtual ~SpecificAllocBase() = default;
  static SpecificAllocBase *getOrCreate(void *tag, size_t size, size_t align,
                                        SpecificAllocBase *(&creator)(void *));
};

// An arena of specific types T, created on-demand.
template <class T> struct SpecificAlloc : public SpecificAllocBase {
  static SpecificAllocBase *create(void *storage) {
    return new (storage) SpecificAlloc<T>();
  }
  llvm::SpecificBumpPtrAllocator<T> alloc;
  static int tag;
};

// The address of this static member is only used as a key in
// CommonLinkerContext::instances. Its value does not matter.
template <class T> int SpecificAlloc<T>::tag = 0;

// Creates the arena on-demand on the first call; or returns it, if it was
// already created.
//
// getOrCreate() is a DenseMap lookup in the linker context, and make<T>() calls
// it once per allocation, which on a link that creates millions of symbols and
// input sections is measurable: about 2% of a one-thread rust-analyzer link.
// The arena belongs to the context and is never replaced while that context
// lives, so the result is cached per thread and revalidated against the
// context generation alone. Comparing &commonContext() would not be a valid
// check by itself -- a second link in the same process can allocate its
// context where the previous one was -- and it is not needed either, since
// every context construction and destruction bumps the generation. Dropping
// it also keeps the fast path off commonContext(). The cache is thread-local
// rather than static because two contexts can in principle be live on
// different threads. getOrCreate() takes a lock, since workers and the main
// thread can reach it at the same time; the cache keeps that lock off the
// fast path.
template <typename T>
inline llvm::SpecificBumpPtrAllocator<T> &getSpecificAllocSingleton() {
  static thread_local uint64_t cachedGeneration = 0;
  static thread_local SpecificAlloc<T> *cachedInstance = nullptr;
  if (LLVM_UNLIKELY(cachedGeneration != commonContextGeneration)) {
    cachedInstance = (SpecificAlloc<T> *)SpecificAllocBase::getOrCreate(
        &SpecificAlloc<T>::tag, sizeof(SpecificAlloc<T>),
        alignof(SpecificAlloc<T>), SpecificAlloc<T>::create);
    cachedGeneration = commonContextGeneration;
  }
  return cachedInstance->alloc;
}

// Creates new instances of T off a (almost) contiguous arena/object pool. The
// instances are destroyed whenever lldMain() goes out of scope.
template <typename T, typename... U> T *make(U &&... args) {
  return new (getSpecificAllocSingleton<T>().Allocate())
      T(std::forward<U>(args)...);
}

template <typename T>
inline llvm::SpecificBumpPtrAllocator<T> &
getSpecificAllocSingletonThreadLocal() {
  thread_local SpecificAlloc<T> instance;
  return instance.alloc;
}

// Create a new instance of T off a thread-local SpecificAlloc, used by code
// like parallel input section initialization. The use cases assume that the
// return value outlives the containing parallelForEach (if exists), which is
// currently guaranteed: when parallelForEach returns, the threads allocating
// the TLS are not destroyed.
//
// Note: Some ports (e.g. ELF) have lots of global states which are currently
// infeasible to remove, and context() just adds overhead with no benefit. The
// allocation performance is of higher importance, so we simply use thread_local
// allocators instead of doing context indirection and pthread_getspecific.
template <typename T, typename... U> T *makeThreadLocal(U &&...args) {
  return new (getSpecificAllocSingletonThreadLocal<T>().Allocate())
      T(std::forward<U>(args)...);
}

template <typename T> T *makeThreadLocalN(size_t n) {
  return new (getSpecificAllocSingletonThreadLocal<T>().Allocate(n)) T[n];
}

} // namespace lld

#endif
