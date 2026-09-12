# REQUIRES: x86
## A strong dylib definition suppresses an archive extraction only when the dylib
## was registered before the archive that provides the name. Here archive b.a
## provides _shared and precedes libstrong.dylib, so the serial linker's index
## entry for _shared left a LazyArchive symbol that the dylib export could not
## displace; the reference that arrives later, from a member of a.a, therefore
## still extracts b.a's member. The walker leaves no lazy symbol behind, so it
## finds the dylib symbol in the slot and has to apply the rule against the
## provider's archive, not against the archive it is currently walking.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/amem.s -o %t/amem.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/bmem.s -o %t/bmem.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/strong.s -o %t/strong.o
# RUN: llvm-ar --format=darwin rcs %t/a.a %t/amem.o
# RUN: llvm-ar --format=darwin rcs %t/b.a %t/bmem.o
# RUN: %lld -dylib -install_name /usr/lib/libstrong.dylib -o %t/libstrong.dylib %t/strong.o

# RUN: mkdir -p %t/d1 %t/d2 %t/d8
# RUN: %lld -lSystem -o %t/d1/out --threads=1 -why_load %t/main.o %t/b.a %t/libstrong.dylib %t/a.a > %t/why1.txt
# RUN: %lld -lSystem -o %t/d2/out --threads=2 -why_load %t/main.o %t/b.a %t/libstrong.dylib %t/a.a > %t/why2.txt
# RUN: %lld -lSystem -o %t/d8/out --threads=8 -why_load %t/main.o %t/b.a %t/libstrong.dylib %t/a.a > %t/why8.txt
# RUN: cmp %t/d1/out %t/d2/out
# RUN: cmp %t/d1/out %t/d8/out
# RUN: diff %t/why1.txt %t/why2.txt
# RUN: diff %t/why1.txt %t/why8.txt
# RUN: FileCheck %s --check-prefix=WHY < %t/why1.txt

## The nested extraction is announced first: a.a's member is only published once
## its own parse, and so the extraction its undefined triggered, has finished.
# WHY:      _shared forced load of {{.*}}b.a(bmem.o)
# WHY-NEXT: _entrya forced load of {{.*}}a.a(amem.o)

## The definition comes from the archive member, not from the dylib.
# RUN: llvm-objdump --syms %t/d1/out | FileCheck %s --check-prefix=SYMS
# RUN: llvm-objdump --syms %t/d8/out | FileCheck %s --check-prefix=SYMS
# SYMS: __TEXT,bm _shared

## The mirror: the dylib before the providing archive does suppress the fetch.
# RUN: mkdir -p %t/m1 %t/m8
# RUN: %lld -lSystem -o %t/m1/out --threads=1 -why_load %t/main.o %t/libstrong.dylib %t/b.a %t/a.a > %t/mwhy1.txt
# RUN: %lld -lSystem -o %t/m8/out --threads=8 -why_load %t/main.o %t/libstrong.dylib %t/b.a %t/a.a > %t/mwhy8.txt
# RUN: cmp %t/m1/out %t/m8/out
# RUN: diff %t/mwhy1.txt %t/mwhy8.txt
# RUN: FileCheck %s --check-prefix=MWHY < %t/mwhy1.txt
# RUN: llvm-objdump --syms %t/m1/out | FileCheck %s --check-prefix=MSYMS

# MWHY:     _entrya forced load of {{.*}}a.a(amem.o)
# MWHY-NOT: bmem.o
# MSYMS-NOT: __TEXT,bm _shared

#--- main.s
.globl _main
_main:
  callq _entrya
  retq

#--- amem.s
.globl _entrya
.section __TEXT,am
_entrya:
  callq _shared
  retq

#--- bmem.s
.globl _shared
.section __TEXT,bm
_shared:
  retq

#--- strong.s
.globl _shared
_shared:
  retq
