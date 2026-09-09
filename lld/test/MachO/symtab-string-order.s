# REQUIRES: aarch64

# RUN: rm -rf %t && split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/external-a.s -o %t/external-a.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/undefined.s -o %t/undefined.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/external-b.s -o %t/external-b.o
# RUN: mkdir -p %t/one %t/many
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/libtest.dylib -no_uuid --threads=1 %t/external-a.o %t/undefined.o %t/external-b.o -o %t/one/libtest.dylib
# RUN: obj2yaml %t/one/libtest.dylib | FileCheck %s
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/libtest.dylib -no_uuid --threads=4 %t/external-a.o %t/undefined.o %t/external-b.o -o %t/many/libtest.dylib
# RUN: cmp %t/one/libtest.dylib %t/many/libtest.dylib

## Local names, followed by global names in their original interleaved
## publication order: defined, undefined, defined.  The duplicate local name
## verifies first-occurrence deduplication across input files.
# CHECK:      StringTable:
# CHECK-NEXT:   - ' '
# CHECK-NEXT:   - private_duplicate
# CHECK-NEXT:   - _external_a
# CHECK-NEXT:   - _undefined
# CHECK-NEXT:   - _external_b

#--- external-a.s
.text
private_duplicate:
  ret
.globl _external_a
_external_a:
  bl private_duplicate
  ret

#--- undefined.s
.data
  .quad _undefined

#--- external-b.s
.text
private_duplicate:
  ret
.globl _external_b
_external_b:
  bl private_duplicate
  ret
