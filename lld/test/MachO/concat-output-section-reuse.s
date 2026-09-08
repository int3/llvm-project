# REQUIRES: x86
# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/strong.s -o %t/strong.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/split.s -o %t/split.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: %lld --threads=1 -no_uuid -lSystem -o %t/one %t/strong.o %t/split.o %t/main.o
# RUN: %lld --threads=4 -no_uuid -lSystem -o %t/many %t/strong.o %t/split.o %t/main.o
# RUN: cmp %t/one %t/many
# RUN: llvm-objdump --macho --syms %t/one | FileCheck %s

## The first atom in split.o is coalesced with strong.o. The second atom must
## still be routed to its ordinary output section, even though it is the first
## non-skipped atom using the per-Section destination cache.

# CHECK: {{0*}} {{.*}} _after

#--- strong.s
.globl _foo
.text
_foo:
  ret

#--- split.s
.globl _foo, _after
.weak_definition _foo
.text
_foo:
  ret
_after:
  ret
.subsections_via_symbols

#--- main.s
.globl _main
.text
_main:
  callq _after
  ret
