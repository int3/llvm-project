# REQUIRES: aarch64

## SymtabSection::prepare() gathers the local symbol-table entries on the
## workers in chunks of input files, whose per-chunk lists are concatenated in
## chunk order, and gathers the STABS candidates in chunks of 8192 symbol-table
## entries. Both must produce
## exactly the sequence the serial walks produce, so the whole output has to be
## byte-identical at every thread count. This input has more than 8192
## symbol-table entries spread over files with very different local counts,
## including a file with no locals at all, a file with no debug info (so none of
## its symbols is a STABS candidate) and a dylib (not an ObjFile, so the
## file-indexed walk has to skip it without disturbing the offsets).

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/many.s -o %t/many.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/few.s -o %t/few.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/globals-only.s -o %t/globals-only.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/no-debug.s -o %t/no-debug.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/dylib.s -o %t/dylib.o
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/dep.dylib -no_uuid \
# RUN:     %t/dylib.o -o %t/dep.dylib

# RUN: mkdir -p %t/t1 %t/t2 %t/t8
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup --threads=1 %t/many.o %t/globals-only.o \
# RUN:     %t/dep.dylib %t/few.o %t/no-debug.o -o %t/t1/out
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup --threads=2 %t/many.o %t/globals-only.o \
# RUN:     %t/dep.dylib %t/few.o %t/no-debug.o -o %t/t2/out
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup --threads=8 %t/many.o %t/globals-only.o \
# RUN:     %t/dep.dylib %t/few.o %t/no-debug.o -o %t/t8/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out

## The STABS records are grouped per object file in input order, and the
## no-debug object contributes none.
# RUN: dsymutil -s %t/t8/out | FileCheck %s
# CHECK:      (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}symtab-prepare-split.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/many.o'
# CHECK:      (N_STSYM{{.*}}){{.*}} '_local_0000'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}symtab-prepare-split.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/few.o'
# CHECK:      (N_STSYM{{.*}}){{.*}} '_few_local'
# CHECK-NOT:  (N_STSYM{{.*}}){{.*}} '_undebugged_local'

## -x removes the whole local walk; the selective lists take the match-filtered
## paths on the workers.
# RUN: mkdir -p %t/x1 %t/x8 %t/i1 %t/i8 %t/e1 %t/e8 %t/n1 %t/n8
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -x --threads=1 %t/many.o %t/globals-only.o \
# RUN:     %t/dep.dylib %t/few.o %t/no-debug.o -o %t/x1/out
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -x --threads=8 %t/many.o %t/globals-only.o \
# RUN:     %t/dep.dylib %t/few.o %t/no-debug.o -o %t/x8/out
# RUN: cmp %t/x1/out %t/x8/out

# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_no_strip_list %t/keep.txt \
# RUN:     --threads=1 %t/many.o %t/globals-only.o %t/dep.dylib %t/few.o \
# RUN:     %t/no-debug.o -o %t/i1/out
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_no_strip_list %t/keep.txt \
# RUN:     --threads=8 %t/many.o %t/globals-only.o %t/dep.dylib %t/few.o \
# RUN:     %t/no-debug.o -o %t/i8/out
# RUN: cmp %t/i1/out %t/i8/out
# RUN: llvm-objdump --syms %t/i8/out | FileCheck %s --check-prefix=INCLUDE
# INCLUDE-DAG: l {{.*}} _local_0000
# INCLUDE-NOT: l {{.*}} _local_0001

# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_strip_list %t/keep.txt \
# RUN:     --threads=1 %t/many.o %t/globals-only.o %t/dep.dylib %t/few.o \
# RUN:     %t/no-debug.o -o %t/e1/out
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_strip_list %t/keep.txt \
# RUN:     --threads=8 %t/many.o %t/globals-only.o %t/dep.dylib %t/few.o \
# RUN:     %t/no-debug.o -o %t/e8/out
# RUN: cmp %t/e1/out %t/e8/out
# RUN: llvm-objdump --syms %t/e8/out | FileCheck %s --check-prefix=EXCLUDE
# EXCLUDE-NOT: l {{.*}} _local_0000

## Without deduplicated symbol strings the prefix pass must intern the names in
## entry order, so the local walk stays serial even with eight threads.
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -no-deduplicate-symbol-strings --threads=1 \
# RUN:     %t/many.o %t/globals-only.o %t/dep.dylib %t/few.o %t/no-debug.o \
# RUN:     -o %t/n1/out
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -no-deduplicate-symbol-strings --threads=8 \
# RUN:     %t/many.o %t/globals-only.o %t/dep.dylib %t/few.o %t/no-debug.o \
# RUN:     -o %t/n8/out
# RUN: cmp %t/n1/out %t/n8/out

#--- keep.txt
_local_0000

