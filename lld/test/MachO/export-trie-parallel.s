# REQUIRES: x86
# RUN: rm -rf %t; mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t/exports.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %S/Inputs/export-trie-reexport.s -o %t/reexport.o
# RUN: %lld -dylib -arch x86_64 -no_uuid %t/reexport.o -o %t/libreexported.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libparallel.dylib --threads=1 -exported_symbol _parallel_* -reexport_library %t/libreexported.dylib %t/exports.o -o %t/threads1.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libparallel.dylib --threads=2 -exported_symbol _parallel_* -reexport_library %t/libreexported.dylib %t/exports.o -o %t/threads2.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libparallel.dylib --threads=4 -exported_symbol _parallel_* -reexport_library %t/libreexported.dylib %t/exports.o -o %t/threads4.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libparallel.dylib --threads=8 -exported_symbol _parallel_* -reexport_library %t/libreexported.dylib %t/exports.o -o %t/threads8.dylib
# RUN: cmp %t/threads1.dylib %t/threads2.dylib
# RUN: cmp %t/threads1.dylib %t/threads4.dylib
# RUN: cmp %t/threads1.dylib %t/threads8.dylib
# RUN: llvm-objdump --macho --exports-trie %t/threads8.dylib | FileCheck %s

# The generated names diverge after a long common prefix. The count also makes
# child offsets cross the two-byte ULEB128 boundary. Define the exceptional
# names out of lexical order to keep the serial-order splice observable.
# CHECK-DAG: _parallel_branch_0{{$}}
# CHECK-DAG: _parallel_branch_4095{{$}}
# CHECK-DAG: _parallel_prefix{{$}}
# CHECK-DAG: _parallel_prefix_child{{$}}
# CHECK-DAG: _parallel_weak [weak_def]{{$}}
# CHECK-DAG: _parallel_tlv [per-thread]{{$}}
# CHECK-DAG: _parallel_absolute [absolute]{{$}}
# CHECK-DAG: [re-export] _parallel_reexport (from libreexported){{$}}

.macro parallel_export
.globl _parallel_branch_\@
_parallel_branch_\@:
  retq
.endm

.text
.globl _parallel_zebra
_parallel_zebra:
  retq

.globl _parallel_prefix_child
_parallel_prefix_child:
  retq

.globl _parallel_alpha
_parallel_alpha:
  retq

.globl _parallel_prefix
_parallel_prefix:
  retq

.globl _parallel_weak
.weak_definition _parallel_weak
_parallel_weak:
  retq

.rept 5000
parallel_export
.endr

.globl _parallel_absolute
_parallel_absolute = 0xdead

.section __DATA,__thread_vars,thread_local_variables
.globl _parallel_tlv
_parallel_tlv:
  .quad 0
