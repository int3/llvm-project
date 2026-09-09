# REQUIRES: x86

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: mkdir -p %t/t1 %t/t4
# RUN: %lld -no_uuid -lSystem -undefined dynamic_lookup --threads=1 %t/main.o -o %t/t1/out
# RUN: %lld -no_uuid -lSystem -undefined dynamic_lookup --threads=4 %t/main.o -o %t/t4/out
# RUN: cmp %t/t1/out %t/t4/out
# RUN: llvm-objdump --macho --syms %t/t1/out | FileCheck %s

# CHECK: __TEXT,__text _main
# CHECK: __TEXT,__text _target

#--- main.s
.subsections_via_symbols
.section __TEXT,__text,regular,pure_instructions

.macro emit_relocation
.globl _reloc\@
_reloc\@:
  callq _target
  retq
.endm

.rept 256
emit_relocation
.endr

.globl _target
_target:
  retq

.globl _main
_main:
  callq _reloc0
  callq _dynamic
  retq
