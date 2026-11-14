# REQUIRES: aarch64

## The symbol table, function starts and export trie are partly computed while
## addresses are assigned. Check that the output doesn't depend on the thread
## count, including for the branch-range extension thunks created meanwhile.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/foo.s -o %t/foo.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/main.s -o %t/main.o
# RUN: %lld -arch arm64 -dylib -o %t/libfoo.dylib %t/foo.o

# RUN: %lld -arch arm64 -lSystem -o %t/out %t/main.o %t/libfoo.dylib --threads=1
# RUN: llvm-nm -m %t/out | FileCheck %s --check-prefix=SYMS
# RUN: llvm-objdump --macho --exports-trie %t/out | \
# RUN:   FileCheck %s --check-prefix=EXPORTS

# SYMS-DAG: (__TEXT,__text) non-external _local_fn
# SYMS-DAG: (__TEXT,__text) non-external (was a private external) _hidden_fn
# SYMS-DAG: (__TEXT,__text) non-external (was a private external) _extern_fn.thunk.0
# SYMS-DAG: (__TEXT,__text) external _extern_fn
# SYMS-DAG: (__TEXT,__text) weak external _weak_fn
# SYMS-DAG: (undefined) external _foo (from libfoo)

# EXPORTS-LABEL: Exports trie:
# EXPORTS-DAG:   0x{{[0-9A-F]+}} __mh_execute_header
# EXPORTS-DAG:   0x{{[0-9A-F]+}} _extern_fn
# EXPORTS-DAG:   0x{{[0-9A-F]+}} _main
# EXPORTS-DAG:   0x{{[0-9A-F]+}} _weak_fn [weak_def]

# RUN: %lld -arch arm64 -lSystem -o %t/out %t/main.o %t/libfoo.dylib \
# RUN:   -exported_symbol _extern_fn -exported_symbol _foo --threads=1
# RUN: llvm-objdump --macho --exports-trie %t/out | \
# RUN:   FileCheck %s --check-prefix=REEXPORT
# REEXPORT:      Exports trie:
# REEXPORT-NEXT: 0x{{[0-9A-F]+}} _extern_fn
# REEXPORT-NEXT: [re-export] _foo (from libfoo)
# REEXPORT-EMPTY:

## The code signature covers the output's file name, so use the same name.
# RUN: mkdir %t/1 %t/2 %t/n
# RUN: %lld -arch arm64 -lSystem -o %t/1/out %t/main.o %t/libfoo.dylib --threads=1
# RUN: %lld -arch arm64 -lSystem -o %t/2/out %t/main.o %t/libfoo.dylib --threads=2
# RUN: %lld -arch arm64 -lSystem -o %t/n/out %t/main.o %t/libfoo.dylib
# RUN: cmp %t/1/out %t/2/out
# RUN: cmp %t/1/out %t/n/out

# RUN: %lld -arch arm64 -lSystem -o %t/1/out %t/main.o %t/libfoo.dylib -x --threads=1
# RUN: %lld -arch arm64 -lSystem -o %t/2/out %t/main.o %t/libfoo.dylib -x --threads=2
# RUN: %lld -arch arm64 -lSystem -o %t/n/out %t/main.o %t/libfoo.dylib -x
# RUN: cmp %t/1/out %t/2/out
# RUN: cmp %t/1/out %t/n/out

# RUN: %lld -arch arm64 -lSystem -o %t/1/out %t/main.o %t/libfoo.dylib -dead_strip --threads=1
# RUN: %lld -arch arm64 -lSystem -o %t/2/out %t/main.o %t/libfoo.dylib -dead_strip --threads=2
# RUN: %lld -arch arm64 -lSystem -o %t/n/out %t/main.o %t/libfoo.dylib -dead_strip
# RUN: cmp %t/1/out %t/2/out
# RUN: cmp %t/1/out %t/n/out

# RUN: %lld -arch arm64 -lSystem -o %t/1/out %t/main.o %t/libfoo.dylib \
# RUN:   -exported_symbol _extern_fn -exported_symbol _foo --threads=1
# RUN: %lld -arch arm64 -lSystem -o %t/2/out %t/main.o %t/libfoo.dylib \
# RUN:   -exported_symbol _extern_fn -exported_symbol _foo --threads=2
# RUN: %lld -arch arm64 -lSystem -o %t/n/out %t/main.o %t/libfoo.dylib \
# RUN:   -exported_symbol _extern_fn -exported_symbol _foo
# RUN: cmp %t/1/out %t/2/out
# RUN: cmp %t/1/out %t/n/out

#--- foo.s
.globl _foo
_foo:
  ret

#--- main.s
.globl _main, _extern_fn, _weak_fn
.private_extern _hidden_fn
.weak_definition _weak_fn

.text
_main:
  bl _extern_fn
  bl _local_fn
  bl _hidden_fn
  bl _foo
  ret

## More than the 128 MiB branch range, so the calls above need thunks. The
## padding is two atoms, so a thunk can go between them.
.no_dead_strip _pad1, _pad2
_pad1:
  .space 0x4000000
_pad2:
  .space 0x4000000

_extern_fn:
  ret
_local_fn:
  ret
_hidden_fn:
  ret
_weak_fn:
  ret

.subsections_via_symbols
