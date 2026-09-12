# REQUIRES: x86
## A thin archive's members are separate files, and llvm::object::Archive opens
## one lazily by appending to its ThinBuffers vector without a lock, so the
## walker -- which reads members on workers, both in the index prepass and in the
## member parses -- must not own a thin archive's index. canWalk() declines, the
## archive publishes its index the ordinary way on the main thread, and the
## output is the same at every thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/t1 %t/t2 %t/t8 %t/t16 %t/r1 %t/r8

# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/b.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/unused.s -o %t/unused.o

## The same members, once as a thin archive and once as a regular one.
# RUN: llvm-ar --format=gnu rcsT %t/thin.a %t/a.o %t/b.o %t/unused.o
# RUN: llvm-ar --format=darwin rcs %t/regular.a %t/a.o %t/b.o %t/unused.o

# RUN: %lld -lSystem -o %t/t1/out --threads=1 -why_load %t/main.o %t/thin.a > %t/t1/why
# RUN: %lld -lSystem -o %t/t2/out --threads=2 -why_load %t/main.o %t/thin.a > %t/t2/why
# RUN: %lld -lSystem -o %t/t8/out --threads=8 -why_load %t/main.o %t/thin.a > %t/t8/why
# RUN: %lld -lSystem -o %t/t16/out --threads=16 -why_load %t/main.o %t/thin.a > %t/t16/why
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: cmp %t/t1/out %t/t16/out
# RUN: cmp %t/t1/why %t/t8/why
# RUN: cmp %t/t1/why %t/t16/why

## Same extraction as from a regular archive, which the walker does own.
# RUN: %lld -lSystem -o %t/r1/out --threads=1 -why_load %t/main.o %t/regular.a > %t/r1/why
# RUN: %lld -lSystem -o %t/r8/out --threads=8 -why_load %t/main.o %t/regular.a > %t/r8/why
# RUN: cmp %t/r1/out %t/r8/out
# RUN: sed -e 's/regular\.a/ARCHIVE/' %t/r1/why > %t/r1/why.norm
# RUN: sed -e 's/thin\.a/ARCHIVE/' %t/t1/why > %t/t1/why.norm
# RUN: cmp %t/r1/why.norm %t/t1/why.norm
# RUN: FileCheck %s --check-prefix=WHY < %t/t1/why.norm

# WHY:      _fromA forced load of {{.*}}ARCHIVE(a.o)
# WHY-NEXT: _fromB forced load of {{.*}}ARCHIVE(b.o)
# WHY-NOT:  forced load

#--- main.s
.globl _main
_main:
  callq _fromA
  ret

#--- a.s
.globl _fromA
_fromA:
  callq _fromB
  ret

#--- b.s
.globl _fromB
_fromB:
  ret

#--- unused.s
.globl _unused
_unused:
  ret
