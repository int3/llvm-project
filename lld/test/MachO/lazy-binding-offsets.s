# REQUIRES: x86

## Keep the lazy-binding entries in the same order as the stub entries even
## when interposable Defined symbols are mixed with DylibSymbols. The second
## call to _interposable_local must not create another entry.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/lib.s -o %t/lib.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/defined.s -o %t/defined.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: %lld -dylib -install_name @executable_path/libmixed.dylib %t/lib.o -o %t/libmixed.dylib
# RUN: %lld -no_fixup_chains -interposable -lSystem -L%t -lmixed -o %t/main %t/main.o %t/defined.o
# RUN: llvm-objdump --macho --no-print-imm-hex --rebase --lazy-bind %t/main > %t/objdump
# RUN: llvm-objdump --no-print-imm-hex -D --no-show-raw-insn %t/main >> %t/objdump
# RUN: FileCheck %s < %t/objdump

# CHECK-LABEL: Rebase table:
# CHECK-NEXT:  segment  section            address     type
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} pointer
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} pointer
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} pointer
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} pointer
# CHECK-EMPTY:
# CHECK-LABEL: Lazy bind table:
# CHECK-NEXT:  segment  section            address     dylib            symbol
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} flat-namespace   _main
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} flat-namespace   _interposable_local
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} libmixed         _dylib_long_name
# CHECK-NEXT:  __DATA   __la_symbol_ptr    {{.*}} libmixed         _dylib_a
# CHECK-EMPTY:

# CHECK-LABEL: Disassembly of section __TEXT,__stub_helper:
# CHECK:       pushq $0
# CHECK-NEXT:  jmp
# CHECK-NEXT:  pushq $12
# CHECK-NEXT:  jmp
# CHECK-NEXT:  pushq $38
# CHECK-NEXT:  jmp
# CHECK-NEXT:  pushq $61
# CHECK-NEXT:  jmp
# CHECK-NOT:   pushq

#--- lib.s
.globl _dylib_a, _dylib_long_name
_dylib_a:
  retq
_dylib_long_name:
  retq

#--- defined.s
.globl _interposable_local
_interposable_local:
  retq

#--- main.s
.globl _main
.text
_main:
  callq _dylib_a
  callq _interposable_local
  callq _dylib_long_name
  callq _interposable_local
  retq
