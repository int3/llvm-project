# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/input.s -o %t/input.o
# RUN: %lld -e Entry %t/input.o -order_file %t/order -o %t/order.out
# RUN: llvm-nm --numeric-sort --format=just-symbols %t/order.out | FileCheck %s --check-prefix=ORDER
# RUN: %lld -e Entry %t/input.o -order_file %t/short-order -o %t/short.out
# RUN: llvm-nm --numeric-sort --format=just-symbols %t/short.out | FileCheck %s --check-prefix=SHORT

## `aaap` hashes to the same prefix-filter bit as the order-file key `aaac`,
## but must still fall through to the exact normalized-name lookup.

# ORDER:      matched.__uniq.1.llvm.2.Tgm
# ORDER-NEXT: repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm
# ORDER-NEXT: aaac
# ORDER-NEXT: qualified
# ORDER-NEXT: Entry
# ORDER-NEXT: unrelated.dot
# ORDER-NEXT: aaap
# ORDER-NEXT: foo.llvm.1
# ORDER-NEXT: short_other
# ORDER-NEXT: xyz

## A normalized key shorter than four bytes disables the filter, so the raw
## suffixed spelling is still ordered by the existing exact lookup.
# SHORT:      foo.llvm.1
# SHORT-NEXT: Entry

#--- order
matched
repeat.__uniq.one.__uniq.other.llvm.x.llvm.y.Tgm
aaac
input.o:qualified

#--- short-order
foo

#--- input.s
.subsections_via_symbols
.text
.globl Entry, unrelated.dot, matched.__uniq.1.llvm.2.Tgm
.globl repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm
.globl aaap, aaac, qualified, foo.llvm.1, short_other, xyz
Entry:
  retq
unrelated.dot:
  retq
matched.__uniq.1.llvm.2.Tgm:
  retq
repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm:
  retq
aaap:
  retq
aaac:
  retq
qualified:
  retq
foo.llvm.1:
  retq
short_other:
  retq
xyz:
  retq
