# REQUIRES: aarch64
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/undef.s -o %t/undef.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/lazy.s -o %t/lazy.o
# RUN: llvm-ar crs %t/lazy.a %t/lazy.o
# RUN: mkdir -p %t/1 %t/2 %t/8 %t/16
# RUN: %lld -arch arm64 -no_uuid --threads=1 %t/main.o %t/undef.o -o %t/1/out
# RUN: %lld -arch arm64 -no_uuid --threads=2 %t/main.o %t/undef.o -o %t/2/out
# RUN: %lld -arch arm64 -no_uuid --threads=8 %t/main.o %t/undef.o -o %t/8/out
# RUN: %lld -arch arm64 -no_uuid --threads=16 %t/main.o %t/undef.o -o %t/16/out
# RUN: cmp %t/1/out %t/2/out
# RUN: cmp %t/1/out %t/8/out
# RUN: cmp %t/1/out %t/16/out
# RUN: llvm-nm %t/8/out | FileCheck %s

## A lazy archive name can be replaced too. It is registered before _early,
## whereas a newly appended thunk would follow _early in the string table.
# RUN: %lld -arch arm64 -no_uuid --threads=1 %t/lazy.a %t/main.o -o %t/1/out
# RUN: %lld -arch arm64 -no_uuid --threads=8 %t/lazy.a %t/main.o -o %t/8/out
# RUN: cmp %t/1/out %t/8/out
# RUN: llvm-nm %t/8/out | FileCheck %s

## The fallback must also work without the batched string-table path, and when
## private externs (including the thunk) are omitted from the symbol table.
# RUN: %lld -arch arm64 -no_uuid --threads=1 -no-deduplicate-symbol-strings %t/undef.o %t/main.o -o %t/1/out
# RUN: %lld -arch arm64 -no_uuid --threads=8 -no-deduplicate-symbol-strings %t/undef.o %t/main.o -o %t/8/out
# RUN: cmp %t/1/out %t/8/out
# RUN: %lld -arch arm64 -no_uuid --threads=1 -x %t/main.o %t/undef.o -o %t/1/out
# RUN: %lld -arch arm64 -no_uuid --threads=8 -x %t/main.o %t/undef.o -o %t/8/out
# RUN: cmp %t/1/out %t/8/out

# CHECK-DAG: T _early
# CHECK-DAG: t _early.thunk.0
# CHECK-DAG: T _main

## One out-of-range backward call suffices. The unused undefined symbol is
## replaced by the generated thunk, rather than appended to the symtab. Both
## symbol collection and function-start collection must see the replacement.
#--- main.s
.text
.subsections_via_symbols
.globl _early, _main
_early:
  ret
.space 0x8000000
_main:
  bl _early
  ret

#--- undef.s
.globl _early.thunk.0

#--- lazy.s
.globl _early.thunk.0
_early.thunk.0:
  ret
