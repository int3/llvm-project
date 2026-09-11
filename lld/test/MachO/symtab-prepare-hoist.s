# REQUIRES: aarch64

## The symbol-table and function-starts prefixes are prepared concurrently with
## address assignment, which is also where branch-range extension thunks add new
## symbols. Check that the symbol table, string table and function starts are
## identical however many threads are used, for every symbol-presence mode, and
## that an undefined-symbol error, which is reported before the preparation
## tasks are spawned, is still diagnosed normally.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/test.s -o %t/test.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/missing.s -o %t/missing.o
# RUN: echo "_local_kept" > %t/keep.txt
## The ad-hoc code signature covers the output basename, so each comparison
## links to the same basename in a different directory.
# RUN: mkdir -p %t/t1 %t/t2 %t/t8 %t/x1 %t/x8 %t/keep1 %t/keep8 %t/strip1 %t/strip8 %t/nd1 %t/nd8 %t/ds1 %t/ds8

# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid --threads=1 %t/test.o -o %t/t1/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid --threads=2 %t/test.o -o %t/t2/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid --threads=8 %t/test.o -o %t/t8/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: llvm-nm %t/t8/out | FileCheck %s --check-prefix=SYMS

## -x drops the object-file local walk entirely.
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -x --threads=1 %t/test.o -o %t/x1/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -x --threads=8 %t/test.o -o %t/x8/out
# RUN: cmp %t/x1/out %t/x8/out

## Selectively included and excluded local symbols.
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -non_global_symbols_no_strip_list %t/keep.txt --threads=1 %t/test.o -o %t/keep1/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -non_global_symbols_no_strip_list %t/keep.txt --threads=8 %t/test.o -o %t/keep8/out
# RUN: cmp %t/keep1/out %t/keep8/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -non_global_symbols_strip_list %t/keep.txt --threads=1 %t/test.o -o %t/strip1/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -non_global_symbols_strip_list %t/keep.txt --threads=8 %t/test.o -o %t/strip8/out
# RUN: cmp %t/strip1/out %t/strip8/out

## Serial string interning, i.e. no deduplication pass to split.
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -no-deduplicate-symbol-strings --threads=1 %t/test.o -o %t/nd1/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -no-deduplicate-symbol-strings --threads=8 %t/test.o -o %t/nd8/out
# RUN: cmp %t/nd1/out %t/nd8/out

# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -dead_strip --threads=1 %t/test.o -o %t/ds1/out
# RUN: %lld -arch arm64 -lSystem -U _stub_target -no_uuid -dead_strip --threads=8 %t/test.o -o %t/ds8/out
# RUN: cmp %t/ds1/out %t/ds8/out

## An undefined symbol is reported before the preparation tasks are spawned; a
## thunk-range overrun would be reported after them. Check the ordinary early
## error path still exits cleanly at high thread counts.
# RUN: not %lld -arch arm64 -lSystem -U _stub_target -no_uuid --threads=8 %t/missing.o -o %t/err 2>&1 | FileCheck %s --check-prefix=ERR

## The thunk symbols are private extern, so they are emitted as local symbols
## even though they are created after the local walk has been prepared.
# SYMS-DAG: t _late_global.thunk.0
# SYMS-DAG: t _stub_target.thunk.0
# SYMS-DAG: t _local_kept
# SYMS-DAG: t _private
# SYMS-DAG: T _main
# SYMS-DAG: U _stub_target

# ERR: error: undefined symbol: _nowhere

#--- test.s
.subsections_via_symbols

.globl _main
.p2align 2
_main:
  bl _late_global
  bl _late_local
  bl _stub_target
  ret

.p2align 2
_local_kept:
  ret

.private_extern _private
.globl _private
.p2align 2
_private:
  ret

## 0x8000000 is 128 MiB, one more than the forward branch limit. Distribute it
## over two functions since thunk insertion cannot handle a single 128 MiB one.
.globl _spacer1, _spacer2
_spacer1:
.space 0x4000000
_spacer2:
.space 0x4000000

.globl _late_global
.p2align 2
_late_global:
  ret

.p2align 2
_late_local:
  ret

#--- missing.s
.globl _main
_main:
  bl _nowhere
  ret
