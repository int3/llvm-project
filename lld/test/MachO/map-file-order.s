# REQUIRES: x86 && thread_support

## b.a is registered before libE, but b.o is only extracted recursively while
## a.a is registered. The map must put b.o before its parent a.o, after libE,
## and before libD, even when archive registration is deferred past the dylibs.
## Creation ids put a.o before b.o; owning-archive order puts b.o before libE.
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/refs.s -o %t/refs.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/b.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/tail.s -o %t/tail.o
# RUN: llvm-ar rcs %t/a.a %t/a.o
# RUN: llvm-ar rcs %t/b.a %t/b.o
# RUN: %lld -dylib -install_name /Result -dead_strip -o %t/out --threads=1 -map %t/one.map %t/refs.o %t/b.a %t/libE.tbd %t/a.a %t/libD.tbd
# RUN: cp %t/out %t/one.out
# RUN: %lld -dylib -install_name /Result -dead_strip -o %t/out --threads=4 -map %t/four.map %t/refs.o %t/b.a %t/libE.tbd %t/a.a %t/libD.tbd
# RUN: diff %t/one.map %t/four.map
# RUN: cmp %t/one.out %t/out
# RUN: FileCheck %s < %t/four.map
# CHECK:      # Object files:
# CHECK-NEXT: [  0] linker synthesized
# CHECK-NEXT: [  1] {{.*}}libSystem.tbd
# CHECK-NEXT: [  2] {{.*}}refs.o
# CHECK-NEXT: [  3] {{.*}}libE.tbd
# CHECK-NEXT: [  4] {{.*}}b.a(b.o)
# CHECK-NEXT: [  5] {{.*}}a.a(a.o)
# CHECK-NEXT: [  6] {{.*}}libD.tbd
# CHECK-NEXT: # Sections:
# CHECK:      # Dead Stripped Symbols:
# CHECK:      [  4] _unused_b
# CHECK-NEXT: {{.*}}[  5] _unused_a

## A direct object after the held archives drains the phase through the serial
## index-publication fallback. Its map must use the same registration positions.
# RUN: %lld -dylib -install_name /Result -dead_strip -o %t/out --threads=1 -map %t/one.map %t/refs.o %t/b.a %t/libE.tbd %t/a.a %t/libD.tbd %t/tail.o
# RUN: cp %t/out %t/one.out
# RUN: %lld -dylib -install_name /Result -dead_strip -o %t/out --threads=4 -map %t/four.map %t/refs.o %t/b.a %t/libE.tbd %t/a.a %t/libD.tbd %t/tail.o
# RUN: diff %t/one.map %t/four.map
# RUN: cmp %t/one.out %t/out

## Mandatory member loading uses the same nested completion order.
# RUN: %lld -dylib -install_name /Result -dead_strip -o %t/out --threads=1 -map %t/one.map %t/refs.o %t/b.a %t/libE.tbd -force_load %t/a.a %t/libD.tbd
# RUN: cp %t/out %t/one.out
# RUN: env LLD_REVERSE_HELD_PARSE=1 %lld -dylib -install_name /Result -dead_strip -o %t/out --threads=4 -map %t/four.map %t/refs.o %t/b.a %t/libE.tbd -force_load %t/a.a %t/libD.tbd
# RUN: diff %t/one.map %t/four.map
# RUN: cmp %t/one.out %t/out

#--- refs.s
.globl _root
_root:
  retq
.data
.globl _refs
_refs:
.quad _a
.quad _d
.quad _e

#--- a.s
.globl _a
_a:
  retq
.data
.quad _b
.globl _unused_a
.private_extern _unused_a
_unused_a:
  .quad 0
.subsections_via_symbols

#--- b.s
.globl _b
_b:
  retq
.globl _unused_b
.private_extern _unused_b
_unused_b:
  retq
.subsections_via_symbols

#--- tail.s
.globl _tail
_tail:
  retq

#--- libD.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: /usr/lib/libD.dylib
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _d ]
...

#--- libE.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: /usr/lib/libE.dylib
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _e ]
...
