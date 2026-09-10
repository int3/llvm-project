# REQUIRES: x86

## Check that the read-only classification used by Writer::scanSymbols keeps
## both its symbol-vector and per-object symbol order when replayed.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %s -o %t.o
# RUN: %lld -arch x86_64 -lSystem -no_uuid --threads=1 %t.o -o %t.one
# RUN: %lld -arch x86_64 -lSystem -no_uuid --threads=8 %t.o -o %t.eight
# RUN: cmp %t.one %t.eight
# RUN: llvm-objdump --macho --unwind-info --syms %t.eight | FileCheck %s

# CHECK:      SYMBOL TABLE:
# CHECK-DAG: {{.*}} l     F __TEXT,__text _local_alias
# CHECK-DAG: {{.*}} g     F __TEXT,__text _global_alias
# CHECK-DAG: {{.*}} _objc_msgSend$first
# CHECK-DAG: {{.*}} _objc_msgSend$second
# CHECK:      Contents of __unwind_info section:
# CHECK:      Second level indices:
# CHECK-DAG: function offset={{.*}}

.text
.macro parallel_global
.globl _parallel_global\@
_parallel_global\@:
  retq
.endm

## More than one 4096-symbol classification chunk makes the eight-thread run
## exercise the parallel global selection path without a checked-in listing.
.rept 5000
parallel_global
.endr

.globl _objc_msgSend
_objc_msgSend:
  retq

.globl _main
_main:
.cfi_startproc
  callq _objc_msgSend$first
  callq _objc_msgSend$second
  retq
.cfi_endproc

## The global alias is visited during the symVector replay. Its local alias
## is replayed only after that, in ObjFile::symbols order.
.globl _global_alias
_local_alias:
.cfi_startproc
  retq
.cfi_endproc
_global_alias = _local_alias

## A nonzero alternate entry exercises UnwindInfoSection's fallback index.
.globl _nonzero_base, _nonzero_entry
_nonzero_base:
  nop
.alt_entry _nonzero_entry
_nonzero_entry:
.cfi_startproc
  retq
.cfi_endproc

.subsections_via_symbols
