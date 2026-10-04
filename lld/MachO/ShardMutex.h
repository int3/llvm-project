//===- ShardMutex.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A mutex for the parallel input phase's shard locks: it tries the lock in user
// space a bounded number of times before blocking.
//
// These critical sections are usually short (a cached-hash lookup, a bump
// allocation, a placement-new), but map rehashes, vector growth, allocator
// refill and page faults can lengthen them. Historical macOS System Traces
// showed substantial psynch_mutexwait traffic in the symbol and provider folds.
// Trying the mutex before its blocking path can avoid some of that traffic.
//
// The loop bounds the number of try_lock attempts and processor backoff hints
// at 128, not elapsed time or acquisition latency. A descheduled holder can
// leave contenders spinning without useful work and can suffer priority
// inversion. After the attempts, acquisition uses std::mutex::lock().
// Fairness and starvation guarantees are those of the underlying mutex;
// where its policy permits, the extra try_lock attempts give new arrivals
// additional opportunities to acquire ahead of sleeping waiters.
// Synchronization comes from std::mutex, not from the processor backoff.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_SHARD_MUTEX_H
#define LLD_MACHO_SHARD_MUTEX_H

#include <mutex>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace lld::macho {

class ShardMutex {
public:
  void lock() {
    for (unsigned i = 0; i != kSpins; ++i) {
      if (mu.try_lock())
        return;
      cpuRelax();
    }
    mu.lock();
  }
  bool try_lock() { return mu.try_lock(); }
  void unlock() { mu.unlock(); }

private:
  static constexpr unsigned kSpins = 128;
  static void cpuRelax() {
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC has no GCC-style inline assembly on x64 or ARM64; use its
    // intrinsics for the same instructions.
#if defined(_M_ARM64) || defined(_M_ARM64EC)
    __isb(_ARM64_BARRIER_SY);
#elif defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#endif
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64EC)
    // ARM64EC clang-cl also defines __x86_64__, so test it before x86.
    // ISB is the chosen processor backoff; its cost is processor-dependent.
    // Neither ISB nor the ARM YIELD instruction is an OS scheduler yield.
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    // No hint: the bounded try_lock loop is still correct.
#endif
  }

  std::mutex mu;
};

} // namespace lld::macho

#endif
