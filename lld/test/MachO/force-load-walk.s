# REQUIRES: x86
## -force_load loads every member of one archive at that archive's own position,
## before its index is published. A forced member's undefined symbol can only be
## satisfied from an archive already registered, so a provider in a later archive
## is loaded from its own index entry, with the symbol as the reason. The walker
## replays all of that as a prelude; nothing may depend on the thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/p1.s -o %t/p1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/f1.s -o %t/f1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/f2.s -o %t/f2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/t.s -o %t/t.o
# RUN: llvm-ar --format=darwin rcs %t/a1.a %t/p1.o
# RUN: llvm-ar --format=darwin rcs %t/a2.a %t/f1.o %t/f2.o
# RUN: llvm-ar --format=darwin rcs %t/a3.a %t/t.o

# RUN: mkdir -p %t/f1 %t/f2 %t/f8
# RUN: %lld -lSystem -o %t/f1/out --threads=1 -why_load %t/main.o %t/a1.a -force_load %t/a2.a %t/a3.a > %t/why1.txt
# RUN: %lld -lSystem -o %t/f2/out --threads=2 -why_load %t/main.o %t/a1.a -force_load %t/a2.a %t/a3.a > %t/why2.txt
# RUN: %lld -lSystem -o %t/f8/out --threads=8 -why_load %t/main.o %t/a1.a -force_load %t/a2.a %t/a3.a > %t/why8.txt
# RUN: cmp %t/f1/out %t/f2/out
# RUN: cmp %t/f1/out %t/f8/out
# RUN: diff %t/why1.txt %t/why2.txt
# RUN: diff %t/why1.txt %t/why8.txt
# RUN: FileCheck %s --check-prefix=WHY < %t/why1.txt

# WHY:      _p1 forced load of {{.*}}a1.a(p1.o)
# WHY-NEXT: -force_load forced load of {{.*}}a2.a(f1.o)
# WHY-NEXT: -force_load forced load of {{.*}}a2.a(f2.o)
# WHY-NEXT: _third forced load of {{.*}}a3.a(t.o)

# RUN: llvm-objdump --syms %t/f1/out | FileCheck %s --check-prefix=SYMS
# SYMS-DAG: __TEXT,p1 _p1
# SYMS-DAG: __TEXT,f1 _f1
# SYMS-DAG: __TEXT,f2 _f2
# SYMS-DAG: __TEXT,th _third

#--- main.s
.globl _main
_main:
  callq _p1
  retq

#--- p1.s
.globl _p1
.section __TEXT,p1
_p1:
  retq

#--- f1.s
.globl _f1
.section __TEXT,f1
_f1:
  callq _third
  retq

#--- f2.s
.globl _f2
.section __TEXT,f2
_f2:
  retq

#--- t.s
.globl _third
.section __TEXT,th
_third:
  retq
