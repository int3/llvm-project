# REQUIRES: x86
## Symbols are checked against the order file's key prefixes before the exact
## lookup. Make sure that filter only ever rejects symbols that cannot match.

# RUN: rm -rf %t && split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/input.s -o %t/input.o

# RUN: %lld -e Entry %t/input.o -order_file %t/order -o %t/order.out
# RUN: llvm-nm --numeric-sort --format=just-symbols %t/order.out | FileCheck %s --check-prefix=ORDER
## aaacx passes the filter for key aaac, but has to fail the exact lookup.
# ORDER:      matched.__uniq.1.llvm.2.Tgm
# ORDER-NEXT: repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm
# ORDER-NEXT: aaac
# ORDER-NEXT: qualified
# ORDER-NEXT: Entry
# ORDER-NEXT: unrelated.dot
# ORDER-NEXT: aaacx
# ORDER-NEXT: foo.llvm.1
# ORDER-NEXT: xyz

## With every key at least 8 bytes long, the filter looks at 8-byte prefixes.
# RUN: %lld -e Entry %t/input.o -order_file %t/long-order -o %t/long.out
# RUN: llvm-nm --numeric-sort --format=just-symbols %t/long.out | FileCheck %s --check-prefix=LONG
# LONG:      repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm
# LONG-NEXT: qualified
# LONG-NEXT: Entry

## A key shorter than 4 bytes disables the filter.
# RUN: %lld -e Entry %t/input.o -order_file %t/short-order -o %t/short.out
# RUN: llvm-nm --numeric-sort --format=just-symbols %t/short.out | FileCheck %s --check-prefix=SHORT
# SHORT:      foo.llvm.1
# SHORT-NEXT: Entry

#--- order
matched
repeat.__uniq.one.__uniq.other.llvm.x.llvm.y.Tgm
aaac
input.o:qualified

#--- long-order
repeat.__uniq.one.__uniq.other.llvm.x.llvm.y.Tgm
input.o:qualified

#--- short-order
foo

#--- input.s
.subsections_via_symbols
.text
.globl Entry, unrelated.dot, matched.__uniq.1.llvm.2.Tgm
.globl repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm
.globl aaacx, aaac, qualified, foo.llvm.1, xyz
Entry:
  retq
unrelated.dot:
  retq
matched.__uniq.1.llvm.2.Tgm:
  retq
repeat.__uniq.one.__uniq.two.llvm.three.llvm.four.Tgm:
  retq
aaacx:
  retq
aaac:
  retq
qualified:
  retq
foo.llvm.1:
  retq
xyz:
  retq
