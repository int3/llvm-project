# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t.o
# RUN: %lld -lSystem %t.o -o %t
# RUN: dsymutil -s %t | FileCheck %s

## A normal compile unit has a real child DIE. The lazy source-file reader
## needs only the root DIE and must retain its N_SO path without constructing
## the child DIE array.
# CHECK: (N_SO{{ *}}){{.*}}'/tmp/normal-child.c'

.text
.globl _main
_main:
  retq

.section __DWARF,__debug_abbrev,regular,debug
  .byte 1                       ## abbreviation code
  .byte 17                      ## DW_TAG_compile_unit
  .byte 1                       ## DW_CHILDREN_yes
  .byte 3                       ## DW_AT_name
  .byte 8                       ## DW_FORM_string
  .byte 27                      ## DW_AT_comp_dir
  .byte 8                       ## DW_FORM_string
  .byte 0
  .byte 0
  .byte 2                       ## abbreviation code
  .byte 46                      ## DW_TAG_subprogram
  .byte 0                       ## DW_CHILDREN_no
  .byte 0
  .byte 0
  .byte 0                       ## end of abbreviation set

.section __DWARF,__debug_info,regular,debug
  .long Ldebug_info_end-Ldebug_info_start
Ldebug_info_start:
  .short 4
  .long 0                       ## abbreviation offset
  .byte 8                       ## address size
  .byte 1                       ## compile-unit abbreviation code
  .asciz "normal-child.c"
  .asciz "/tmp"
  .byte 2                       ## real child DIE
  .byte 0                       ## end of children
Ldebug_info_end:
