# REQUIRES: x86 && thread_support
## Before the commit, the output's file mapping drops its page-table entries
## on several threads (Linux only; a no-op elsewhere and at one thread). The
## written pages stay in the page cache, so the file must be the one-thread
## link's, byte for byte. Writing to stdout uses an in-memory buffer, which
## must keep its data: the hook does nothing there.

# RUN: rm -rf %t; mkdir -p %t/t1 %t/t2 %t/t8 %t/t16
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t/test.o
# RUN: %lld -no_uuid --threads=1 %t/test.o -o %t/t1/out
# RUN: %lld -no_uuid --threads=2 %t/test.o -o %t/t2/out
# RUN: %lld -no_uuid --threads=8 %t/test.o -o %t/t8/out --time-trace --time-trace-granularity=0 --time-trace=%t/t8.json
# RUN: %lld -no_uuid --threads=16 %t/test.o -o %t/t16/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: cmp %t/t1/out %t/t16/out
# RUN: FileCheck %s --check-prefix=TRACE < %t/t8.json
# RUN: %lld -no_uuid --threads=8 %t/test.o -o - > %t/stdout
# RUN: cmp %t/t1/out %t/stdout

# TRACE: Release output mapping

.globl _main
.text
_main:
  ret

## About 1.5 MiB of non-zero data, so that each range covers many pages.
.data
.space 1572864, 0xab
