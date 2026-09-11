# REQUIRES: aarch64

## The N_OSO absolute path is built from one working directory captured once per
## link instead of one getcwd() per object file. Link with relative object paths
## from a directory that is not the test's, so the captured directory is what
## decides the recorded paths, and check that -oso_prefix is still applied after
## absolutization.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/objs %t/elsewhere
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/objs/a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/objs/b.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/objs/c.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/objs/d.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/objs/e.o

## Relative object paths, resolved against %t/objs.
# RUN: cd %t/objs && %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib \
# RUN:     -no_uuid -undefined dynamic_lookup --threads=1 a.o b.o c.o d.o e.o \
# RUN:     -o %t/rel-1.dylib
# RUN: cd %t/objs && %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib \
# RUN:     -no_uuid -undefined dynamic_lookup --threads=8 a.o b.o c.o d.o e.o \
# RUN:     -o %t/rel-8.dylib
# RUN: cmp %t/rel-1.dylib %t/rel-8.dylib
# RUN: dsymutil -s %t/rel-8.dylib | FileCheck %s -DDIR=%t --check-prefix=REL
# REL:      (N_OSO{{.*}}){{.*}} '[[DIR]]/objs/a.o'
# REL:      (N_OSO{{.*}}){{.*}} '[[DIR]]/objs/b.o'
# REL:      (N_OSO{{.*}}){{.*}} '[[DIR]]/objs/c.o'
# REL:      (N_OSO{{.*}}){{.*}} '[[DIR]]/objs/d.o'
# REL:      (N_OSO{{.*}}){{.*}} '[[DIR]]/objs/e.o'

## The same link run from a different directory records different paths, even
## though the objects are the same files.
# RUN: cd %t/elsewhere && %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib \
# RUN:     -no_uuid -undefined dynamic_lookup --threads=8 ../objs/a.o ../objs/b.o ../objs/c.o \
# RUN:     ../objs/d.o ../objs/e.o -o %t/other-8.dylib
# RUN: dsymutil -s %t/other-8.dylib | FileCheck %s -DDIR=%t --check-prefix=OTHER
# OTHER: (N_OSO{{.*}}){{.*}} '[[DIR]]/elsewhere/../objs/a.o'

## -oso_prefix is stripped after absolutization, so it matches the captured
## directory too.
# RUN: cd %t/objs && %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib \
# RUN:     -no_uuid -undefined dynamic_lookup --threads=8 -oso_prefix %t/objs/ \
# RUN:     a.o b.o c.o d.o e.o -o %t/prefix-8.dylib
# RUN: cd %t/objs && %no-lsystem-lld -arch arm64 -dylib -install_name @rpath/test.dylib \
# RUN:     -no_uuid -undefined dynamic_lookup --threads=1 -oso_prefix %t/objs/ \
# RUN:     a.o b.o c.o d.o e.o -o %t/prefix-1.dylib
# RUN: cmp %t/prefix-1.dylib %t/prefix-8.dylib
# RUN: dsymutil -s %t/prefix-8.dylib | FileCheck %s --check-prefix=PREFIX
# PREFIX:      (N_OSO{{.*}}){{.*}} 'a.o'
# PREFIX:      (N_OSO{{.*}}){{.*}} 'b.o'
# PREFIX:      (N_OSO{{.*}}){{.*}} 'c.o'
# PREFIX:      (N_OSO{{.*}}){{.*}} 'd.o'
# PREFIX:      (N_OSO{{.*}}){{.*}} 'e.o'

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
  .asciz "stabs-oso-relative-cwd.s"
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
