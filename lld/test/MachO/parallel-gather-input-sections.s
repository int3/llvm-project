# REQUIRES: x86
# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/first.s -o %t/first.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/second.s -o %t/second.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/third.s -o %t/third.o
# RUN: %lld -lSystem -init_offsets -no_uuid --threads=1 %t/first.o %t/second.o %t/third.o -o %t/one
# RUN: %lld -lSystem -init_offsets -no_uuid --threads=8 %t/first.o %t/second.o %t/third.o -o %t/eight
# RUN: cmp %t/one %t/eight
# RUN: llvm-objdump --macho --section=__DATA,__data %t/eight | FileCheck %s
# RUN: llvm-objdump --syms %t/eight | FileCheck %s --check-prefix=WEAK

## Exercise an ordinary section around literal and init-offset special inputs,
## while the duplicate weak subsection is omitted. The byte comparison also
## checks that the resulting inputSections order is identical at one and eight
## threads.

# CHECK: Contents of (__DATA,__data) section
# CHECK: 01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
# CHECK: 03 00 00 00 00 00 00 00
# WEAK-COUNT-1: _shared_weak

#--- first.s
.globl _main, _first_init
.text
_main:
  ret
_first_init:
  ret

.section __DATA,__data
.quad 1

.cstring
.asciz "first"

.section __TEXT,__literal4,4byte_literals
.long 0x11111111

.section __DATA,__mod_init_func,mod_init_funcs
.quad _first_init

.section __TEXT,__text
.globl _shared_weak
.weak_definition _shared_weak
_shared_weak:
  ret

.subsections_via_symbols

#--- second.s
.globl _second_init
.text
_second_init:
  ret

.section __DATA,__data
.quad 2

.cstring
.asciz "second"

.section __TEXT,__literal4,4byte_literals
.long 0x22222222

.section __DATA,__mod_init_func,mod_init_funcs
.quad _second_init

.section __TEXT,__text
.globl _shared_weak
.weak_definition _shared_weak
_shared_weak:
  ret

.subsections_via_symbols

#--- third.s
.globl _third_init
.text
_third_init:
  ret

.section __DATA,__data
.quad 3

.cstring
.asciz "third"

.section __TEXT,__literal4,4byte_literals
.long 0x33333333

.section __DATA,__mod_init_func,mod_init_funcs
.quad _third_init

.subsections_via_symbols
