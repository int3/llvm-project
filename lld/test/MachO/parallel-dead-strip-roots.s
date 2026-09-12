# REQUIRES: x86

## Root selection runs on workers when the symbol vector is large enough, and
## literal-piece liveness is written directly by the traversal workers rather
## than merged serially. Exercise both with an -exported_symbols_list glob over
## thousands of globals, each keeping a cstring and an 8-byte literal alive.
## The two links use the same output basename so nothing that embeds the output
## path can mask a real difference.

# RUN: rm -rf %t
# RUN: mkdir -p %t/t1 %t/t8
# RUN: %python %S/Inputs/gen-dead-strip-roots.py > %t/roots.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/roots.s -o %t/roots.o
# RUN: echo "_main" > %t/exports.txt
# RUN: echo "_leafE*" >> %t/exports.txt

# RUN: %lld -lSystem -dead_strip -no_uuid -exported_symbols_list %t/exports.txt \
# RUN:   --threads=1 %t/roots.o -o %t/t1/linked
# RUN: %lld -lSystem -dead_strip -no_uuid -exported_symbols_list %t/exports.txt \
# RUN:   --threads=8 %t/roots.o -o %t/t8/linked
# RUN: cmp %t/t1/linked %t/t8/linked

## Exported roots survive; unexported, unreferenced definitions do not.
# RUN: llvm-nm %t/t8/linked | FileCheck --check-prefix=SYMS %s
# SYMS-DAG: _leafE0
# SYMS-DAG: _leafE1599
# SYMS-NOT: _leafD

## Literals reachable only from the stripped half are stripped with it, so the
## atomic live bits must not have leaked into neighbouring pieces.
# RUN: llvm-objdump --macho --section="__TEXT,__cstring" %t/t8/linked \
# RUN:   | FileCheck --check-prefix=STR %s
# STR-DAG: liveE0
# STR-NOT: deadD
