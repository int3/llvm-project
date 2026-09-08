# REQUIRES: aarch64

# RUN: rm -rf %t && mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -oso_prefix %t/ %t/input.o -o %t/default.dylib
# RUN: obj2yaml %t/default.dylib | FileCheck %s --check-prefix=DEFAULT
# RUN: %lld -arch arm64 -dylib -oso_prefix %t/ -no-deduplicate-symbol-strings %t/input.o -o %t/nodedup.dylib
# RUN: obj2yaml %t/nodedup.dylib | FileCheck %s --check-prefix=NONDEDUP

## With the default string-table policy, STABS should use the offsets already
## assigned to local/external symbols. Cover functions and data of both kinds.
# DEFAULT:      NameList:
# DEFAULT:        - n_strx:          64
# DEFAULT-NEXT:     n_type:          0x64
# DEFAULT:        - n_strx:          99
# DEFAULT-NEXT:     n_type:          0x66
# DEFAULT:        - n_strx:          2
# DEFAULT-NEXT:     n_type:          0x24
# DEFAULT:        - n_strx:          18
# DEFAULT-NEXT:     n_type:          0x26
# DEFAULT:        - n_strx:          30
# DEFAULT-NEXT:     n_type:          0x24
# DEFAULT:        - n_strx:          49
# DEFAULT-NEXT:     n_type:          0x20
# DEFAULT:        - n_strx:          2
# DEFAULT-NEXT:     n_type:          0xE
# DEFAULT:        - n_strx:          18
# DEFAULT-NEXT:     n_type:          0xE
# DEFAULT:        - n_strx:          30
# DEFAULT-NEXT:     n_type:          0xF
# DEFAULT:        - n_strx:          49
# DEFAULT-NEXT:     n_type:          0xF
# DEFAULT:        StringTable:
# DEFAULT-NEXT:     - ' '
# DEFAULT-NEXT:     - _local_function
# DEFAULT-NEXT:     - _local_data
# DEFAULT-NEXT:     - _external_function
# DEFAULT-NEXT:     - _external_data
# DEFAULT-NEXT:     - /tmp{{[/\\]}}stabs-symbol-string-offsets.s
# DEFAULT-NEXT:     - input.o

## Without deduplication, the ordinary symbols retain their original entries
## and STABS append separate copies of every symbol name.
# NONDEDUP:      NameList:
# NONDEDUP:        - n_strx:          64
# NONDEDUP-NEXT:   n_type:          0x64
# NONDEDUP:        - n_strx:          99
# NONDEDUP-NEXT:   n_type:          0x66
# NONDEDUP:        - n_strx:          107
# NONDEDUP-NEXT:   n_type:          0x24
# NONDEDUP:        - n_strx:          123
# NONDEDUP-NEXT:   n_type:          0x26
# NONDEDUP:        - n_strx:          135
# NONDEDUP-NEXT:   n_type:          0x24
# NONDEDUP:        - n_strx:          154
# NONDEDUP-NEXT:   n_type:          0x20
# NONDEDUP:        - n_strx:          2
# NONDEDUP-NEXT:   n_type:          0xE
# NONDEDUP:        - n_strx:          18
# NONDEDUP-NEXT:   n_type:          0xE
# NONDEDUP:        - n_strx:          30
# NONDEDUP-NEXT:   n_type:          0xF
# NONDEDUP:        - n_strx:          49
# NONDEDUP-NEXT:   n_type:          0xF
# NONDEDUP:        StringTable:
# NONDEDUP-NEXT:     - ' '
# NONDEDUP-NEXT:     - _local_function
# NONDEDUP-NEXT:     - _local_data
# NONDEDUP-NEXT:     - _external_function
# NONDEDUP-NEXT:     - _external_data
# NONDEDUP-NEXT:     - /tmp{{[/\\]}}stabs-symbol-string-offsets.s
# NONDEDUP-NEXT:     - input.o
# NONDEDUP-NEXT:     - _local_function
# NONDEDUP-NEXT:     - _local_data
# NONDEDUP-NEXT:     - _external_function
# NONDEDUP-NEXT:     - _external_data

.text
_local_function:
Lfunc_begin0:
  ret
Lfunc_end0:

.globl _external_function
_external_function:
  ret

.section __DATA,__data
_local_data:
  .quad 0

.globl _external_data
_external_data:
  .quad 0

.section __DWARF,__debug_str,regular,debug
Ldebug_str:
Lsource_name:
  .asciz "stabs-symbol-string-offsets.s"
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
