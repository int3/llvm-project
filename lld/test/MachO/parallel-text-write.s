# REQUIRES: aarch64
# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/b.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/data.s -o %t/data.o
# RUN: %lld -dylib -arch arm64 -lSystem --threads=1 -o %t/libfoo.dylib %t/main.o %t/a.o %t/b.o %t/data.o
# RUN: cp %t/libfoo.dylib %t/serial.dylib
# RUN: %lld -dylib -arch arm64 -lSystem --threads=4 -o %t/libfoo.dylib %t/main.o %t/a.o %t/b.o %t/data.o
# RUN: cmp %t/serial.dylib %t/libfoo.dylib
# RUN: llvm-objdump --syms %t/libfoo.dylib | FileCheck %s

# CHECK: _main
# CHECK: _a
# CHECK: _b
# CHECK: _data

#--- main.s
.text
.globl _main
_main:
  bl _a
  bl _b
  ret

#--- a.s
.text
.globl _a
_a:
  adrp x0, _data@PAGE
  ldr x0, [x0, _data@PAGEOFF]
  ret

#--- b.s
.text
.globl _b
_b:
  bl _a
  ret

#--- data.s
.data
.globl _data
_data:
  .quad 0
