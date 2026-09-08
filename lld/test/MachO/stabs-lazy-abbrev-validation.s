# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t.o
# RUN: %lld -lSystem %t.o -o %t 2>&1 | FileCheck %s

## The second abbreviation is not used by the root DIE, but a malformed
## declaration still invalidates the selected abbreviation set. The lazy STABS
## reader must therefore fall back to DWARFUnit and retain its diagnostic.
# CHECK: warning: {{.*}}malformed abbreviation declaration attribute

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
  .byte 0
  .byte 0
  .byte 2                       ## unused abbreviation code
  .byte 46                      ## DW_TAG_subprogram
  .byte 0                       ## DW_CHILDREN_no
  .byte 3                       ## DW_AT_name
  .byte 0                       ## malformed zero form
  .byte 0
  .byte 0

.section __DWARF,__debug_info,regular,debug
  .long Ldebug_info_end-Ldebug_info_start
Ldebug_info_start:
  .short 4
  .long 0                       ## abbreviation offset
  .byte 8                       ## address size
  .byte 1                       ## abbreviation code
  .asciz "lazy-abbrev.c"
  .byte 0                       ## end of children
Ldebug_info_end:
