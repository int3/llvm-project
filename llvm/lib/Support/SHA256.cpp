//====- SHA256.cpp - SHA256 implementation ---*- C++ -* ======//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/*
 *  The SHA-256 Secure Hash Standard was published by NIST in 2002.
 *
 *  http://csrc.nist.gov/publications/fips/fips180-2/fips180-2.pdf
 *
 *   The implementation is based on nacl's sha256 implementation [0] and LLVM's
 *  pre-exsiting SHA1 code [1].
 *
 *   [0] https://hyperelliptic.org/nacl/nacl-20110221.tar.bz2 (public domain
 *       code)
 *   [1] llvm/lib/Support/SHA1.{h,cpp}
 */
//===----------------------------------------------------------------------===//

#include "llvm/Support/SHA256.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SwapByteOrder.h"
#include <string.h>

#if LLVM_IS_X86 && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#define LLVM_SHA256_HAS_SHA_NI 1
#define LLVM_SHA256_TARGET_SHA_NI __attribute__((target("sha,sse2")))
#else
#define LLVM_SHA256_HAS_SHA_NI 0
#endif

namespace llvm {

#define SHR(x, c) ((x) >> (c))
#define ROTR(x, n) (((x) >> n) | ((x) << (32 - (n))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define SIGMA_0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIGMA_1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))

#define SIGMA_2(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define SIGMA_3(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))

#define F_EXPAND(A, B, C, D, E, F, G, H, M1, M2, M3, M4, k)                    \
  do {                                                                         \
    H += SIGMA_1(E) + CH(E, F, G) + M1 + k;                                    \
    D += H;                                                                    \
    H += SIGMA_0(A) + MAJ(A, B, C);                                            \
    M1 += SIGMA_2(M2) + M3 + SIGMA_3(M4);                                      \
  } while (0);

#if LLVM_SHA256_HAS_SHA_NI
namespace {

// Buffer.L already holds host-order SHA words. Keep this helper private so
// callers without SHA-NI continue to use the scalar implementation below.
LLVM_SHA256_TARGET_SHA_NI static void hashBlockSHA(uint32_t *State,
                                                   const uint32_t *Words) {
  static constexpr uint32_t K[] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  static_assert(sizeof(K) / sizeof(K[0]) == 64);

  __m128i StateABCD = _mm_loadu_si128(reinterpret_cast<const __m128i *>(State));
  __m128i StateEFGH =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(State + 4));
  StateABCD = _mm_shuffle_epi32(StateABCD, 0xb1);
  StateEFGH = _mm_shuffle_epi32(StateEFGH, 0xb1);
  __m128i State0 = _mm_unpacklo_epi64(StateEFGH, StateABCD);
  __m128i State1 = _mm_unpackhi_epi64(StateEFGH, StateABCD);

  const __m128i State0Save = State0;
  const __m128i State1Save = State1;
  __m128i Msg0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Words + 0));
  __m128i Msg1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Words + 4));
  __m128i Msg2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Words + 8));
  __m128i Msg3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Words + 12));
  for (unsigned I = 0; I != 16; ++I) {
    __m128i Message = Msg0;
    __m128i Rounds = _mm_add_epi32(
        Message, _mm_loadu_si128(reinterpret_cast<const __m128i *>(K + I * 4)));
    State1 = _mm_sha256rnds2_epu32(State1, State0, Rounds);
    State0 =
        _mm_sha256rnds2_epu32(State0, State1, _mm_shuffle_epi32(Rounds, 0x0e));

    if (I == 15)
      break;

    __m128i Next = _mm_sha256msg1_epu32(Msg0, Msg1);
    Next = _mm_add_epi32(
        Next, _mm_or_si128(_mm_srli_si128(Msg2, 4), _mm_slli_si128(Msg3, 12)));
    Next = _mm_sha256msg2_epu32(Next, Msg3);
    Msg0 = Msg1;
    Msg1 = Msg2;
    Msg2 = Msg3;
    Msg3 = Next;
  }

  State0 = _mm_add_epi32(State0, State0Save);
  State1 = _mm_add_epi32(State1, State1Save);
  StateABCD = _mm_shuffle_epi32(_mm_unpackhi_epi64(State0, State1), 0xb1);
  StateEFGH = _mm_shuffle_epi32(_mm_unpacklo_epi64(State0, State1), 0xb1);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(State), StateABCD);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(State + 4), StateEFGH);
}

} // namespace
#endif

void SHA256::init() {
  InternalState.State[0] = 0x6A09E667;
  InternalState.State[1] = 0xBB67AE85;
  InternalState.State[2] = 0x3C6EF372;
  InternalState.State[3] = 0xA54FF53A;
  InternalState.State[4] = 0x510E527F;
  InternalState.State[5] = 0x9B05688C;
  InternalState.State[6] = 0x1F83D9AB;
  InternalState.State[7] = 0x5BE0CD19;
  InternalState.ByteCount = 0;
  InternalState.BufferOffset = 0;
}

void SHA256::hashBlock() {
#if LLVM_SHA256_HAS_SHA_NI
  static const bool HasSHA =
      LLVM_CPU_SUPPORTS("sha") && LLVM_CPU_SUPPORTS("sse2");
  if (HasSHA) {
    hashBlockSHA(InternalState.State, InternalState.Buffer.L);
    return;
  }
#endif

  uint32_t A = InternalState.State[0];
  uint32_t B = InternalState.State[1];
  uint32_t C = InternalState.State[2];
  uint32_t D = InternalState.State[3];
  uint32_t E = InternalState.State[4];
  uint32_t F = InternalState.State[5];
  uint32_t G = InternalState.State[6];
  uint32_t H = InternalState.State[7];

  uint32_t W00 = InternalState.Buffer.L[0];
  uint32_t W01 = InternalState.Buffer.L[1];
  uint32_t W02 = InternalState.Buffer.L[2];
  uint32_t W03 = InternalState.Buffer.L[3];
  uint32_t W04 = InternalState.Buffer.L[4];
  uint32_t W05 = InternalState.Buffer.L[5];
  uint32_t W06 = InternalState.Buffer.L[6];
  uint32_t W07 = InternalState.Buffer.L[7];
  uint32_t W08 = InternalState.Buffer.L[8];
  uint32_t W09 = InternalState.Buffer.L[9];
  uint32_t W10 = InternalState.Buffer.L[10];
  uint32_t W11 = InternalState.Buffer.L[11];
  uint32_t W12 = InternalState.Buffer.L[12];
  uint32_t W13 = InternalState.Buffer.L[13];
  uint32_t W14 = InternalState.Buffer.L[14];
  uint32_t W15 = InternalState.Buffer.L[15];

  F_EXPAND(A, B, C, D, E, F, G, H, W00, W14, W09, W01, 0x428A2F98);
  F_EXPAND(H, A, B, C, D, E, F, G, W01, W15, W10, W02, 0x71374491);
  F_EXPAND(G, H, A, B, C, D, E, F, W02, W00, W11, W03, 0xB5C0FBCF);
  F_EXPAND(F, G, H, A, B, C, D, E, W03, W01, W12, W04, 0xE9B5DBA5);
  F_EXPAND(E, F, G, H, A, B, C, D, W04, W02, W13, W05, 0x3956C25B);
  F_EXPAND(D, E, F, G, H, A, B, C, W05, W03, W14, W06, 0x59F111F1);
  F_EXPAND(C, D, E, F, G, H, A, B, W06, W04, W15, W07, 0x923F82A4);
  F_EXPAND(B, C, D, E, F, G, H, A, W07, W05, W00, W08, 0xAB1C5ED5);
  F_EXPAND(A, B, C, D, E, F, G, H, W08, W06, W01, W09, 0xD807AA98);
  F_EXPAND(H, A, B, C, D, E, F, G, W09, W07, W02, W10, 0x12835B01);
  F_EXPAND(G, H, A, B, C, D, E, F, W10, W08, W03, W11, 0x243185BE);
  F_EXPAND(F, G, H, A, B, C, D, E, W11, W09, W04, W12, 0x550C7DC3);
  F_EXPAND(E, F, G, H, A, B, C, D, W12, W10, W05, W13, 0x72BE5D74);
  F_EXPAND(D, E, F, G, H, A, B, C, W13, W11, W06, W14, 0x80DEB1FE);
  F_EXPAND(C, D, E, F, G, H, A, B, W14, W12, W07, W15, 0x9BDC06A7);
  F_EXPAND(B, C, D, E, F, G, H, A, W15, W13, W08, W00, 0xC19BF174);

  F_EXPAND(A, B, C, D, E, F, G, H, W00, W14, W09, W01, 0xE49B69C1);
  F_EXPAND(H, A, B, C, D, E, F, G, W01, W15, W10, W02, 0xEFBE4786);
  F_EXPAND(G, H, A, B, C, D, E, F, W02, W00, W11, W03, 0x0FC19DC6);
  F_EXPAND(F, G, H, A, B, C, D, E, W03, W01, W12, W04, 0x240CA1CC);
  F_EXPAND(E, F, G, H, A, B, C, D, W04, W02, W13, W05, 0x2DE92C6F);
  F_EXPAND(D, E, F, G, H, A, B, C, W05, W03, W14, W06, 0x4A7484AA);
  F_EXPAND(C, D, E, F, G, H, A, B, W06, W04, W15, W07, 0x5CB0A9DC);
  F_EXPAND(B, C, D, E, F, G, H, A, W07, W05, W00, W08, 0x76F988DA);
  F_EXPAND(A, B, C, D, E, F, G, H, W08, W06, W01, W09, 0x983E5152);
  F_EXPAND(H, A, B, C, D, E, F, G, W09, W07, W02, W10, 0xA831C66D);
  F_EXPAND(G, H, A, B, C, D, E, F, W10, W08, W03, W11, 0xB00327C8);
  F_EXPAND(F, G, H, A, B, C, D, E, W11, W09, W04, W12, 0xBF597FC7);
  F_EXPAND(E, F, G, H, A, B, C, D, W12, W10, W05, W13, 0xC6E00BF3);
  F_EXPAND(D, E, F, G, H, A, B, C, W13, W11, W06, W14, 0xD5A79147);
  F_EXPAND(C, D, E, F, G, H, A, B, W14, W12, W07, W15, 0x06CA6351);
  F_EXPAND(B, C, D, E, F, G, H, A, W15, W13, W08, W00, 0x14292967);

  F_EXPAND(A, B, C, D, E, F, G, H, W00, W14, W09, W01, 0x27B70A85);
  F_EXPAND(H, A, B, C, D, E, F, G, W01, W15, W10, W02, 0x2E1B2138);
  F_EXPAND(G, H, A, B, C, D, E, F, W02, W00, W11, W03, 0x4D2C6DFC);
  F_EXPAND(F, G, H, A, B, C, D, E, W03, W01, W12, W04, 0x53380D13);
  F_EXPAND(E, F, G, H, A, B, C, D, W04, W02, W13, W05, 0x650A7354);
  F_EXPAND(D, E, F, G, H, A, B, C, W05, W03, W14, W06, 0x766A0ABB);
  F_EXPAND(C, D, E, F, G, H, A, B, W06, W04, W15, W07, 0x81C2C92E);
  F_EXPAND(B, C, D, E, F, G, H, A, W07, W05, W00, W08, 0x92722C85);
  F_EXPAND(A, B, C, D, E, F, G, H, W08, W06, W01, W09, 0xA2BFE8A1);
  F_EXPAND(H, A, B, C, D, E, F, G, W09, W07, W02, W10, 0xA81A664B);
  F_EXPAND(G, H, A, B, C, D, E, F, W10, W08, W03, W11, 0xC24B8B70);
  F_EXPAND(F, G, H, A, B, C, D, E, W11, W09, W04, W12, 0xC76C51A3);
  F_EXPAND(E, F, G, H, A, B, C, D, W12, W10, W05, W13, 0xD192E819);
  F_EXPAND(D, E, F, G, H, A, B, C, W13, W11, W06, W14, 0xD6990624);
  F_EXPAND(C, D, E, F, G, H, A, B, W14, W12, W07, W15, 0xF40E3585);
  F_EXPAND(B, C, D, E, F, G, H, A, W15, W13, W08, W00, 0x106AA070);

  F_EXPAND(A, B, C, D, E, F, G, H, W00, W14, W09, W01, 0x19A4C116);
  F_EXPAND(H, A, B, C, D, E, F, G, W01, W15, W10, W02, 0x1E376C08);
  F_EXPAND(G, H, A, B, C, D, E, F, W02, W00, W11, W03, 0x2748774C);
  F_EXPAND(F, G, H, A, B, C, D, E, W03, W01, W12, W04, 0x34B0BCB5);
  F_EXPAND(E, F, G, H, A, B, C, D, W04, W02, W13, W05, 0x391C0CB3);
  F_EXPAND(D, E, F, G, H, A, B, C, W05, W03, W14, W06, 0x4ED8AA4A);
  F_EXPAND(C, D, E, F, G, H, A, B, W06, W04, W15, W07, 0x5B9CCA4F);
  F_EXPAND(B, C, D, E, F, G, H, A, W07, W05, W00, W08, 0x682E6FF3);
  F_EXPAND(A, B, C, D, E, F, G, H, W08, W06, W01, W09, 0x748F82EE);
  F_EXPAND(H, A, B, C, D, E, F, G, W09, W07, W02, W10, 0x78A5636F);
  F_EXPAND(G, H, A, B, C, D, E, F, W10, W08, W03, W11, 0x84C87814);
  F_EXPAND(F, G, H, A, B, C, D, E, W11, W09, W04, W12, 0x8CC70208);
  F_EXPAND(E, F, G, H, A, B, C, D, W12, W10, W05, W13, 0x90BEFFFA);
  F_EXPAND(D, E, F, G, H, A, B, C, W13, W11, W06, W14, 0xA4506CEB);
  F_EXPAND(C, D, E, F, G, H, A, B, W14, W12, W07, W15, 0xBEF9A3F7);
  F_EXPAND(B, C, D, E, F, G, H, A, W15, W13, W08, W00, 0xC67178F2);

  InternalState.State[0] += A;
  InternalState.State[1] += B;
  InternalState.State[2] += C;
  InternalState.State[3] += D;
  InternalState.State[4] += E;
  InternalState.State[5] += F;
  InternalState.State[6] += G;
  InternalState.State[7] += H;
}

