# REQUIRES: x86
# RUN: rm -rf %t && split-file %s %t
# RUN: %python %t/generate.py compressed > %t/compressed.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin \
# RUN:   -emit-compact-unwind-non-canonical=true %t/compressed.s -o %t/compressed.o
# RUN: %lld -dylib -arch x86_64 -lSystem -lc++ -no_uuid -install_name compressed \
# RUN:   --threads=1 %t/compressed.o -o %t/compressed-t1
# RUN: %lld -dylib -arch x86_64 -lSystem -lc++ -no_uuid -install_name compressed \
# RUN:   --threads=8 %t/compressed.o -o %t/compressed-t8
# RUN: cmp %t/compressed-t1 %t/compressed-t8
# RUN: llvm-objdump --macho --unwind-info %t/compressed-t1 | FileCheck %s --check-prefix=COMPRESSED
# RUN: %python %t/generate.py regular > %t/regular.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin \
# RUN:   -emit-compact-unwind-non-canonical=true %t/regular.s -o %t/regular.o
# RUN: %lld -dylib -arch x86_64 -lSystem -lc++ -no_uuid -install_name regular \
# RUN:   --threads=1 %t/regular.o -o %t/regular-t1
# RUN: %lld -dylib -arch x86_64 -lSystem -lc++ -no_uuid -install_name regular \
# RUN:   --threads=8 %t/regular.o -o %t/regular-t8
# RUN: cmp %t/regular-t1 %t/regular-t8
# RUN: llvm-objdump --macho --unwind-info %t/regular-t1 | FileCheck %s --check-prefix=REGULAR

## The first-level LSDA offsets are prefixes of the LSDA descriptors owned by
## their page. Exercise compressed pages, a trailing page without an LSDA, and
## the regular-page fallback. The records are manual so page formation does
## not depend on assembler compact-unwind lowering. Each case has 9,000
## surviving function slots, exercising multiple parallel prepare chunks.

# COMPRESSED: Top level indices: (count = 10)
# COMPRESSED-NEXT: [0]: {{.*}} LSDA offset=0x[[#%.8x,C0:]]
# COMPRESSED-NEXT: [1]: {{.*}} LSDA offset=0x[[#%.8x,C0 + 16]]
# COMPRESSED-NEXT: [2]: {{.*}} LSDA offset=0x[[#%.8x,C0 + 32]]
# COMPRESSED-NEXT: [3]: {{.*}} LSDA offset=0x[[#%.8x,C0 + 32]]
# COMPRESSED: LSDA descriptors:
# COMPRESSED-NEXT: [0]:
# COMPRESSED-NEXT: [1]:
# COMPRESSED-NEXT: [2]:
# COMPRESSED-NEXT: [3]:
# COMPRESSED: Second level index[0]:
# COMPRESSED: [1020]: {{.*}} encoding[{{.*}}]=
# COMPRESSED: Second level index[1]:
# COMPRESSED: [1020]: {{.*}} encoding[{{.*}}]=
# COMPRESSED: Second level index[2]:
# COMPRESSED: [1]: {{.*}} encoding[{{.*}}]=

# REGULAR: Top level indices: (count = 19)
# REGULAR-NEXT: [0]: {{.*}} LSDA offset=0x[[#%.8x,R0:]]
# REGULAR-NEXT: [1]: {{.*}} LSDA offset=0x[[#%.8x,R0 + 16]]
# REGULAR-NEXT: [2]: {{.*}} LSDA offset=0x[[#%.8x,R0 + 32]]
# REGULAR: LSDA descriptors:
# REGULAR-NEXT: [0]:
# REGULAR-NEXT: [1]:
# REGULAR-NEXT: [2]:
# REGULAR-NEXT: [3]:
# REGULAR: Second level index[0]:
# REGULAR: [510]: {{.*}} encoding=
# REGULAR: Second level index[1]:
# REGULAR: [510]: {{.*}} encoding=

#--- generate.py
import sys


def emit_function(name):
    print(".p2align 2")
    print(".globl _%s" % name)
    print("_%s:" % name)
    print("  retq")


def emit_compact_unwind(name, encoding, lsda):
    print(".section __LD,__compact_unwind,regular,debug")
    print("  .quad _%s" % name)
    print("  .long 1")
    print("  .long 0x%08x" % encoding)
    print("  .quad 0")
    print("  .quad .Lexception%d" % lsda if lsda is not None else "  .quad 0")
    print(".text")


def emit_lsdas(indices):
    print(".section __TEXT,__gcc_except_tab")
    for i in indices:
        print(".Lexception%d:" % i)
        print("  .byte 0")


if sys.argv[1] == "compressed":
    lsda = {0, 1020, 1021, 1022}
    for i in range(9000):
        emit_function("c%d" % i)
        emit_compact_unwind("c%d" % i, 0x03000000, i if i in lsda else None)
    emit_lsdas(sorted(lsda))
elif sys.argv[1] == "regular":
    lsda = {0, 510, 511, 512}
    for i in range(9000):
        emit_function("r%d" % i)
        emit_compact_unwind("r%d" % i, 0x01000000 | i,
                            i if i in lsda else None)
    emit_lsdas(sorted(lsda))
else:
    raise ValueError("expected compressed or regular")
