# REQUIRES: x86
## The export trie is built on a task group spawned right after scanSymbols(),
## while the rest of the link assigns addresses. Check that the trie is
## byte-identical to the serial one when the window between the spawn and the
## join is full of the things that change symbols there: undefined symbols
## recovered into section/segment boundary definitions and into dynamic
## lookups, stub and GOT entries, aliases, and a filtered export list. Multiple
## reexported libraries make the serialized dylib ordinals, which are only
## assigned when the load commands are created, differ from each other. A
## reexported dylib symbol only reaches the trie when an explicit export list
## matches it, so the filtered run below is the one that covers the ordinal.

# RUN: rm -rf %t; mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t/hoist.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %S/Inputs/export-trie-reexport.s -o %t/reexport.o
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libone.dylib %t/reexport.o -o %t/libone.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libtwo.dylib %t/reexport.o -o %t/libtwo.dylib

# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=1 %t/hoist.o -o %t/t1.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=2 %t/hoist.o -o %t/t2.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=8 %t/hoist.o -o %t/t8.dylib
# RUN: cmp %t/t1.dylib %t/t2.dylib
# RUN: cmp %t/t1.dylib %t/t8.dylib
# RUN: llvm-objdump --macho --exports-trie %t/t8.dylib | FileCheck %s \
# RUN:   --implicit-check-not=section\$start\$ --implicit-check-not=segment\$start\$ \
# RUN:   --implicit-check-not=_dynamic_lookup --implicit-check-not=_missing_entirely

## An explicit export list drops most of the definitions and keeps the
## reexports, exercising a different symbol selection on the same window.
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -exported_symbol _hoist_weak -exported_symbol _parallel_reexport \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=1 %t/hoist.o -o %t/filtered1.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -exported_symbol _hoist_weak -exported_symbol _parallel_reexport \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=8 %t/hoist.o -o %t/filtered8.dylib
# RUN: cmp %t/filtered1.dylib %t/filtered8.dylib
# RUN: llvm-objdump --macho --exports-trie %t/filtered8.dylib | FileCheck %s --check-prefix=FILTERED --implicit-check-not=_hoist_target

## A dead-stripped link drops the unreferenced definitions; the trie must still
## match the serial build.
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup -dead_strip \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=1 %t/hoist.o -o %t/strip1.dylib
# RUN: %lld -dylib -arch x86_64 -no_uuid -install_name @rpath/libhoist.dylib \
# RUN:   -U _dynamic_lookup -undefined dynamic_lookup -dead_strip \
# RUN:   -alias _hoist_target _hoist_alias \
# RUN:   -reexport_library %t/libone.dylib -reexport_library %t/libtwo.dylib \
# RUN:   --threads=8 %t/hoist.o -o %t/strip8.dylib
# RUN: cmp %t/strip1.dylib %t/strip8.dylib

# CHECK-DAG: _hoist_target{{$}}
# CHECK-DAG: _hoist_alias{{$}}
# CHECK-DAG: _hoist_weak [weak_def]{{$}}
# CHECK-DAG: _hoist_tlv [per-thread]{{$}}
# CHECK-DAG: _hoist_absolute [absolute]{{$}}
## The boundary and dynamic-lookup symbols are not exported; the implicit
## check-nots above enforce that.

# FILTERED-DAG: _hoist_weak [weak_def]{{$}}
# FILTERED-DAG: [re-export] _parallel_reexport (from libone){{$}}

.text
.globl _hoist_target
_hoist_target:
  retq

.globl _hoist_ref
_hoist_ref:
## A section/segment boundary undefined is recovered into a private-extern
## definition inside the hoist window, replacing the symbol in place.
  movq section$start$__DATA$__hoistsect@GOTPCREL(%rip), %rax
  movq segment$start$__DATA@GOTPCREL(%rip), %rax
## An undefined that -U turns into a dynamic lookup, also inside the window.
  callq _dynamic_lookup
## An undefined with no definition anywhere, recovered by -undefined
## dynamic_lookup during relocation scanning.
  callq _missing_entirely
  retq

.section __DATA,__thread_vars,thread_local_variables
.globl _hoist_tlv
_hoist_tlv:
  .quad 0
  .quad 0
  .quad 0

.globl _hoist_absolute
_hoist_absolute = 0x1234

.text
.globl _hoist_weak
.weak_definition _hoist_weak
_hoist_weak:
  retq

.subsections_via_symbols
