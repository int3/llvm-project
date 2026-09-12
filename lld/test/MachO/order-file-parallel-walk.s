# REQUIRES: x86

## The order-file priority walk is chunked across input files (64 files per
## chunk) and runs on workers. This links more than one chunk's worth of
## object files with an order file that names symbols in reverse file order,
## so the per-chunk candidate lists must be merged in file order for the
## layout to come out right. The link must also be byte-identical at one and
## eight threads.
##
## The archive member covers the parts of the fold that must stay serial: its
## file-scoped entry (`libx.a(member.o):_amem1`) reaches
## SymbolPriorityEntry::getPriority's saver().save() path, and both of its
## globals live in one input section, so that section takes the min over two
## matched entries. sym00 is named twice in the order file, so the entry
## itself also folds two priorities.

# RUN: rm -rf %t && mkdir -p %t/t1 %t/t8
# RUN: %python %S/Inputs/gen-order-file-parallel-walk.py %t llvm-mc x86_64-apple-darwin
# RUN: rm -f %t/libx.a && llvm-ar rcs %t/libx.a %t/member.o
## Link at one and eight threads into identically named outputs in separate
## directories, since the output basename is embedded in the image.
# RUN: %lld -e Entry -order_file %t/order -o %t/t1/out --threads=1 %t/f*.o -force_load %t/libx.a
# RUN: %lld -e Entry -order_file %t/order -o %t/t8/out --threads=8 %t/f*.o -force_load %t/libx.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: llvm-nm --numeric-sort --format=just-symbols %t/t1/out | FileCheck %s

## Ordered symbols come first, in order-file order: sym00 from its first
## (lowest-ordered) entry, then the rest in reverse file order, then the
## archive member's section; the unordered Entry comes last.
# CHECK: sym00
# CHECK-NEXT: sym65
# CHECK-NEXT: sym64
# CHECK-NEXT: sym63
# CHECK-NEXT: sym62
# CHECK-NEXT: sym61
# CHECK-NEXT: sym60
# CHECK-NEXT: sym59
# CHECK-NEXT: sym58
# CHECK-NEXT: sym57
# CHECK-NEXT: sym56
# CHECK-NEXT: sym55
# CHECK-NEXT: sym54
# CHECK-NEXT: sym53
# CHECK-NEXT: sym52
# CHECK-NEXT: sym51
# CHECK-NEXT: sym50
# CHECK-NEXT: sym49
# CHECK-NEXT: sym48
# CHECK-NEXT: sym47
# CHECK-NEXT: sym46
# CHECK-NEXT: sym45
# CHECK-NEXT: sym44
# CHECK-NEXT: sym43
# CHECK-NEXT: sym42
# CHECK-NEXT: sym41
# CHECK-NEXT: sym40
# CHECK-NEXT: sym39
# CHECK-NEXT: sym38
# CHECK-NEXT: sym37
# CHECK-NEXT: sym36
# CHECK-NEXT: sym35
# CHECK-NEXT: sym34
# CHECK-NEXT: sym33
# CHECK-NEXT: sym32
# CHECK-NEXT: sym31
# CHECK-NEXT: sym30
# CHECK-NEXT: sym29
# CHECK-NEXT: sym28
# CHECK-NEXT: sym27
# CHECK-NEXT: sym26
# CHECK-NEXT: sym25
# CHECK-NEXT: sym24
# CHECK-NEXT: sym23
# CHECK-NEXT: sym22
# CHECK-NEXT: sym21
# CHECK-NEXT: sym20
# CHECK-NEXT: sym19
# CHECK-NEXT: sym18
# CHECK-NEXT: sym17
# CHECK-NEXT: sym16
# CHECK-NEXT: sym15
# CHECK-NEXT: sym14
# CHECK-NEXT: sym13
# CHECK-NEXT: sym12
# CHECK-NEXT: sym11
# CHECK-NEXT: sym10
# CHECK-NEXT: sym09
# CHECK-NEXT: sym08
# CHECK-NEXT: sym07
# CHECK-NEXT: sym06
# CHECK-NEXT: sym05
# CHECK-NEXT: sym04
# CHECK-NEXT: sym03
# CHECK-NEXT: sym02
# CHECK-NEXT: sym01
# CHECK-NEXT: _amem0
# CHECK-NEXT: _amem1
# CHECK-NEXT: Entry
