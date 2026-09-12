# REQUIRES: x86
## -all_load loads every member of every command-line archive at that archive's
## own position, before its index is published, so a member's reference into a
## later archive does not fetch anything there and an unreferenced member is
## loaded anyway. The walker's prelude must reproduce that at any thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/x1.s -o %t/x1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/x2.s -o %t/x2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/y1.s -o %t/y1.o
# RUN: llvm-ar --format=darwin rcs %t/x.a %t/x1.o %t/x2.o
# RUN: llvm-ar --format=darwin rcs %t/y.a %t/y1.o

# RUN: mkdir -p %t/a1 %t/a2 %t/a8
# RUN: %lld -lSystem -all_load -o %t/a1/out --threads=1 -why_load %t/main.o %t/x.a %t/y.a > %t/why1.txt
# RUN: %lld -lSystem -all_load -o %t/a2/out --threads=2 -why_load %t/main.o %t/x.a %t/y.a > %t/why2.txt
# RUN: %lld -lSystem -all_load -o %t/a8/out --threads=8 -why_load %t/main.o %t/x.a %t/y.a > %t/why8.txt
# RUN: cmp %t/a1/out %t/a2/out
# RUN: cmp %t/a1/out %t/a8/out
# RUN: diff %t/why1.txt %t/why2.txt
# RUN: diff %t/why1.txt %t/why8.txt
# RUN: FileCheck %s --check-prefix=WHY < %t/why1.txt

# WHY:      -all_load forced load of {{.*}}x.a(x1.o)
# WHY-NEXT: -all_load forced load of {{.*}}x.a(x2.o)
# WHY-NEXT: -all_load forced load of {{.*}}y.a(y1.o)

# RUN: llvm-objdump --syms %t/a1/out | FileCheck %s --check-prefix=SYMS
# SYMS-DAG: __TEXT,x1 _x1
# SYMS-DAG: __TEXT,x2 _x2
# SYMS-DAG: __TEXT,y1 _y1

#--- main.s
.globl _main
_main:
  callq _x1
  retq

#--- x1.s
.globl _x1
.section __TEXT,x1
_x1:
  callq _y1
  retq

#--- x2.s
.globl _x2
.section __TEXT,x2
_x2:
  retq

#--- y1.s
.globl _y1
.section __TEXT,y1
_y1:
  callq _x1
  retq
