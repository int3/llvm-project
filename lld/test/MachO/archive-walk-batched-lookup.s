# REQUIRES: x86
## The archive walker resolves the providers of an extracted member's names in
## staged blocks (prefetch the slots, probe, then compare) instead of one lookup
## at a time. big.o has 1,200 undefined names, more than any one block, whose
## providers sit in three archives: a1.a is before big.a on the command line, so
## its members are extracted from inside big.o's undefined walk (a recursion in
## the middle of a resolved block), while a2.a and a3.a are still ahead of the
## frontier and their entries are scheduled. Their members reference names in
## other archives in turn. Which members load, in what order, and the output
## must not depend on the thread count or on whether the member parses stream.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/t1 %t/t2 %t/t8 %t/ns
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/big.s -o %t/big.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/x01.s -o %t/x01.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/x23.s -o %t/x23.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/x456.s -o %t/x456.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/y.s -o %t/y.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/x789.s -o %t/x789.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/z.s -o %t/z.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/unused.s -o %t/unused.o
# RUN: llvm-ar --format=darwin rcs %t/a1.a %t/x01.o %t/x23.o %t/unused.o
# RUN: llvm-ar --format=darwin rcs %t/big.a %t/big.o
# RUN: llvm-ar --format=darwin rcs %t/a2.a %t/x456.o %t/y.o
# RUN: llvm-ar --format=darwin rcs %t/a3.a %t/z.o %t/x789.o

# RUN: %lld -lSystem -o %t/t1/out --threads=1 -why_load -map %t/t1/map %t/main.o %t/a1.a %t/big.a %t/a2.a %t/a3.a > %t/t1/why.txt
# RUN: %lld -lSystem -o %t/t2/out --threads=2 -why_load -map %t/t2/map %t/main.o %t/a1.a %t/big.a %t/a2.a %t/a3.a > %t/t2/why.txt
# RUN: %lld -lSystem -o %t/t8/out --threads=8 -why_load -map %t/t8/map %t/main.o %t/a1.a %t/big.a %t/a2.a %t/a3.a > %t/t8/why.txt
# RUN: env LLD_NO_STREAM_MEMBER_PARSE=1 %lld -lSystem -o %t/ns/out --threads=8 -why_load -map %t/ns/map %t/main.o %t/a1.a %t/big.a %t/a2.a %t/a3.a > %t/ns/why.txt
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: cmp %t/t1/out %t/ns/out
# RUN: cmp %t/t1/why.txt %t/t2/why.txt
# RUN: cmp %t/t1/why.txt %t/t8/why.txt
# RUN: cmp %t/t1/why.txt %t/ns/why.txt
# RUN: sed -e 's/[a-z0-9]*\/out$/OUT/' %t/t1/map > %t/t1/map.norm
# RUN: sed -e 's/[a-z0-9]*\/out$/OUT/' %t/t2/map > %t/t2/map.norm
# RUN: sed -e 's/[a-z0-9]*\/out$/OUT/' %t/t8/map > %t/t8/map.norm
# RUN: sed -e 's/[a-z0-9]*\/out$/OUT/' %t/ns/map > %t/ns/map.norm
# RUN: cmp %t/t1/map.norm %t/t2/map.norm
# RUN: cmp %t/t1/map.norm %t/t8/map.norm
# RUN: cmp %t/t1/map.norm %t/ns/map.norm
# RUN: FileCheck %s < %t/t1/why.txt

## The serial linker publishes a member after the members its parse extracted,
## so a1.a's members, extracted from inside big.o's undefined walk, come first.
# CHECK:      _x000 forced load of {{.*}}a1.a(x01.o)
# CHECK-NEXT: _x200 forced load of {{.*}}a1.a(x23.o)
# CHECK-NEXT: _big forced load of {{.*}}big.a(big.o)
# CHECK-NEXT: _x400 forced load of {{.*}}a2.a(x456.o)
# CHECK-NEXT: _y00 forced load of {{.*}}a2.a(y.o)
# CHECK-NEXT: _z00 forced load of {{.*}}a3.a(z.o)
# CHECK-NEXT: _x700 forced load of {{.*}}a3.a(x789.o)
# CHECK-NOT:  unused.o

#--- main.s
.globl _main
_main:
  callq _big
  retq

#--- big.s
.globl _big
.text
_big:
  retq
.data
.irp a,0,1,2,3,4,5,6,7,8,9
.irp b,0,1,2,3,4,5,6,7,8,9
.irp c,0,1,2,3,4,5,6,7,8,9
  .quad _x\a\b\c
.endr
.endr
.endr
.irp a,0,1,2,3,4,5,6,7,8,9
.irp b,0,1,2,3,4,5,6,7,8,9
  .quad _y\a\b
  .quad _z\a\b
.endr
.endr

#--- x01.s
.text
.irp a,0,1
.irp b,0,1,2,3,4,5,6,7,8,9
.irp c,0,1,2,3,4,5,6,7,8,9
.globl _x\a\b\c
_x\a\b\c:
  retq
.endr
.endr
.endr
## A reference into a3.a, which is still ahead of the frontier.
.data
  .quad _z50

#--- x23.s
.text
.irp a,2,3
.irp b,0,1,2,3,4,5,6,7,8,9
.irp c,0,1,2,3,4,5,6,7,8,9
.globl _x\a\b\c
_x\a\b\c:
  retq
.endr
.endr
.endr

#--- x456.s
.text
.irp a,4,5,6
.irp b,0,1,2,3,4,5,6,7,8,9
.irp c,0,1,2,3,4,5,6,7,8,9
.globl _x\a\b\c
_x\a\b\c:
  retq
.endr
.endr
.endr

#--- y.s
.text
.irp a,0,1,2,3,4,5,6,7,8,9
.irp b,0,1,2,3,4,5,6,7,8,9
.globl _y\a\b
_y\a\b:
  retq
.endr
.endr
## A reference back into a1.a, whose member is already extracted.
.data
  .quad _x123

#--- x789.s
.text
.irp a,7,8,9
.irp b,0,1,2,3,4,5,6,7,8,9
.irp c,0,1,2,3,4,5,6,7,8,9
.globl _x\a\b\c
_x\a\b\c:
  retq
.endr
.endr
.endr

#--- z.s
.text
.irp a,0,1,2,3,4,5,6,7,8,9
.irp b,0,1,2,3,4,5,6,7,8,9
.globl _z\a\b
_z\a\b:
  retq
.endr
.endr

#--- unused.s
.text
.globl _unused
_unused:
  retq
