//===- Memory.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/Memory.h"
#include "lld/Common/CommonLinkerContext.h"

#include <mutex>

using namespace llvm;
using namespace lld;

// The registry is reached from more than one thread: worker threads look up
// arenas while the main thread may be registering a new one (its first
// make<T>() of a type during a parallel input phase), and a DenseMap insert
// that rehashes would free the buckets a concurrent lookup is reading. The
// lock is off every hot path: getSpecificAllocSingleton() caches the result
// per thread and per context generation, so each thread gets here once per
// arena type per link.
static std::mutex registryMutex;

SpecificAllocBase *
lld::SpecificAllocBase::getOrCreate(void *tag, size_t size, size_t align,
                                    SpecificAllocBase *(&creator)(void *)) {
  std::lock_guard<std::mutex> lock(registryMutex);
  auto &instances = context().instances;
  auto &instance = instances[tag];
  if (instance == nullptr) {
    void *storage = context().bAlloc.Allocate(size, align);
    instance = creator(storage);
  }
  return instance;
}
