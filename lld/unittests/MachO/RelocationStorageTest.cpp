//===- RelocationStorageTest.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../MachO/Relocations.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Parallel.h"
#include "gtest/gtest.h"
#include <thread>

using namespace lld;
using namespace lld::macho;

LLD_HAS_DRIVER(macho)

namespace {
class RelocationStorageTest : public testing::Test {
  llvm::ThreadPoolStrategy savedStrategy;

protected:
  void SetUp() override {
    savedStrategy = llvm::parallel::strategy;
    llvm::parallel::strategy = llvm::hardware_concurrency(8);
    new CommonLinkerContext;
    initRelocationBufferSlabs();
  }
  void TearDown() override {
    CommonLinkerContext::destroy();
    llvm::parallel::strategy = savedStrategy;
  }
};

TEST_F(RelocationStorageTest, BoundariesAndRawStorage) {
  auto &slabs = context().relocationBufferSlabs[0];
  RelocationAllocator<Relocation> alloc;
  EXPECT_EQ(nullptr, alloc.allocate(0));
  alloc.deallocate(nullptr, 0);
  for (size_t n : {1U, 254U, 255U, 256U, 257U, 512U}) {
    Relocation *p = alloc.allocate(n);
    EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(p) % alignof(Relocation));
    EXPECT_EQ(n < 256, slabs.identifyObject(p).has_value());
    // Construction belongs to the container/caller, not the slab allocator.
    new (p) Relocation();
    p->setOffset(n);
    EXPECT_EQ(n, p->getOffset());
    p->~Relocation();
    alloc.deallocate(p, n);
  }
}

TEST_F(RelocationStorageTest, ReserveGrowEraseCopyMoveSwap) {
  RelocationVector a;
  EXPECT_TRUE(a.empty());
  a.reserve(1);
  a.emplace_back();
  a.front().setOffset(42);
  for (size_t n : {255U, 256U, 257U, 1024U}) {
    a.reserve(n);
    a.resize(n);
    EXPECT_EQ(42U, a.front().getOffset());
    a.back().setOffset(n);
    a.erase(a.begin() + 1, a.end() - 1);
    EXPECT_EQ(n, a.back().getOffset());
  }
  RelocationVector b(a);
  EXPECT_EQ(42U, b.front().getOffset());
  auto *small = b.data();
  auto *large = a.data();
  a.swap(b);
  EXPECT_EQ(small, a.data());
  EXPECT_EQ(large, b.data());
  RelocationVector c(std::move(a));
  EXPECT_EQ(small, c.data());
  b = std::move(c);
  EXPECT_EQ(small, b.data());
  EXPECT_EQ(42U, b.front().getOffset());
  b.clear();
  b.shrink_to_fit();
  EXPECT_TRUE(b.empty());
}

TEST_F(RelocationStorageTest, WorkerAllocationMainGrowthAndForeignDestruction) {
  std::vector<RelocationVector> vectors(4096);
  llvm::parallelFor(0, vectors.size(), [&](size_t i) {
    vectors[i].resize(i % 257 + 1);
    vectors[i].front().setOffset(i);
  });
  for (size_t i = 0; i != vectors.size(); ++i) {
    EXPECT_EQ(i, vectors[i].front().getOffset());
    // Growth and copying happen on a different worker identity.
    vectors[i].reserve(512);
    EXPECT_EQ(i, vectors[i].front().getOffset());
    vectors[i].resize(1);
    vectors[i].shrink_to_fit();
  }
  std::thread destroy([owned = std::move(vectors)]() mutable { owned.clear(); });
  destroy.join();
}

struct VectorOwner {
  RelocationVector relocs;
  ~VectorOwner() { EXPECT_EQ(37U, relocs.front().getOffset()); }
};

TEST_F(RelocationStorageTest, ContextLifetimeAndRepeatedLinks) {
  for (unsigned threads : {1U, 2U, 8U, 16U, 1U}) {
    CommonLinkerContext::destroy();
    llvm::parallel::strategy = llvm::hardware_concurrency(threads);
    new CommonLinkerContext;
    initRelocationBufferSlabs();
    VectorOwner *owner = make<VectorOwner>();
    owner->relocs.resize(255);
    owner->relocs.front().setOffset(37);
    // The context runs arena-owned vector destructors while slabs are live.
    // Separately exercise raw frees after cleanup: deallocation must not
    // consult the dead context, its config, or an executor thread count.
    RelocationAllocator<Relocation> alloc;
    Relocation *small = alloc.allocate(255), *large = alloc.allocate(256);
    CommonLinkerContext::destroy();
    EXPECT_FALSE(hasContext());
    std::thread destroy([small, large] {
      RelocationAllocator<Relocation> alloc;
      alloc.deallocate(small, 255);
      alloc.deallocate(large, 256);
    });
    destroy.join();
  }
}

