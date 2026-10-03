# REQUIRES: aarch64

## A linker-generated thunk symbol must not take over an input symbol of the
## same name: that would silently redirect every branch to the input symbol.

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t.o
# RUN: not %lld -arch arm64 -lSystem -o %t.out %t.o 2>&1 | FileCheck %s

# CHECK: error: linker-generated thunk name '_foo.thunk.0' collides with _foo.thunk.0 from {{.*}}.o

.subsections_via_symbols

.globl _main, _foo, _foo.thunk.0, _spacer1, _spacer2
.weak_definition _foo.thunk.0
.p2align 2
_main:
  bl _foo
  bl _foo.thunk.0
  ret

.p2align 2
_foo.thunk.0:
  ret

_spacer1:
.space 0x4000000
_spacer2:
.space 0x4000000

.p2align 2
_foo:
  ret
