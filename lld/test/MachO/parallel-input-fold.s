# REQUIRES: x86
## The parallel input phase folds direct-object symbol records on workers. Every
## order-sensitive transition must produce the same bytes and the same
## diagnostics at every thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/r1 %t/r16 %t/r8 %t/s1 %t/s16 %t/s2 %t/s8 %t/t1 %t/t16 %t/t2 %t/t4 %t/t8
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/weak1.s -o %t/weak1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/weak2.s -o %t/weak2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/strong.s -o %t/strong.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/common1.s -o %t/common1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/common2.s -o %t/common2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/ref.s -o %t/ref.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o

## Weak/weak coalescing with a local alias at the losing offset, a strong
## definition that outranks both, and equal-size commons in both orders.
# RUN: %lld -lSystem -o %t/t1/out --threads=1 %t/main.o %t/weak1.o %t/weak2.o %t/ref.o %t/common1.o %t/common2.o
# RUN: %lld -lSystem -o %t/t2/out --threads=2 %t/main.o %t/weak1.o %t/weak2.o %t/ref.o %t/common1.o %t/common2.o
# RUN: %lld -lSystem -o %t/t4/out --threads=4 %t/main.o %t/weak1.o %t/weak2.o %t/ref.o %t/common1.o %t/common2.o
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/weak1.o %t/weak2.o %t/ref.o %t/common1.o %t/common2.o
# RUN: %lld -lSystem -o %t/t16/out --threads=16 %t/main.o %t/weak1.o %t/weak2.o %t/ref.o %t/common1.o %t/common2.o
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t4/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: cmp %t/t1/out %t/t16/out
# RUN: llvm-objdump --syms %t/t1/out | FileCheck %s --check-prefix=WEAKFIRST

# RUN: %lld -lSystem -o %t/r1/out --threads=1 %t/main.o %t/weak2.o %t/weak1.o %t/ref.o %t/common2.o %t/common1.o
# RUN: %lld -lSystem -o %t/r8/out --threads=8 %t/main.o %t/weak2.o %t/weak1.o %t/ref.o %t/common2.o %t/common1.o
# RUN: %lld -lSystem -o %t/r16/out --threads=16 %t/main.o %t/weak2.o %t/weak1.o %t/ref.o %t/common2.o %t/common1.o
# RUN: cmp %t/r1/out %t/r8/out
# RUN: cmp %t/r1/out %t/r16/out

## A strong definition after two weak ones: the strong prevails, the weak
## sections coalesce, and the __eh_frame FDE of the weak prefix winner is still
## emitted because -dead_strip is off.
# RUN: %lld -lSystem -o %t/s1/out --threads=1 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/ref.o
# RUN: %lld -lSystem -o %t/s2/out --threads=2 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/ref.o
# RUN: %lld -lSystem -o %t/s8/out --threads=8 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/ref.o
# RUN: %lld -lSystem -o %t/s16/out --threads=16 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/ref.o
# RUN: cmp %t/s1/out %t/s2/out
# RUN: cmp %t/s1/out %t/s8/out
# RUN: cmp %t/s1/out %t/s16/out
# RUN: llvm-objdump --syms %t/s1/out | FileCheck %s --check-prefix=STRONGWINS

## Three files defining the same strong symbol: the duplicate diagnostics must
## appear in input order at every thread count.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup1.s -o %t/dup1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup2.s -o %t/dup2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup3.s -o %t/dup3.o
# RUN: not %lld -lSystem -o /dev/null --threads=1 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -o /dev/null --threads=2 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -o /dev/null --threads=4 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -o /dev/null --threads=8 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -o /dev/null --threads=16 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP

# WEAKFIRST-DAG: w O __TEXT,weak1 _foo
# WEAKFIRST-DAG: g     O __DATA,__common _com
# STRONGWINS-DAG: g     O __TEXT,strong _foo

# DUP:      error: duplicate symbol: _dup
# DUP-NEXT: >>> defined in {{.*}}dup1.o
# DUP-NEXT: >>> defined in {{.*}}dup2.o
# DUP:      error: duplicate symbol: _dup
# DUP-NEXT: >>> defined in {{.*}}dup2.o
# DUP-NEXT: >>> defined in {{.*}}dup3.o

#--- main.s
.globl _main
_main:
  callq _foo
  retq

#--- weak1.s
.globl _foo
.weak_definition _foo
.section __TEXT,weak1
_local_alias_1:
_foo:
  retq

#--- weak2.s
.globl _foo
.weak_definition _foo
.section __TEXT,weak2
_local_alias_2:
_foo:
  retq

#--- strong.s
.globl _foo
.section __TEXT,strong
_foo:
  retq

#--- ref.s
.globl _refs
_refs:
  callq _foo
  retq

#--- common1.s
.comm _com,8,3

#--- common2.s
.comm _com,8,3

#--- dup1.s
.globl _dup
.section __TEXT,dup1
_dup:
  retq

#--- dup2.s
.globl _dup
.section __TEXT,dup2
_dup:
  retq

#--- dup3.s
.globl _dup
.section __TEXT,dup3
_dup:
  retq
