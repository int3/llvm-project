# REQUIRES: x86
## Split out of parallel-input-archive.s because the diagnostic comes from
## fatal(): the LLD_IN_TEST=2 harness suppresses the output of every iteration
## but the last, and a fatal() leaves the linker context undestroyed, so the
## harness exits after the first iteration with its output already discarded.
# UNSUPPORTED: main-run-twice

## The parallel input phase packs a record ordinal out of a top-level input
## ordinal, a walk generation and a within-walk index. LLD_ORDINAL_BITS narrows
## those widths so the overflow check can be reached; a top-level ordinal that
## does not fit must be diagnosed, not silently truncated.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dysmain.s -o %t/dysmain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dyshead.s -o %t/dyshead.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dysn1.s -o %t/dysn1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dysn2.s -o %t/dysn2.o
# RUN: llvm-ar --format=darwin rcs %t/dys.a %t/dyshead.o %t/dysn1.o %t/dysn2.o
# RUN: mkdir -p %t/narrowtop
# RUN: env LLD_ORDINAL_BITS=1,23,23 not %lld -lSystem -o %t/narrowtop/out --threads=8 %t/dysmain.o %t/dys.a 2>&1 | FileCheck %s --check-prefix=ORDOVER

# ORDOVER: error: input record ordinal out of range

#--- dysmain.s
.globl _main
_main:
  callq _head
  retq

#--- dyshead.s
.globl _head
.comm _shared_common,8,3
.section __TEXT,dh
_head:
  callq _n1
  callq _n2
  retq

#--- dysn1.s
.globl _n1
.comm _shared_common,8,3
.section __TEXT,d1
_n1:
  retq

#--- dysn2.s
.globl _n2
.section __TEXT,d2
_n2:
  retq
