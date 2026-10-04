# REQUIRES: aarch64
## ObjCStubsSection::setUp() marks _objc_msgSend used. When the link defines
## _objc_msgSend itself and -dead_strip finds nothing else that references it,
## that turns a dead definition live and puts it in the export trie. The export
## symbols for the trie built on a worker must be selected after that, or the
## trie differs between --threads=1 and higher thread counts.

# RUN: rm -rf %t; mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: mkdir %t/t1 %t/t2 %t/t8
# RUN: %lld -arch arm64 -lSystem -dead_strip -map %t/t1/out.map \
# RUN:   --threads=1 %t/input.o -o %t/t1/out
# RUN: %lld -arch arm64 -lSystem -dead_strip -map %t/t2/out.map \
# RUN:   --threads=2 %t/input.o -o %t/t2/out
# RUN: %lld -arch arm64 -lSystem -dead_strip -map %t/t8/out.map \
# RUN:   --threads=8 %t/input.o -o %t/t8/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
## The map files name their output, so compare them past that line.
# RUN: tail -n +2 %t/t1/out.map > %t/t1.map
# RUN: tail -n +2 %t/t2/out.map > %t/t2.map
# RUN: tail -n +2 %t/t8/out.map > %t/t8.map
# RUN: cmp %t/t1.map %t/t2.map
# RUN: cmp %t/t1.map %t/t8.map
# RUN: llvm-objdump --macho --exports-trie %t/t8/out | FileCheck %s

# CHECK-LABEL: Exports trie:
# CHECK-DAG:   _main
# CHECK-DAG:   _objc_msgSend

.section __TEXT,__objc_methname,cstring_literals
lselref1:
  .asciz "foo"

.section __DATA,__objc_selrefs,literal_pointers,no_dead_strip
.p2align 3
.quad lselref1

.text
.globl _main
.p2align 2
_main:
  bl _objc_msgSend$foo
  ret

.globl _objc_msgSend
.p2align 2
_objc_msgSend:
  ret

.subsections_via_symbols