void SHA256::addUncounted(uint8_t Data) {
  if constexpr (sys::IsBigEndianHost)
    InternalState.Buffer.C[InternalState.BufferOffset] = Data;
  else
    InternalState.Buffer.C[InternalState.BufferOffset ^ 3] = Data;

  InternalState.BufferOffset++;
  if (InternalState.BufferOffset == BLOCK_LENGTH) {
    hashBlock();
    InternalState.BufferOffset = 0;
  }
}

void SHA256::writebyte(uint8_t Data) {
  ++InternalState.ByteCount;
  addUncounted(Data);
}

void SHA256::update(ArrayRef<uint8_t> Data) {
  InternalState.ByteCount += Data.size();

  // Finish the current block.
  if (InternalState.BufferOffset > 0) {
    const size_t Remainder = std::min<size_t>(
        Data.size(), BLOCK_LENGTH - InternalState.BufferOffset);
    for (size_t I = 0; I < Remainder; ++I)
      addUncounted(Data[I]);
    Data = Data.drop_front(Remainder);
  }

  // Fast buffer filling for large inputs.
  while (Data.size() >= BLOCK_LENGTH) {
    assert(InternalState.BufferOffset == 0);
    static_assert(BLOCK_LENGTH % 4 == 0);
    constexpr size_t BLOCK_LENGTH_32 = BLOCK_LENGTH / 4;
    for (size_t I = 0; I < BLOCK_LENGTH_32; ++I)
      InternalState.Buffer.L[I] = support::endian::read32be(&Data[I * 4]);
    hashBlock();
    Data = Data.drop_front(BLOCK_LENGTH);
  }

  // Finish the remainder.
  for (uint8_t C : Data)
    addUncounted(C);
}

