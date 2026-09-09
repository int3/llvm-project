# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %s -o %t.o
# RUN: %lld -arch x86_64 -lSystem %t.o -o %t
# RUN: llvm-objdump --macho --unwind-info --syms %t | FileCheck %s -D#%x,BASE=0x100000000

## The local alias owns the compact-unwind entry while its global alias is
## visited first by Writer::scanSymbols(). The later local scan must replace
## the entry without changing its insertion order.
# CHECK:      SYMBOL TABLE:
# CHECK-DAG:  [[#%x,MAIN:]]       l     F __TEXT,__text _local_alias
# CHECK-DAG:  [[#%x,MAIN]]       g     F __TEXT,__text _main
# CHECK-DAG:  [[#%x,NONZERO_BASE:]] g     F __TEXT,__text _nonzero_base
# CHECK-DAG:  [[#%x,NONZERO_ENTRY:]] g     F __TEXT,__text _nonzero_entry
# CHECK:      Contents of __unwind_info section:
# CHECK:      Second level indices:
# CHECK-DAG:    function offset=0x[[#%.8x,MAIN-BASE]], encoding[{{[0-9]+}}]=0x04000018
# CHECK-DAG:    function offset=0x[[#%.8x,NONZERO_BASE-BASE]], encoding[{{[0-9]+}}]=0x00000000
# CHECK-DAG:    function offset=0x[[#%.8x,NONZERO_ENTRY-BASE]], encoding[{{[0-9]+}}]=0x04000038

.text
.globl _main
_local_alias:
.cfi_startproc
  retq
.cfi_endproc
_main = _local_alias

## An alternative entry remains in the same ConcatInputSection at a nonzero
## offset, and therefore exercises the fallback key-to-index map.
.globl _nonzero_base, _nonzero_entry
_nonzero_base:
  nop
.alt_entry _nonzero_entry
_nonzero_entry:
.cfi_startproc
  retq
.cfi_endproc

.subsections_via_symbols
