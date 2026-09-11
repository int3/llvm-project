# REQUIRES: aarch64

## Five debug objects around a 128 MiB spacer force branch-range extension
## thunks. Thunks are appended to the symbol table after
## SymtabSection::prepare() has already gathered and ordered the STABS symbols,
## so the gathered order is only valid because no thunk has an object file.
## finalizeContents() re-runs the STABS filter over the appended suffix and
## rebuilds if anything passes; these links check the invariant holds and that
## the output does not depend on the thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/c.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/d.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/e.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/spacer.s -o %t/spacer.o

# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path header.pcm --threads=1 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/spacer.o %t/d.o %t/e.o -o %t/threads-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path header.pcm --threads=2 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/spacer.o %t/d.o %t/e.o -o %t/threads-2.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path header.pcm --threads=8 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/spacer.o %t/d.o %t/e.o -o %t/threads-8.dylib
# RUN: cmp %t/threads-1.dylib %t/threads-2.dylib
# RUN: cmp %t/threads-1.dylib %t/threads-8.dylib

## A thunk really was created, i.e. the suffix really is non-empty.
# RUN: llvm-nm %t/threads-8.dylib | FileCheck %s --check-prefix=THUNK
# THUNK: _stabs_dynamic_lookup.thunk.

## The STABS order is the N_AST record, then one source/object pair per object
## file in command-line order. The spacer object has no debug info, so it
## contributes no pair.
# RUN: dsymutil -s %t/threads-8.dylib | FileCheck %s
# CHECK:      (N_AST{{.*}}){{.*}} 'header.pcm'
# CHECK:      (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-prepare-hoist-thunks.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/a.o'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-prepare-hoist-thunks.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/b.o'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-prepare-hoist-thunks.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/c.o'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-prepare-hoist-thunks.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/d.o'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-prepare-hoist-thunks.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/e.o'

#--- spacer.s
## 0x8000000 is 128 MiB, one more than the forward branch limit. Split over two
## symbols: the thunk insertion algorithm cannot deal with one 128 MiB function.
.globl _spacer1, _spacer2
.text
_spacer1:
.space 0x4000000
_spacer2:
.space 0x4000000
.subsections_via_symbols

#--- test.s
.text
_duplicate_function:
Lfunc_begin0:
  bl _stabs_dynamic_lookup
  ret
Lfunc_end0:

.section __DATA,__data
_duplicate_data:
  .quad 0

.section __DWARF,__debug_str,regular,debug
Ldebug_str:
Lsource_name:
  .asciz "stabs-prepare-hoist-thunks.s"
Lcomp_dir:
  .asciz "/tmp"

.section __DWARF,__debug_abbrev,regular,debug
Lsection_abbrev:
  .byte 1                       // Abbreviation Code
  .byte 17                      // DW_TAG_compile_unit
  .byte 1                       // DW_CHILDREN_yes
  .byte 3                       // DW_AT_name
  .byte 14                      // DW_FORM_strp
  .byte 27                      // DW_AT_comp_dir
  .byte 14                      // DW_FORM_strp
  .byte 17                      // DW_AT_low_pc
  .byte 1                       // DW_FORM_addr
  .byte 18                      // DW_AT_high_pc
  .byte 6                       // DW_FORM_data4
  .byte 0                       // EOM(1)
  .byte 0                       // EOM(2)
  .byte 0                       // EOM(3)

.section __DWARF,__debug_info,regular,debug
.set Lunit_length, Ldebug_info_end-Ldebug_info_start
  .long Lunit_length
Ldebug_info_start:
  .short 4                      // DWARF version number
  .long Lsection_abbrev-Lsection_abbrev
  .byte 8                       // Address size
  .byte 1                       // Abbrev [1]
  .long Lsource_name-Ldebug_str // DW_AT_name
  .long Lcomp_dir-Ldebug_str    // DW_AT_comp_dir
  .quad Lfunc_begin0            // DW_AT_low_pc
.set Lhigh_pc, Lfunc_end0-Lfunc_begin0
  .long Lhigh_pc                // DW_AT_high_pc
  .byte 0                       // End of children
Ldebug_info_end:

.subsections_via_symbols
