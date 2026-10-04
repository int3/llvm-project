# REQUIRES: aarch64

## The thunk walk keeps its pending branches in a ring that grows while the
## front waits, prefetches ahead of the front when a blocking front resolves
## and the queue drains in a burst, and resolves the deferred redirects found
## at the scan and at the front as one merged stream. This link queues 6,000
## branches from one section (more than the ring's initial 4,096 entries)
## behind fronts that wait 64 MiB:
## - _a's forward calls to _near become direct in the burst;
## - _a's far calls to _far, _objc_msgSend$foo and _extern each force one thunk
##   and the rest are redirected at the front;
## - _b's calls to _objc_msgSend$foo and _extern find those thunks at the scan
##   and take the stubs shortcut at the end (an Objective-C stub, known from
##   its section, and a __stubs entry);
## - _b's far backward calls to _main (addend zero, not a stub, so the end
##   loop reads its name) and to _a+8 get thunks in the end loop.
## The thunk counts and the output are the walk's before these changes, and
## the same at one and eight threads.

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t.o
# RUN: mkdir -p %t.dir1 %t.dir8
# RUN: %lld -arch arm64 -U _extern --threads=1 -no_uuid --verbose \
# RUN:   -o %t.dir1/linked-output %t.o 2>&1 | FileCheck %s --check-prefix=LOG
# RUN: %lld -arch arm64 -U _extern --threads=8 -no_uuid \
# RUN:   -o %t.dir8/linked-output %t.o
# RUN: cmp %t.dir1/linked-output %t.dir8/linked-output
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn \
# RUN:   --disassemble-symbols=_a,_b %t.dir1/linked-output | FileCheck %s

# LOG: __text: Created 5 (0 KB) thunks and updated 6500 branch targets

# CHECK-LABEL: <_a>:
# CHECK-NEXT:    bl {{.*}} <_near>
# CHECK-NEXT:    bl {{.*}} <_far.thunk.0>
# CHECK-NEXT:    bl {{.*}} <_objc_msgSend$foo.thunk.0>
# CHECK-NEXT:    bl {{.*}} <_extern.thunk.0>
# CHECK-NEXT:    bl {{.*}} <_near>
# CHECK-NEXT:    bl {{.*}} <_far.thunk.0>
# CHECK-NEXT:    bl {{.*}} <_objc_msgSend$foo.thunk.0>
# CHECK-NEXT:    bl {{.*}} <_extern.thunk.0>

# CHECK-LABEL: <_b>:
# CHECK-NEXT:    bl {{.*}} <_far>
# CHECK-NEXT:    bl {{.*}} <_objc_msgSend$foo>
# CHECK-NEXT:    bl {{.*}} <_a+8.thunk.0>
# CHECK-NEXT:    bl {{[^.]*$}}
# CHECK-NEXT:    bl {{.*}} <_main.thunk.0>
# CHECK-NEXT:    bl {{.*}} <_far>

.section  __TEXT,__objc_methname,cstring_literals
lselref1:
  .asciz  "foo"

.section  __DATA,__objc_selrefs,literal_pointers,no_dead_strip
.p2align  3
.quad lselref1

.text
.globl _objc_msgSend
_objc_msgSend:
  ret

.globl _main
_main:
  ret

.globl _a
_a:
.rept 1500
  bl _near
  bl _far
  bl _objc_msgSend$foo
  bl _extern
.endr
  ret

.globl _spacer1
_spacer1:
.space 0x4000000

.globl _near
_near:
  ret

.globl _spacer2
_spacer2:
.space 0x4000000

.globl _far
_far:
  ret

.globl _b
_b:
.rept 1000
  bl _far
  bl _objc_msgSend$foo
  bl _a+8
  bl _extern
  bl _main
.endr
  ret

.subsections_via_symbols
