# REQUIRES: x86

## Fan out beyond the parallel frontier threshold. Workers share literal words
## and symbols, claim new unwind entries, and queue sections beyond the DFS limit.
## The live-support chain requires another traversal after the support scan.
# RUN: rm -rf %t; split-file %s %t
# RUN: %python %t/generate.py > %t/input.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/input.s -o %t/input.o
# RUN: %lld -lSystem -dead_strip --threads=1 %t/input.o -o %t/out -map %t/map
# RUN: cp %t/out %t/reference; cp %t/map %t/reference.map
# RUN: %lld -lSystem -dead_strip --threads=2 %t/input.o -o %t/out -map %t/map
# RUN: cmp %t/reference %t/out; cmp %t/reference.map %t/map
# RUN: %lld -lSystem -dead_strip --threads=8 %t/input.o -o %t/out -map %t/map
# RUN: cmp %t/reference %t/out; cmp %t/reference.map %t/map
# RUN: %lld -lSystem -dead_strip --threads=16 %t/input.o -o %t/out -map %t/map
# RUN: cmp %t/reference %t/out; cmp %t/reference.map %t/map
# RUN: %lld -lSystem -dead_strip --threads=1 -why_live '_*' %t/input.o -o %t/out > %t/why1 2>&1
# RUN: %lld -lSystem -dead_strip --threads=8 -why_live '_*' %t/input.o -o %t/out > %t/why8 2>&1
# RUN: cmp %t/why1 %t/why8
# RUN: llvm-nm --defined-only %t/out | FileCheck %s
# RUN: llvm-dwarfdump --eh-frame %t/out | FileCheck %s --check-prefix=UNWIND

# CHECK-NOT: _dead
# CHECK: _chain1999_4
# CHECK: _f0
# CHECK: _f1999
# CHECK: _main
# CHECK: _shared
# CHECK: _support1
# CHECK: _support2
# CHECK: _support_target
# CHECK-NOT: _dead
# UNWIND-COUNT-4000: FDE cie=
# UNWIND-NOT: FDE cie=

#--- generate.py
print('.text\n.globl _main\n_main:')
for i in range(2000):
    print(f'callq _f{i}')
print('retq')
for i in range(2000):
    print(f'.globl _f{i}\n_f{i}:\n.cfi_startproc\n.cfi_escape 0')
    print(f'leaq Lstr{i}(%rip), %rax\nleaq Lword{i}(%rip), %rax')
    print('leaq Lshared(%rip), %rax\ncallq _shared')
    print(f'callq _chain{i}_0\nretq\n.cfi_endproc')
    for depth in range(5):
        print(f'_chain{i}_{depth}:')
        if depth == 4:
            print('.cfi_startproc\n.cfi_escape 0')
        if depth != 4:
            print(f'callq _chain{i}_{depth + 1}')
        print('retq')
        if depth == 4:
            print('.cfi_endproc')
print('_shared:\nretq\n.globl _dead\n_dead:\nretq')
print('_support_target:\nretq')
print('.section __TEXT,__cstring,cstring_literals\nLshared:\n.asciz "shared"')
for i in range(2000):
    print(f'Lstr{i}:\n.asciz "literal{i}"')
print('.section __TEXT,__literal8,8byte_literals\n.p2align 3')
for i in range(2000):
    print(f'Lword{i}:\n.quad {i + 1}')
print('.section __DATA,support,regular,live_support')
print('_support1:\n.quad _f0\n.quad _support_target\n_support2:\n.quad _support1')
print('.subsections_via_symbols')
