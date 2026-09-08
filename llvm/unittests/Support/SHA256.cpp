//===- llvm/unittest/Support/SHA256Test.cpp - SHA256 tests
//----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements unit tests for the SHA256 functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/SHA256.h"
#include "llvm/ADT/ArrayRef.h"
#include "gtest/gtest.h"
#include <vector>

using namespace llvm;

namespace {

static std::string toHex(ArrayRef<uint8_t> Input) {
  static const char *const LUT = "0123456789abcdef";
  size_t Length = Input.size();

  std::string Output;
  Output.reserve(2 * Length);
  for (size_t i = 0; i < Length; ++i) {
    const unsigned char c = Input[i];
    Output.push_back(LUT[c >> 4]);
    Output.push_back(LUT[c & 15]);
  }
  return Output;
}

/// Tests an arbitrary set of bytes passed as \p Input.
void TestSHA256Sum(ArrayRef<uint8_t> Input, StringRef Final) {
  SHA256 Hash;
  Hash.update(Input);
  auto hash = Hash.final();
  auto hashStr = toHex(hash);
  EXPECT_EQ(hashStr, Final);
}

std::vector<uint8_t> makeBlockBoundaryInput(size_t Size) {
  std::vector<uint8_t> Input(Size);
  for (size_t I = 0; I != Size; ++I)
    Input[I] = static_cast<uint8_t>(I * 37 + 11);
  return Input;
}

using KV = std::pair<const char *, const char *>;

TEST(SHA256Test, SHA256) {
  std::array<KV, 5> testvectors{
      KV{"",
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      KV{"a",
         "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
      KV{"abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      KV{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      KV{"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklm"
         "nopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"}};

  for (auto input_expected : testvectors) {
    auto str = std::get<0>(input_expected);
    auto expected = std::get<1>(input_expected);
    TestSHA256Sum({reinterpret_cast<const uint8_t *>(str), strlen(str)},
                  expected);
  }

  std::string rep(1000, 'a');
  SHA256 Hash;
  for (int i = 0; i < 1000; ++i) {
    Hash.update({reinterpret_cast<const uint8_t *>(rep.data()), rep.size()});
  }
  auto hash = Hash.final();
  auto hashStr = toHex(hash);
  EXPECT_EQ(hashStr,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(SHA256Test, BlockBoundariesAndUnalignedUpdates) {
  struct TestVector {
    size_t Size;
    StringRef Digest;
  };
  const TestVector Vectors[] = {
      {55, "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
      {56, "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
      {63, "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
      {64, "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
      {65, "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
      {4095,
       "14247e71abeac04b1bb56818238f449fe1339e1d9b347ade9224b12460f39d1c"},
      {4096,
       "4e441a3533bb2c10cd5649981d395744213e09a336746b5a3458fee4057205ec"},
      {4097,
       "042a02b55ba342cdd30322331816b6e4bca073f33bdb80149dc74a6f949802f4"},
  };

  for (const TestVector &Vector : Vectors) {
    std::vector<uint8_t> Input = makeBlockBoundaryInput(Vector.Size);
    for (size_t Offset : {size_t(0), size_t(1), size_t(3), size_t(7)}) {
      std::vector<uint8_t> Storage(Vector.Size + Offset);
      for (size_t I = 0; I != Vector.Size; ++I)
        Storage[Offset + I] = Input[I];
      ArrayRef<uint8_t> Unaligned(Storage.data() + Offset, Vector.Size);
      TestSHA256Sum(Unaligned, Vector.Digest);

      SHA256 Hash;
      const size_t Split = Vector.Size / 2;
      Hash.update(Unaligned.take_front(Split));
      Hash.update(Unaligned.drop_front(Split));
      EXPECT_EQ(toHex(Hash.final()), Vector.Digest) << Vector.Size << Offset;
    }
  }
}

TEST(SHA256Test, ResultThenContinueAndInitReuse) {
  constexpr StringRef PrefixDigest =
      "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e";
  constexpr StringRef FullDigest =
      "042a02b55ba342cdd30322331816b6e4bca073f33bdb80149dc74a6f949802f4";
  std::vector<uint8_t> Input = makeBlockBoundaryInput(4097);

  SHA256 Hash;
  Hash.update(ArrayRef<uint8_t>(Input).take_front(64));
  EXPECT_EQ(toHex(Hash.result()), PrefixDigest);
  Hash.update(ArrayRef<uint8_t>(Input).drop_front(64));
  EXPECT_EQ(toHex(Hash.final()), FullDigest);

  Hash.init();
  Hash.update(Input);
  EXPECT_EQ(toHex(Hash.final()), FullDigest);
}

} // namespace
