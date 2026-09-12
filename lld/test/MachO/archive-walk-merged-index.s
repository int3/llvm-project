# REQUIRES: x86
## Two things the walker does off the ordered argument walk: it reads each held
## archive's index -- the index itself, its entries' member offsets, the member
## maps and each member's undefined names -- as worker tasks started when the
## archive is held, and it derives both the earliest provider of each name and the
## entries the serial walk has to visit from one chunked pass over the index, with
## one symbol-table lookup per entry instead of two passes and two lookups.
## Neither which member is extracted, nor the order, nor the output bytes may
## change, at any thread count.
##
## What this covers that parallel-input-archive.s does not: three archives held at
## once, so three prepass chains run concurrently and a member of the first
## schedules an entry of the second, and the per-archive candidate lists are built
## from several chunks each; an archive whose index is wider than one 8192-entry
## chunk, so the offset resolution and the candidate lists both span chunks and
## the lists have to be concatenated in index order; and an object after the
## archives, which ends the phase where it stands, abandons whatever of the
## prepass has not finished, and publishes the indexes the ordinary way.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/x1 %t/x2 %t/x8 %t/x16 %t/d1 %t/d2 %t/d8 %t/d16

# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/late-main.s -o %t/late-main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/late.s -o %t/late.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/unused.s -o %t/unused.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/b.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wide.s -o %t/wide.o
# RUN: llvm-ar --format=darwin rcs %t/liba.a %t/a.o %t/unused.o
# RUN: llvm-ar --format=darwin rcs %t/libb.a %t/b.o
# RUN: llvm-ar --format=darwin rcs %t/libwide.a %t/wide.o

## Three held archives, one of them with 10,001 index entries. _fromA extracts
## from the first archive; the member it brings in references _fromB, whose only
## provider is still ahead of the walk, so the entry is scheduled and extracted
## when the walk reaches the second archive; _s9999 extracts from the wide
## archive at an index entry past the first chunk boundary.
# RUN: %lld -lSystem -o %t/x1/out --threads=1 -why_load %t/main.o %t/liba.a %t/libb.a %t/libwide.a > %t/x1/why
# RUN: %lld -lSystem -o %t/x2/out --threads=2 -why_load %t/main.o %t/liba.a %t/libb.a %t/libwide.a > %t/x2/why
# RUN: %lld -lSystem -o %t/x8/out --threads=8 -why_load %t/main.o %t/liba.a %t/libb.a %t/libwide.a > %t/x8/why
# RUN: %lld -lSystem -o %t/x16/out --threads=16 -why_load %t/main.o %t/liba.a %t/libb.a %t/libwide.a > %t/x16/why
# RUN: cmp %t/x1/out %t/x2/out
# RUN: cmp %t/x1/out %t/x8/out
# RUN: cmp %t/x1/out %t/x16/out
# RUN: cmp %t/x1/why %t/x2/why
# RUN: cmp %t/x1/why %t/x8/why
# RUN: cmp %t/x1/why %t/x16/why
# RUN: FileCheck %s --check-prefix=WHY < %t/x1/why
# RUN: llvm-objdump --syms %t/x1/out | FileCheck %s --check-prefix=SYMS

## The member that is never extracted must not be there, and the wide archive's
## 10,000 other index entries must not drag anything in.
# WHY:      _fromA forced load of {{.*}}liba.a(a.o)
# WHY-NEXT: _fromB forced load of {{.*}}libb.a(b.o)
# WHY-NEXT: _s9999 forced load of {{.*}}libwide.a(wide.o)
# WHY-NOT:  forced load

# SYMS-DAG: g     F __TEXT,__text _fromA
# SYMS-DAG: g     F __TEXT,__text _fromB
# SYMS-DAG: g     F __TEXT,__text _s9999
# SYMS-NOT: _unused

## An object after the archives ends the phase where it stands, which abandons
## whatever of the prepass has not finished and publishes the indexes the
## ordinary way. Same extraction, same bytes.
# RUN: %lld -lSystem -o %t/d1/out --threads=1 -why_load %t/late-main.o %t/liba.a %t/libb.a %t/libwide.a %t/late.o > %t/d1/why
# RUN: %lld -lSystem -o %t/d2/out --threads=2 -why_load %t/late-main.o %t/liba.a %t/libb.a %t/libwide.a %t/late.o > %t/d2/why
# RUN: %lld -lSystem -o %t/d8/out --threads=8 -why_load %t/late-main.o %t/liba.a %t/libb.a %t/libwide.a %t/late.o > %t/d8/why
# RUN: %lld -lSystem -o %t/d16/out --threads=16 -why_load %t/late-main.o %t/liba.a %t/libb.a %t/libwide.a %t/late.o > %t/d16/why
# RUN: cmp %t/d1/out %t/d2/out
# RUN: cmp %t/d1/out %t/d8/out
# RUN: cmp %t/d1/out %t/d16/out
# RUN: cmp %t/d1/why %t/d2/why
# RUN: cmp %t/d1/why %t/d8/why
# RUN: cmp %t/d1/why %t/d16/why
# RUN: FileCheck %s --check-prefix=WHY < %t/d1/why

#--- main.s
.globl _main
_main:
  callq _fromA
  callq _s9999
  ret

#--- late-main.s
.globl _main
_main:
  callq _fromA
  callq _s9999
  callq _late
  ret

#--- late.s
.globl _late
_late:
  ret

#--- a.s
.globl _fromA
_fromA:
  callq _fromB
  ret

#--- unused.s
.globl _unused
_unused:
  ret

#--- b.s
.globl _fromB
_fromB:
  ret

#--- wide.s
.irpc a, 0123456789
.irpc b, 0123456789
.irpc c, 0123456789
.irpc d, 0123456789
.globl _s\a\b\c\d
_s\a\b\c\d:
  nop
.endr
.endr
.endr
.endr
