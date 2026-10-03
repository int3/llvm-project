# REQUIRES: aarch64

## The thunk walk keeps a waiting front branch until it is forced or its
## target section becomes final, and re-evaluates it only at the first input
## after its target (its wake index). These links pin the edges of that rule:
##
## (A) The front is forced in the same iteration in which its target would
##     have become final: a thunk is created although the target's start is
##     within range of the call.
## (B) One iteration later the front is not forced (exactly at the slop
##     boundary), wakes once its target is final, and becomes a direct call.
## (C) The front wakes with its target final but out of range (the addend
##     points past the target), keeps waiting through one more input, and
##     is forced at the next one.
##
## Layout: a 72-byte caller with the branch at offset P1, a 64 MiB spacer, a
## second spacer, _t (64 bytes), _spacer3 (16 bytes), _spacer4 (4 KiB).
## _t starts 0x7fff3dc bytes after the caller, so the branch's forward limit
## (P1 + 0x7fffffc) less the slop (256 * 12 bytes) and _t's end decides (A)
## P1 = 0, (B) P1 = 32, (C) P1 = 64 with addend 0x2000.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/call-a.s -o %t/call-a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/call-b.s -o %t/call-b.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/call-c.s -o %t/call-c.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/spacer.s -o %t/spacer.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/tail.s -o %t/tail.o

# RUN: %lld -arch arm64 -lSystem -map %t/a.map -o %t/a %t/call-a.o %t/spacer.o %t/tail.o
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn --disassemble-symbols=_c %t/a \
# RUN:   | FileCheck %s --check-prefix=A
# RUN: FileCheck %s --input-file %t/a.map --check-prefix=A-MAP

# RUN: %lld -arch arm64 -lSystem -map %t/b.map -o %t/b %t/call-b.o %t/spacer.o %t/tail.o
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn --disassemble-symbols=_c %t/b \
# RUN:   | FileCheck %s --check-prefix=B
# RUN: FileCheck %s --input-file %t/b.map --check-prefix=B-MAP --implicit-check-not=.thunk.

# RUN: %lld -arch arm64 -lSystem -map %t/c.map -o %t/c %t/call-c.o %t/spacer.o %t/tail.o
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn --disassemble-symbols=_c %t/c \
# RUN:   | FileCheck %s --check-prefix=C
# RUN: FileCheck %s --input-file %t/c.map --check-prefix=C-MAP

# A:     <_c>:
# A-NEXT: bl 0x{{[[:xdigit:]]+}} <_t.thunk.0>

# A-MAP:      {{\] _c$}}
# A-MAP-NEXT: {{\] _spacer1$}}
# A-MAP-NEXT: {{\] _spacer2$}}
# A-MAP-NEXT: {{\] _t\.thunk\.0$}}
# A-MAP-NEXT: {{\] _t$}}
# A-MAP-NEXT: {{\] _spacer3$}}
# A-MAP-NEXT: {{\] _spacer4$}}
# A-MAP-NEXT: {{\] _main$}}

# B:      <_c>:
# B:      bl 0x{{[[:xdigit:]]+}} <_t>

# B-MAP:      {{\] _c$}}
# B-MAP-NEXT: {{\] _spacer1$}}
# B-MAP-NEXT: {{\] _spacer2$}}
# B-MAP-NEXT: {{\] _t$}}
# B-MAP-NEXT: {{\] _spacer3$}}
# B-MAP-NEXT: {{\] _spacer4$}}
# B-MAP-NEXT: {{\] _main$}}

# C:      <_c>:
# C:      bl 0x{{[[:xdigit:]]+}} <_t+8192.thunk.0>

# C-MAP:      {{\] _c$}}
# C-MAP-NEXT: {{\] _spacer1$}}
# C-MAP-NEXT: {{\] _spacer2$}}
# C-MAP-NEXT: {{\] _t$}}
# C-MAP-NEXT: {{\] _spacer3$}}
# C-MAP-NEXT: {{\] _t\+8192\.thunk\.0$}}
# C-MAP-NEXT: {{\] _spacer4$}}
# C-MAP-NEXT: {{\] _main$}}

#--- call-a.s
.subsections_via_symbols
.globl _c
.p2align 2
_c:
  bl _t
  .space 64
  ret

#--- call-b.s
.subsections_via_symbols
.globl _c
.p2align 2
_c:
  .space 32
  bl _t
  .space 32
  ret

#--- call-c.s
.subsections_via_symbols
.globl _c
.p2align 2
_c:
  .space 64
  bl _t+0x2000
  ret

#--- spacer.s
.subsections_via_symbols
.globl _spacer1
.p2align 2
_spacer1:
  .space 0x4000000

#--- tail.s
.subsections_via_symbols
.globl _spacer2, _t, _spacer3, _spacer4, _main
.p2align 2
_spacer2:
  .space 0x3fff394
.p2align 2
_t:
  .space 60
  ret
.p2align 2
_spacer3:
  .space 16
.p2align 2
_spacer4:
  .space 0x1000
.p2align 2
_main:
  ret
