# REQUIRES: aarch64

## The N_SO/N_OSO strings are computed on workers and interned through the
## ordered string-table batch, so their offsets must match what serial
## addString() calls would have produced in every symbol-table mode, and must
## not depend on the thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/c.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/d.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/e.o

# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path _duplicate_data --threads=1 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/ast-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path _duplicate_data --threads=2 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/ast-2.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path _duplicate_data --threads=8 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/ast-8.dylib
# RUN: cmp %t/ast-1.dylib %t/ast-2.dylib
# RUN: cmp %t/ast-1.dylib %t/ast-8.dylib

## The AST path here is spelled exactly like a symbol name that the
## normal-symbol batch has already interned, so the STABS batch has to find the
## existing string-table entry instead of appending a second copy.
# RUN: dsymutil -s %t/ast-8.dylib | FileCheck %s --check-prefix=AST
# AST: (N_AST{{.*}}){{.*}} '_duplicate_data'

## -x drops every local symbol, so the STABS symbols come only from the
## external ones.
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -x --threads=1 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/strip-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -x --threads=8 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/strip-8.dylib
# RUN: cmp %t/strip-1.dylib %t/strip-8.dylib
# RUN: dsymutil -s %t/strip-8.dylib | FileCheck %s --check-prefix=STRIP
# STRIP-NOT: N_STSYM

## Selective local-symbol lists.
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_no_strip_list %t/include.txt \
# RUN:     --threads=1 %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/include-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_no_strip_list %t/include.txt \
# RUN:     --threads=8 %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/include-8.dylib
# RUN: cmp %t/include-1.dylib %t/include-8.dylib
# RUN: dsymutil -s %t/include-8.dylib | FileCheck %s --check-prefix=INCLUDE
# INCLUDE:     (N_FUN{{.*}}){{.*}} '_duplicate_function'
# INCLUDE-NOT: (N_STSYM{{.*}}){{.*}} '_duplicate_data'

# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_strip_list %t/include.txt \
# RUN:     --threads=1 %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/exclude-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -non_global_symbols_strip_list %t/include.txt \
# RUN:     --threads=8 %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/exclude-8.dylib
# RUN: cmp %t/exclude-1.dylib %t/exclude-8.dylib
# RUN: dsymutil -s %t/exclude-8.dylib | FileCheck %s --check-prefix=EXCLUDE
# EXCLUDE:     (N_STSYM{{.*}}){{.*}} '_duplicate_data'
# EXCLUDE-NOT: (N_FUN{{.*}}){{.*}} '_duplicate_function'

## -no-deduplicate-symbol-strings keeps the serial emission, with the symbol
## strings appended between the source and object records.
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path header.pcm \
# RUN:     -no-deduplicate-symbol-strings --threads=1 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/nodedup-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid \
# RUN:     -undefined dynamic_lookup -add_ast_path header.pcm \
# RUN:     -no-deduplicate-symbol-strings --threads=8 \
# RUN:     %t/a.o %t/b.o %t/c.o %t/d.o %t/e.o -o %t/nodedup-8.dylib
# RUN: cmp %t/nodedup-1.dylib %t/nodedup-8.dylib
# RUN: dsymutil -s %t/nodedup-8.dylib | FileCheck %s --check-prefix=NODEDUP
# NODEDUP:      (N_AST{{.*}}){{.*}} 'header.pcm'
# NODEDUP:      (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-prepare-hoist-modes.s'
# NODEDUP-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/a.o'
# NODEDUP-NEXT: (N_FUN{{.*}}){{.*}} '_duplicate_function'

#--- include.txt
_duplicate_function

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
  .asciz "stabs-prepare-hoist-modes.s"
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
