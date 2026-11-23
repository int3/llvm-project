# REQUIRES: x86 && thread_support
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/archive.s -o %t/archive.o
# RUN: llvm-ar rcs %t/libfoo.a %t/archive.o
# RUN: %lld --read-workers=0 %t/main.o -L%t -lfoo -o %t/out
# RUN: cp %t/out %t/expected
# RUN: %lld %t/main.o -L%t -lfoo -o %t/out
# RUN: cmp %t/expected %t/out
# RUN: %lld --threads=1 %t/main.o -L%t -lfoo -o %t/out
# RUN: cmp %t/expected %t/out
# RUN: %lld -ObjC %t/main.o %t/libfoo.a -o %t/out
# RUN: cmp %t/expected %t/out
# RUN: %lld -ObjC --threads=1 %t/main.o %t/libfoo.a -o %t/out
# RUN: cmp %t/expected %t/out

#--- main.s
.globl _main
_main:
  callq _foo
  retq

#--- archive.s
.globl _foo
_foo:
  retq
.data
.space 10485761
