# REQUIRES: x86
## The fold's winner is a function of the record set and the records' ordinals,
## not of the order the workers arrive in. LLD_REVERSE_HELD_PARSE reverses that
## arrival order, so a strong definition can be applied before the weak one it
## displaces and a low-ordinal weak definition can arrive last: both must still
## produce the serial linker's output, its diagnostics and its unwind info.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/ref %t/f1 %t/f8 %t/r1 %t/r2 %t/r8 %t/r16
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/weak1.s -o %t/weak1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/weak2.s -o %t/weak2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/strong.s -o %t/strong.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/tail.s -o %t/tail.o

## _foo is defined weakly at ordinals 1 and 2 and strongly at ordinal 3, so the
## strong definition displaces a weak prefix winner. Reversed, the workers apply
## the strong definition first and the lowest-ordinal weak one last.
# RUN: %lld -lSystem -o %t/ref/out --threads=1 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/tail.o
# RUN: %lld -lSystem -o %t/f8/out --threads=8 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/tail.o
# RUN: cmp %t/ref/out %t/f8/out
# RUN: env LLD_REVERSE_HELD_PARSE=1 %lld -lSystem -o %t/r1/out --threads=1 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/tail.o
# RUN: env LLD_REVERSE_HELD_PARSE=1 %lld -lSystem -o %t/r2/out --threads=2 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/tail.o
# RUN: env LLD_REVERSE_HELD_PARSE=1 %lld -lSystem -o %t/r8/out --threads=8 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/tail.o
# RUN: env LLD_REVERSE_HELD_PARSE=1 %lld -lSystem -o %t/r16/out --threads=16 %t/main.o %t/weak1.o %t/weak2.o %t/strong.o %t/tail.o
# RUN: cmp %t/ref/out %t/r1/out
# RUN: cmp %t/ref/out %t/r2/out
# RUN: cmp %t/ref/out %t/r8/out
# RUN: cmp %t/ref/out %t/r16/out
# RUN: llvm-objdump --syms %t/ref/out | FileCheck %s

## The local aliases of both losing weak definitions move into the prevailing
## strong definition's section.
# CHECK-DAG: l     O __TEXT,strong _alias1
# CHECK-DAG: l     O __TEXT,strong _alias2
# CHECK-DAG: g     O __TEXT,strong _foo

## Duplicate strong definitions are diagnosed in input order however the workers
## apply them.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup1.s -o %t/dup1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup2.s -o %t/dup2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup3.s -o %t/dup3.o
# RUN: not env LLD_REVERSE_HELD_PARSE=1 %lld -lSystem -o /dev/null --threads=8 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not env LLD_REVERSE_HELD_PARSE=1 %lld -lSystem -o /dev/null --threads=16 %t/main.o %t/weak1.o %t/dup1.o %t/dup2.o %t/dup3.o 2>&1 | FileCheck %s --check-prefix=DUP

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
  callq _tail
  retq

#--- weak1.s
.globl _foo
.weak_definition _foo
.section __TEXT,weak1
_alias1:
_foo:
  retq

#--- weak2.s
.globl _foo
.weak_definition _foo
.section __TEXT,weak2
_alias2:
_foo:
  retq

#--- strong.s
.globl _foo
.section __TEXT,strong
_foo:
  retq

#--- tail.s
.globl _tail
_tail:
  callq _foo
  retq

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
