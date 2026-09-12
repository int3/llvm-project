# REQUIRES: aarch64

## The LINKEDIT function-starts address fill and sort, the symbol-table index
## assignment and the chained-fixups location sort run on the thread pool.
## Their results are either positional or a total order over values, so the
## output must be byte-identical at any thread count. The ad-hoc code signature
## covers the output basename, so each link uses the same basename in its own
## directory.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/dylib.s -o %t/dylib.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/test.o
# RUN: mkdir -p %t/t1 %t/t2 %t/t8 %t/c1 %t/c2 %t/c8
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/libfoo.dylib \
# RUN:     -no_uuid %t/dylib.o -o %t/libfoo.dylib

## Classic dyld info: function starts, the symbol table and the indirect symbol
## table (which reads the assigned symtab indices).
# RUN: %lld -arch arm64 -no_uuid --threads=1 %t/test.o %t/libfoo.dylib -o %t/t1/out
# RUN: %lld -arch arm64 -no_uuid --threads=2 %t/test.o %t/libfoo.dylib -o %t/t2/out
# RUN: %lld -arch arm64 -no_uuid --threads=8 %t/test.o %t/libfoo.dylib -o %t/t8/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: llvm-objdump --syms %t/t8/out > %t/dump
# RUN: llvm-objdump --macho --function-starts %t/t8/out >> %t/dump
# RUN: FileCheck %s --check-prefix=STARTS < %t/dump
# RUN: llvm-objdump --macho --indirect-symbols %t/t8/out | \
# RUN:     FileCheck %s --check-prefix=INDIRECT

## Chained fixups: the location sort feeds the page starts and the chains.
# RUN: %lld -arch arm64 -no_uuid -fixup_chains --threads=1 %t/test.o %t/libfoo.dylib -o %t/c1/out
# RUN: %lld -arch arm64 -no_uuid -fixup_chains --threads=2 %t/test.o %t/libfoo.dylib -o %t/c2/out
# RUN: %lld -arch arm64 -no_uuid -fixup_chains --threads=8 %t/test.o %t/libfoo.dylib -o %t/c8/out
# RUN: cmp %t/c1/out %t/c2/out
# RUN: cmp %t/c1/out %t/c8/out
# RUN: llvm-objdump --macho --chained-fixups %t/c8/out | \
# RUN:     FileCheck %s --check-prefix=FIXUPS

## Enough functions for the function-starts encoder to split the sorted address
## vector into several chunks, so the chunked ULEB stream is compared against
## the serial encoder's. Every function also carries an alias label, so each
## address appears twice and a chunk boundary necessarily falls on a repeated
## address: the encoder must handle the zero delta across the cut.
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/many.s -o %t/many.o
# RUN: mkdir -p %t/m1 %t/m2 %t/m8
# RUN: %lld -arch arm64 -no_uuid --threads=1 %t/many.o -o %t/m1/out
# RUN: %lld -arch arm64 -no_uuid --threads=2 %t/many.o -o %t/m2/out
# RUN: %lld -arch arm64 -no_uuid --threads=8 %t/many.o -o %t/m8/out
# RUN: cmp %t/m1/out %t/m2/out
# RUN: cmp %t/m1/out %t/m8/out

## Each LINKEDIT owner gets its own --time-trace scope, including the owners
## that run on a thread-pool worker.
# RUN: mkdir -p %t/tt
# RUN: %lld -arch arm64 -no_uuid --threads=8 --time-trace=%t/trace.json \
# RUN:     --time-trace-granularity=0 %t/test.o %t/libfoo.dylib -o %t/tt/out
# RUN: FileCheck %s --check-prefix=TRACE < %t/trace.json

# TRACE-DAG: "detail":"__symbol_table"
# TRACE-DAG: "detail":"__func_starts"

## Function starts are sorted addresses, so they are ascending and cover every
## function, whichever thread filled and sorted them.
# STARTS-LABEL: SYMBOL TABLE:
# STARTS-DAG:   [[#%x,F1:]] l F __TEXT,__text _f1
# STARTS-DAG:   [[#%x,F2:]] l F __TEXT,__text _f2
# STARTS-DAG:   [[#%x,G1:]] g F __TEXT,__text _g1
# STARTS-DAG:   [[#%x,G2:]] g F __TEXT,__text _g2
# STARTS-DAG:   [[#%x,MAIN:]] g F __TEXT,__text _main
# STARTS-LABEL: out:
# STARTS:       [[#MAIN]]
# STARTS:       [[#F1]]
# STARTS:       [[#F2]]
# STARTS:       [[#G1]]
# STARTS:       [[#G2]]

# INDIRECT:      Indirect symbols for (__TEXT,__stubs)
# INDIRECT-NEXT: address                    index name
# INDIRECT-NEXT: {{.*}} _dylib_func

## The sorted locations give one __DATA_CONST page and three __DATA pages, with
## the page starts in ascending offset order, plus the two imports.
# FIXUPS:      chained starts in segment {{[0-9]+}} (__DATA_CONST)
# FIXUPS:        page_count = 1
# FIXUPS-NEXT:     page_start[0] = 0
# FIXUPS:      chained starts in segment {{[0-9]+}} (__DATA)
# FIXUPS:        page_count = 3
# FIXUPS-NEXT:     page_start[0] = 0
# FIXUPS-NEXT:     page_start[1] = 4112
# FIXUPS-NEXT:     page_start[2] = 8224
# FIXUPS:      dyld chained import[0]
# FIXUPS:        name_offset = {{.*}} (_dylib_func)
# FIXUPS:      dyld chained import[1]
# FIXUPS:        name_offset = {{.*}} (_dylib_data)

#--- dylib.s
.globl _dylib_func, _dylib_data
.text
_dylib_func:
  ret
.data
_dylib_data:
  .quad 0

#--- test.s
.globl _main, _g1, _g2
.text
_main:
  bl _f1
  bl _dylib_func
  ret
_f1:
  bl _f2
  ret
_f2:
  ret
_g1:
  ret
_g2:
  ret

## Pointers spread over more than one page, in two segments, so the location
## sort produces several page starts per segment.
.section __DATA,__const
.quad _main
.quad _g1

.data
.quad _main
.quad _dylib_data
.space 0x5000
.quad _g1
.quad _f2
.space 0x5000
.quad _g2

.subsections_via_symbols

#--- many.s
.globl _main
.text
_main:
  ret
.macro mkfun
_fun\@:
_alias\@:
  ret
.endm
.rept 9000
mkfun
.endr
.subsections_via_symbols