void SHA256::update(StringRef Str) {
  update(
      ArrayRef<uint8_t>((uint8_t *)const_cast<char *>(Str.data()), Str.size()));
}

void SHA256::pad() {
  // Implement SHA-2 padding (fips180-2 5.1.1)

  // Pad with 0x80 followed by 0x00 until the end of the block
  addUncounted(0x80);
  while (InternalState.BufferOffset != 56)
    addUncounted(0x00);

  uint64_t len = InternalState.ByteCount << 3; // bit size

  // Append length in the last 8 bytes big edian encoded
  addUncounted(len >> 56);
  addUncounted(len >> 48);
  addUncounted(len >> 40);
  addUncounted(len >> 32);
  addUncounted(len >> 24);
  addUncounted(len >> 16);
  addUncounted(len >> 8);
  addUncounted(len);
}

void SHA256::final(std::array<uint32_t, HASH_LENGTH / 4> &HashResult) {
  // Pad to complete the last block
  pad();

  if constexpr (sys::IsBigEndianHost) {
    // Just copy the current state
    for (int i = 0; i < 8; i++) {
      HashResult[i] = InternalState.State[i];
    }
  } else {
    // Swap byte order back
    for (int i = 0; i < 8; i++) {
      HashResult[i] = llvm::byteswap(InternalState.State[i]);
    }
  }
}

std::array<uint8_t, 32> SHA256::final() {
  union {
    std::array<uint32_t, HASH_LENGTH / 4> HashResult;
    std::array<uint8_t, HASH_LENGTH> ReturnResult;
  };
  static_assert(sizeof(HashResult) == sizeof(ReturnResult));
  final(HashResult);
  return ReturnResult;
}

std::array<uint8_t, 32> SHA256::result() {
  auto StateToRestore = InternalState;

  auto Hash = final();

  // Restore the state
  InternalState = StateToRestore;

  // Return pointer to hash (32 characters)
  return Hash;
}

std::array<uint8_t, 32> SHA256::hash(ArrayRef<uint8_t> Data) {
  SHA256 Hash;
  Hash.update(Data);
  return Hash.final();
}

} // namespace llvm
