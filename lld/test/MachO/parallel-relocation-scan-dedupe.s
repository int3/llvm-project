# REQUIRES: x86

## Mechanism 20's parallel relocation classification drops repeat references to
## a stub/GOT/TLV entry within a chunk, because those addEntry() calls are
## idempotent. Check that entry indices, bindings and diagnostics are unchanged
## by thread count, across chunk boundaries (128 input sections per chunk) and
## for both the classic and chained-fixups paths.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/foo.s -o %t/foo.o
# RUN: %lld -dylib -lSystem -install_name @rpath/libfoo.dylib -o %t/libfoo.dylib %t/foo.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/main.s -o %t/main.o

## Classic (opcode) bindings and rebases.
# RUN: mkdir -p %t/t1 %t/t2 %t/t8
# RUN: %lld -no_uuid -lSystem %t/libfoo.dylib --threads=1 %t/main.o -o %t/t1/out
# RUN: %lld -no_uuid -lSystem %t/libfoo.dylib --threads=2 %t/main.o -o %t/t2/out
# RUN: %lld -no_uuid -lSystem %t/libfoo.dylib --threads=8 %t/main.o -o %t/t8/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: llvm-objdump --macho --bind --lazy-bind --weak-bind --rebase %t/t1/out | \
# RUN:   FileCheck %s --check-prefix=CLASSIC

## Chained fixups.
# RUN: mkdir -p %t/c1 %t/c8
# RUN: %lld -no_uuid -lSystem -fixup_chains %t/libfoo.dylib --threads=1 %t/main.o -o %t/c1/out
# RUN: %lld -no_uuid -lSystem -fixup_chains %t/libfoo.dylib --threads=8 %t/main.o -o %t/c8/out
# RUN: cmp %t/c1/out %t/c8/out
# RUN: llvm-objdump --macho --dyld-info %t/c1/out | FileCheck %s --check-prefix=CHAINED

## Undefined-symbol diagnostics keep their order regardless of thread count.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/undef.s -o %t/undef.o
# RUN: not %lld -lSystem --threads=1 %t/undef.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNDEF
# RUN: not %lld -lSystem --threads=8 %t/undef.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNDEF

## GOT and stub entries are assigned in first-reference order: _fa/_ga are
## referenced from the first section, _fb/_gb only from the last one. The
## weak-def and interposable UNSIGNED references are not entry creations and
## must still be replayed individually.

## The checks below are ordered, not -DAG: they pin the first-encounter order of
## _ga before _gb in the GOT and of _fa before _fb among the stubs.

# CLASSIC:      Rebase table:
# CLASSIC:      Bind table:
## _tfoo's TLV reference takes a __got slot (there is no __thread_ptrs).
# CLASSIC:      __DATA_CONST __got {{.*}} _tfoo
# CLASSIC:      __DATA_CONST __got {{.*}} _ga
# CLASSIC:      __DATA_CONST __got {{.*}} _gb
# CLASSIC:      Lazy bind table:
# CLASSIC:      __la_symbol_ptr {{.*}} _fa
# CLASSIC:      __la_symbol_ptr {{.*}} _fb

# CHAINED:      dyld information:
# CHAINED:      __DATA_CONST __got {{.*}} _ga
# CHAINED:      __DATA_CONST __got {{.*}} _fa
# CHAINED:      __DATA_CONST __got {{.*}} _gb
# CHAINED:      __DATA_CONST __got {{.*}} _fb

## Undefined referents are never deduplicated: _undef1 keeps both of its
## reference lines, in the same order, at every thread count.
# UNDEF:      error: undefined symbol: _undef2
# UNDEF-NEXT: >>> referenced by {{.*}}undef.o:(symbol _main+0xb)
# UNDEF:      error: undefined symbol: _undef1
# UNDEF-NEXT: >>> referenced by {{.*}}undef.o:(symbol _main+0x6)
# UNDEF-NEXT: >>> referenced by {{.*}}undef.o:(symbol _main+0x1)

#--- foo.s
.globl _fa, _fb
_fa:
  retq
_fb:
  retq

.section __DATA,__data
.globl _ga, _gb
_ga:
  .quad 1
_gb:
  .quad 2

.section __DATA,__thread_data,thread_local_regular
_tfoo$tlv$init:
  .quad 123
.section __DATA,__thread_vars,thread_local_variables
.globl _tfoo
_tfoo:
  .quad __tlv_bootstrap
  .quad 0
  .quad _tfoo$tlv$init

#--- main.s
.subsections_via_symbols

.section __TEXT,__text,regular,pure_instructions
.globl _main
_main:
## First references: _fa (stub), _ga (GOT), _tfoo (TLV pointer).
  callq _fa
  callq _fa
  movq _ga@GOTPCREL(%rip), %rax
  movq _ga@GOTPCREL(%rip), %rax
  movq _tfoo@TLVP(%rip), %rax
  movq _tfoo@TLVP(%rip), %rax
  callq _repeat0
  callq _last
  retq

## 300 sections of repeat references, spanning three 128-section chunks. Every
## one of these is a within-chunk repeat after the first in its chunk.
.macro emit_repeat
.globl _repeat\@
_repeat\@:
  callq _fa
  callq _fa
  movq _ga@GOTPCREL(%rip), %rax
  movq _tfoo@TLVP(%rip), %rax
  retq
.endm
.rept 300
emit_repeat
.endr

## Last section introduces _fb and _gb, so their entry indices come last.
.globl _last
_last:
  callq _fb
  movq _gb@GOTPCREL(%rip), %rax
  callq _fb
  movq _gb@GOTPCREL(%rip), %rax
  retq

## UNSIGNED references: a weak definition and a local, repeated. These are not
## entry creations and must be replayed one by one.
.globl _weak
.weak_definition _weak
_weak:
  retq

.section __DATA,__data
.globl _ptrs
_ptrs:
  .quad _weak
  .quad _weak
  .quad _main
  .quad _main
  .quad _ga
  .quad _ga

#--- undef.s
.globl _main
_main:
  callq _undef1
  callq _undef1
  callq _undef2
  retq
