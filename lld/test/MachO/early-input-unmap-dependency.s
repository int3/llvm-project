# REQUIRES: x86, aarch64, thread_support
## Compare the live-input path (t1) with the snapshot path (t2/8/16), including
## duplicate archive-member names, a thin member, a fat archive, bitcode and its
## generated object, an opaque section, a dylib and unsuccessful library probes.
## The Python driver inherits lit's link-twice setting on successful links.
# RUN: rm -rf %t; split-file %s %t
# RUN: %python %S/Inputs/early-input-unmap-dependency.py %t %no-fatal-warnings-lld

#--- main.s
.globl _main
_main:
  callq _a
  callq _b
  callq _thin
  callq _fat
  callq _bc
  callq _dyn
  ret

#--- a.s
.globl _a
_a: ret
#--- b.s
.globl _b
_b: ret
#--- thin.s
.globl _thin
_thin: ret
#--- fat.s
.globl _fat
_fat: ret
#--- dyn.s
.globl _dyn
_dyn: ret
#--- bc.ll
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.15.0"
define void @bc() { ret void }
#--- blob.txt
opaque section payload