#--- many.s
## 10,000 symbol-table entries in one file: more than the 8192-entry STABS
## gather chunk, so its candidates span several chunks and no chunk boundary
## coincides with a file boundary.
.text
_many_function:
Lfunc_begin0:
  ret
Lfunc_end0:

.section __DATA,__data
.macro gen_1 p
  _local_\p\()0: .quad 0
  .globl _global_\p\()0
  _global_\p\()0: .quad 0
  _local_\p\()1: .quad 0
  .globl _global_\p\()1
  _global_\p\()1: .quad 0
  _local_\p\()2: .quad 0
  .globl _global_\p\()2
  _global_\p\()2: .quad 0
  _local_\p\()3: .quad 0
  .globl _global_\p\()3
  _global_\p\()3: .quad 0
  _local_\p\()4: .quad 0
  .globl _global_\p\()4
  _global_\p\()4: .quad 0
.endm
.macro gen_2 p
  gen_1 \p\()0
  gen_1 \p\()1
  gen_1 \p\()2
  gen_1 \p\()3
  gen_1 \p\()4
  gen_1 \p\()5
  gen_1 \p\()6
  gen_1 \p\()7
  gen_1 \p\()8
  gen_1 \p\()9
.endm
.macro gen_3 p
  gen_2 \p\()0
  gen_2 \p\()1
  gen_2 \p\()2
  gen_2 \p\()3
  gen_2 \p\()4
  gen_2 \p\()5
  gen_2 \p\()6
  gen_2 \p\()7
  gen_2 \p\()8
  gen_2 \p\()9
.endm
gen_3 0
gen_3 1
gen_3 2
gen_3 3
gen_3 4
gen_3 5
gen_3 6
gen_3 7
gen_3 8
gen_3 9

.section __DWARF,__debug_str,regular,debug
Ldebug_str:
Lsource_name:
  .asciz "symtab-prepare-split.s"
Lcomp_dir:
  .asciz "/tmp"

.section __DWARF,__debug_abbrev,regular,debug
Lsection_abbrev:
  .byte 1
  .byte 17
  .byte 1
  .byte 3
  .byte 14
  .byte 27
  .byte 14
  .byte 17
  .byte 1
  .byte 18
  .byte 6
  .byte 0
  .byte 0
  .byte 0

.section __DWARF,__debug_info,regular,debug
.set Lunit_length, Ldebug_info_end-Ldebug_info_start
  .long Lunit_length
Ldebug_info_start:
  .short 4
  .long Lsection_abbrev-Lsection_abbrev
  .byte 8
  .byte 1
  .long Lsource_name-Ldebug_str
  .long Lcomp_dir-Ldebug_str
  .quad Lfunc_begin0
.set Lhigh_pc, Lfunc_end0-Lfunc_begin0
  .long Lhigh_pc
  .byte 0
Ldebug_info_end:

.subsections_via_symbols

#--- few.s
## One local and one global, with debug info: a small contribution between two
## much larger ones.
.text
_few_function:
Lfunc_begin0:
  ret
Lfunc_end0:

.section __DATA,__data
_few_local:
  .quad 0
.globl _few_global
_few_global:
  .quad 0

.section __DWARF,__debug_str,regular,debug
Ldebug_str:
Lsource_name:
  .asciz "symtab-prepare-split.s"
Lcomp_dir:
  .asciz "/tmp"

.section __DWARF,__debug_abbrev,regular,debug
Lsection_abbrev:
  .byte 1
  .byte 17
  .byte 1
  .byte 3
  .byte 14
  .byte 27
  .byte 14
  .byte 17
  .byte 1
  .byte 18
  .byte 6
  .byte 0
  .byte 0
  .byte 0

.section __DWARF,__debug_info,regular,debug
.set Lunit_length, Ldebug_info_end-Ldebug_info_start
  .long Lunit_length
Ldebug_info_start:
  .short 4
  .long Lsection_abbrev-Lsection_abbrev
  .byte 8
  .byte 1
  .long Lsource_name-Ldebug_str
  .long Lcomp_dir-Ldebug_str
  .quad Lfunc_begin0
.set Lhigh_pc, Lfunc_end0-Lfunc_begin0
  .long Lhigh_pc
  .byte 0
Ldebug_info_end:

.subsections_via_symbols

#--- globals-only.s
## No local symbols at all: this file must contribute nothing to the local
## scatter and must not shift the following files' offsets.
.globl _only_global_a, _only_global_b
.section __DATA,__data
_only_global_a:
  .quad 0
_only_global_b:
  .quad 0
.subsections_via_symbols

#--- no-debug.s
## No compile unit, so none of these is a STABS candidate even though they are
## symbol-table entries.
.globl _undebugged_global
.section __DATA,__data
_undebugged_local:
  .quad 0
_undebugged_global:
  .quad 0
.subsections_via_symbols

#--- dylib.s
.globl _dylib_symbol
.text
_dylib_symbol:
  ret
.subsections_via_symbols
