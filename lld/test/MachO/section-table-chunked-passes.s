# REQUIRES: x86
# RUN: rm -rf %t; mkdir -p %t/one %t/eight
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t/test.o

## The coldness, ICF-classification and createOutputSections passes walk
## inputSections in chunks of 4096 on worker threads. This input has more than
## 8 Ki input sections, so each of those passes spans several chunks, and
## __DATA,__data's inputs neither begin in the first chunk nor end in the chunk
## where they begin. The two links must be byte-identical; the output basename
## is kept the same so that nothing but the thread count differs.
# RUN: %lld -lSystem --icf=all --threads=1 %t/test.o -o %t/one/out
# RUN: %lld -lSystem --icf=all --threads=8 %t/test.o -o %t/eight/out
# RUN: cmp %t/one/out %t/eight/out

## __data's first input is byte aligned and lands in a later chunk than the
## first input of the link; its most-aligned input is the very last input
## section of all. The chunk-ordered alignment fold must still see it.
# RUN: llvm-readobj --sections %t/one/out | FileCheck %s --check-prefix=ALIGN
# ALIGN:      Name: __data
# ALIGN-NEXT: Segment: __DATA
# ALIGN:      Alignment: 12

## The cold function is defined well past the first chunk boundary and must
## still be partitioned to the end of __text, i.e. after _main.
# RUN: llvm-objdump -d %t/one/out | FileCheck %s --check-prefix=COLD
# COLD: <_main>:
# COLD: <_cold>:

## ICF folded each of the four unique bodies, whose copies are spread over every
## chunk, so the per-chunk foldable lists were concatenated in chunk order.
# RUN: llvm-objdump --syms %t/one/out | FileCheck %s --check-prefix=ICF
# ICF-DAG: [[#%x,G0:]] g     F __TEXT,__text _g000000
# ICF-DAG: [[#%x,G0]]  g     F __TEXT,__text _g033333
# ICF-DAG: [[#%x,G3:]] g     F __TEXT,__text _g300000
# ICF-DAG: [[#%x,G3]]  g     F __TEXT,__text _g333333

.subsections_via_symbols

.text
.macro gen_4 c
  .globl _g0\c, _g1\c, _g2\c, _g3\c
  _g0\c:; movl $0, %eax; ret
  _g1\c:; movl $1, %eax; ret
  _g2\c:; movl $2, %eax; ret
  _g3\c:; movl $3, %eax; ret
.endm
.macro gen_16 c
  gen_4 0\c
  gen_4 1\c
  gen_4 2\c
  gen_4 3\c
.endm
.macro gen_64 c
  gen_16 0\c
  gen_16 1\c
  gen_16 2\c
  gen_16 3\c
.endm
.macro gen_256 c
  gen_64 0\c
  gen_64 1\c
  gen_64 2\c
  gen_64 3\c
.endm
.macro gen_1024 c
  gen_256 0\c
  gen_256 1\c
  gen_256 2\c
  gen_256 3\c
.endm
gen_1024 0
gen_1024 1
gen_1024 2
gen_1024 3

.globl _cold
.desc _cold, 0x400
_cold:
  movl $9, %eax
  ret

.globl _main
_main:
  callq _cold
  ret

.data
.macro data_4 c
  .globl _d0\c, _d1\c, _d2\c, _d3\c
  _d0\c: .byte 0
  _d1\c: .byte 1
  _d2\c: .byte 2
  _d3\c: .byte 3
.endm
.macro data_16 c
  data_4 0\c
  data_4 1\c
  data_4 2\c
  data_4 3\c
.endm
.macro data_64 c
  data_16 0\c
  data_16 1\c
  data_16 2\c
  data_16 3\c
.endm
.macro data_256 c
  data_64 0\c
  data_64 1\c
  data_64 2\c
  data_64 3\c
.endm
.macro data_1024 c
  data_256 0\c
  data_256 1\c
  data_256 2\c
  data_256 3\c
.endm
data_1024 0
data_1024 1
data_1024 2
data_1024 3

## The last input section of the whole link, and the only one needing more than
## byte alignment.
.globl _data_aligned
.p2align 12
_data_aligned:
  .quad 0x1234