// Exercise the public entry point, including context initialization/cleanup,
// from a worker of a persistent executor created before the serial link.
TEST(RelocationLibraryTest, SerialLinkOnExecutorWorkerAndReuse) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "Requires the default executor";
#endif
  auto savedStrategy = llvm::parallel::strategy;
  auto restoreStrategy = llvm::scope_exit(
      [&] { llvm::parallel::strategy = savedStrategy; });
  llvm::parallel::strategy = llvm::hardware_concurrency(2);

  // llvm-mc -filetype=obj -triple=arm64-apple-macos input.s
  // .text; .globl _main; _main: ret; .data; .quad _main
  // The data relocation requires a small relocation-vector allocation.
  static const char object[] =
      "\xcf\xfa\xed\xfe\x0c\x00\x00\x01\x00\x00\x00\x00\x01\x00\x00\x00"
      "\x03\x00\x00\x00\x50\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x19\x00\x00\x00\xe8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x0c\x00\x00\x00\x00\x00\x00\x00\x70\x01\x00\x00\x00\x00\x00\x00"
      "\x0c\x00\x00\x00\x00\x00\x00\x00\x07\x00\x00\x00\x07\x00\x00\x00"
      "\x02\x00\x00\x00\x00\x00\x00\x00\x5f\x5f\x74\x65\x78\x74\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x5f\x5f\x54\x45\x58\x54\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x04\x00\x00\x00\x00\x00\x00\x00\x70\x01\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x80\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x5f\x5f\x64\x61\x74\x61\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x5f\x5f\x44\x41\x54\x41\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00"
      "\x08\x00\x00\x00\x00\x00\x00\x00\x74\x01\x00\x00\x00\x00\x00\x00"
      "\x80\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x18\x00\x00\x00"
      "\x88\x01\x00\x00\x03\x00\x00\x00\xb8\x01\x00\x00\x18\x00\x00\x00"
      "\x0b\x00\x00\x00\x50\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00"
      "\x02\x00\x00\x00\x01\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\xc0\x03\x5f\xd6\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x02\x00\x00\x0e\x0d\x00\x00\x00\x0e\x01\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x07\x00\x00\x00\x0e\x02\x00\x00"
      "\x04\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x0f\x01\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x5f\x6d\x61\x69\x6e\x00\x6c"
      "\x74\x6d\x70\x31\x00\x6c\x74\x6d\x70\x30\x00\x00\x00\x00\x00\x00";
  llvm::SmallString<128> input, output;
  int fd;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("lld-reloc-worker", "o", fd,
                                                 input));
  auto removeInput =
      llvm::scope_exit([&] { llvm::sys::fs::remove(input); });
  {
    llvm::raw_fd_ostream os(fd, true);
    os.write(object, sizeof(object) - 1);
  }
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("lld-reloc-worker", "out",
                                                 output));
  auto removeOutput =
      llvm::scope_exit([&] { llvm::sys::fs::remove(output); });
  {
    llvm::parallel::TaskGroup tasks;
    tasks.spawn([&] {
      ASSERT_NE(UINT_MAX, llvm::parallel::getWorkerIndex());
      for (const char *threads : {"--threads=1", "--threads=1", "--threads=8",
                                  "--threads=1"}) {
        SCOPED_TRACE(threads);
        const char *args[] = {"ld64.lld", "-arch", "arm64", "-platform_version",
                              "macos", "11.0", "11.0", threads, input.c_str(),
                              "-o", output.c_str()};
        const DriverDef drivers[] = {{Darwin, &macho::link}};
        std::string out, err;
        llvm::raw_string_ostream stdoutOS(out), stderrOS(err);
        auto result = lldMain(args, stdoutOS, stderrOS, drivers);
        ASSERT_TRUE(result.canRunAgain) << err;
        ASSERT_EQ(0, result.retCode) << err;
        EXPECT_FALSE(hasContext());
        uint64_t size = 0;
        EXPECT_FALSE(llvm::sys::fs::file_size(output, size));
        EXPECT_GT(size, 0U);
      }
    });
  }
}
} // namespace
