# REQUIRES: aarch64

# RUN: rm -rf %t && mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/c.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/d.o
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid --threads=1 \
# RUN:     -add_ast_path header.pcm %t/a.o %t/b.o %t/c.o %t/d.o -o %t/threads-1.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid --threads=2 \
# RUN:     -add_ast_path header.pcm %t/a.o %t/b.o %t/c.o %t/d.o -o %t/threads-2.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid --threads=4 \
# RUN:     -add_ast_path header.pcm %t/a.o %t/b.o %t/c.o %t/d.o -o %t/threads-4.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib -no_uuid --threads=8 \
# RUN:     -add_ast_path header.pcm %t/a.o %t/b.o %t/c.o %t/d.o -o %t/threads-8.dylib
# RUN: cmp %t/threads-1.dylib %t/threads-2.dylib
# RUN: cmp %t/threads-1.dylib %t/threads-4.dylib
# RUN: cmp %t/threads-1.dylib %t/threads-8.dylib
# RUN: dsymutil -s %t/threads-8.dylib | FileCheck %s

## Four contributing objects cross the fragment threshold. They intentionally
## duplicate local function and data names, while the N_AST stays before the
## ordered per-object source/OSO records.
# CHECK:      (N_AST{{.*}}){{.*}} 'header.pcm'
# CHECK:      (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-parallel-fragments.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/a.o'
# CHECK:      (N_FUN{{.*}}){{.*}} '_duplicate_function'
# CHECK:      (N_STSYM{{.*}}){{.*}} '_duplicate_data'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-parallel-fragments.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/b.o'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-parallel-fragments.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/c.o'
# CHECK:      (N_SO{{.*}}) 01
# CHECK-NEXT: (N_SO{{.*}}){{.*}} '/tmp{{[/\\]}}stabs-parallel-fragments.s'
# CHECK-NEXT: (N_OSO{{.*}}){{.*}} '/{{.*}}/d.o'

.text
_duplicate_function:
Lfunc_begin0:
  ret
Lfunc_end0:

.section __DATA,__data
_duplicate_data:
  .quad 0

.section __DWARF,__debug_str,regular,debug
Ldebug_str:
Lsource_name:
  .asciz "stabs-parallel-fragments.s"
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
