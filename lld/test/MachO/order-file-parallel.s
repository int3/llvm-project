# REQUIRES: x86
## The order file is matched against the symbols of 64 input files at a time in
## parallel. Spread the ordered symbols over two such chunks, and check that
## the output is the same at one and eight threads.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin first.s -o first.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin last.s -o last.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin member.s -o member.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin empty.s -o empty.o
# RUN: llvm-ar rcs libx.a member.o
# RUN: mkdir t1 t8
# RUN: %lld -e _main -order_file order --threads=1 -o t1/out first.o @64-empty.rsp last.o -force_load libx.a
# RUN: %lld -e _main -order_file order --threads=8 -o t8/out first.o @64-empty.rsp last.o -force_load libx.a
# RUN: cmp t1/out t8/out
# RUN: llvm-nm --numeric-sort --format=just-symbols t1/out | FileCheck %s

## _amem0 and _amem1 share a section, and the file-qualified entry for _amem1
## gives it the higher priority.
# CHECK:      _last
# CHECK-NEXT: _amem0
# CHECK-NEXT: _amem1
# CHECK-NEXT: _first
# CHECK-NEXT: _main

#--- order
_last
libx.a(member.o):_amem1
_first
_amem0

#--- first.s
.subsections_via_symbols
.globl _main, _first
_main:
  ret
_first:
  ret

#--- last.s
.globl _last
_last:
  ret

#--- member.s
.globl _amem0, _amem1
_amem0:
  ret
_amem1:
  ret

#--- empty.s

#--- 64-empty.rsp
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
empty.o empty.o empty.o empty.o empty.o empty.o empty.o empty.o
