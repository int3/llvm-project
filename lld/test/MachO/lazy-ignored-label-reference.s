# REQUIRES: x86
## An archive member (or a --start-lib object) whose __cfstring section has an
## N_EXT label. ld64 ignores labels in that section, so the parse makes _cf a
## local, but the archive index and the lazy object's symbol table still list
## it. Once the member is loaded for _f, a later reference to _cf finds the lazy
## symbol, whose fetch is a no-op. The name must then be undefined, as it is to
## ld64; leaving the lazy symbol in the slot crashed the writer in
## addNonLazyBindingEntries(). The result must not depend on the thread count.

# RUN: rm -rf %t; split-file %s %t; mkdir %t/t1 %t/t2 %t/t8
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/mainuser.s -o %t/mainuser.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/m.s -o %t/m.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/u.s -o %t/u.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/refcf.s -o %t/refcf.o
# RUN: llvm-ar --format=darwin rcs %t/lib.a %t/m.o
# RUN: llvm-ar --format=darwin rcs %t/user.a %t/u.o

## A member of a later archive references _cf.
# RUN: %lld -o %t/t1/arch --threads=1 -undefined dynamic_lookup %t/mainuser.o %t/lib.a %t/user.a
# RUN: %lld -o %t/t8/arch --threads=8 -undefined dynamic_lookup %t/mainuser.o %t/lib.a %t/user.a
# RUN: llvm-objdump --macho --bind --syms %t/t1/arch | FileCheck %s --check-prefix=BIND
# RUN: llvm-objdump --macho --bind --syms %t/t8/arch | FileCheck %s --check-prefix=BIND
# RUN: not %lld -o /dev/null --threads=1 %t/mainuser.o %t/lib.a %t/user.a 2>&1 | FileCheck %s --check-prefix=ARCH
# RUN: not %lld -o /dev/null --threads=8 %t/mainuser.o %t/lib.a %t/user.a 2>&1 | FileCheck %s --check-prefix=ARCH

## The same, with the member force-loaded.
# RUN: %lld -o %t/t1/force --threads=1 -undefined dynamic_lookup %t/mainuser.o -force_load %t/lib.a %t/user.a
# RUN: %lld -o %t/t8/force --threads=8 -undefined dynamic_lookup %t/mainuser.o -force_load %t/lib.a %t/user.a
# RUN: llvm-objdump --macho --bind --syms %t/t1/force | FileCheck %s --check-prefix=BIND
# RUN: llvm-objdump --macho --bind --syms %t/t8/force | FileCheck %s --check-prefix=BIND

## An object file after the archive references _cf.
# RUN: %lld -o %t/t1/obj --threads=1 -undefined dynamic_lookup %t/main.o %t/lib.a %t/refcf.o
## Ignored external labels decline before the walker mutates the table. The
## later object must find exactly the lazy state serial publication left.
# RUN: %lld -o %t/t8/obj --threads=8 -undefined dynamic_lookup %t/main.o %t/lib.a %t/refcf.o --time-trace=%t/residual.json --time-trace-granularity=0
# RUN: FileCheck %s --check-prefix=RESIDUAL --input-file=%t/residual.json
# RESIDUAL-NOT: Publish residual archive indexes
# RESIDUAL: "Publish archive indexes"
# RESIDUAL-NOT: Publish residual archive indexes
# RUN: llvm-objdump --macho --bind --syms %t/t1/obj | FileCheck %s --check-prefix=BIND
# RUN: llvm-objdump --macho --bind --syms %t/t8/obj | FileCheck %s --check-prefix=BIND
# RUN: not %lld -o /dev/null --threads=1 %t/main.o %t/lib.a %t/refcf.o 2>&1 | FileCheck %s --check-prefix=OBJ
# RUN: not %lld -o /dev/null --threads=8 %t/main.o %t/lib.a %t/refcf.o 2>&1 | FileCheck %s --check-prefix=OBJ

## A lazy object (--start-lib) instead of an archive member.
# RUN: %lld -o %t/t1/lazy --threads=1 -undefined dynamic_lookup %t/mainuser.o --start-lib %t/m.o --end-lib %t/u.o
# RUN: %lld -o %t/t8/lazy --threads=8 -undefined dynamic_lookup %t/mainuser.o --start-lib %t/m.o --end-lib %t/u.o
# RUN: llvm-objdump --macho --bind --syms %t/t1/lazy | FileCheck %s --check-prefix=BIND
# RUN: llvm-objdump --macho --bind --syms %t/t8/lazy | FileCheck %s --check-prefix=BIND
# RUN: not %lld -o /dev/null --threads=1 %t/mainuser.o --start-lib %t/m.o --end-lib %t/u.o 2>&1 | FileCheck %s --check-prefix=LAZY
# RUN: not %lld -o /dev/null --threads=8 %t/mainuser.o --start-lib %t/m.o --end-lib %t/u.o 2>&1 | FileCheck %s --check-prefix=LAZY

## The output does not depend on the thread count.
# RUN: cmp %t/t1/arch %t/t8/arch
# RUN: cmp %t/t1/force %t/t8/force
# RUN: cmp %t/t1/obj %t/t8/obj
# RUN: cmp %t/t1/lazy %t/t8/lazy

## Recursive extraction: the outer _cf reference fetches nested-m, which
## references _g and fetches nested-u. Its weak _cf reference converts the
## still-lazy slot to Undefined before the outer strong reference returns.
## The outer reference must still make the import strong, as in ld64.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/chain-main.s -o %t/chain-main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/chain-ref.s -o %t/chain-ref.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/chain-ref.s -defsym WEAK=1 -o %t/chain-weak.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/nested-m.s -o %t/nested-m.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/nested-u.s -o %t/nested-u.o
# RUN: llvm-ar --format=darwin rcs %t/nested-lib.a %t/nested-m.o
# RUN: llvm-ar --format=darwin rcs %t/nested-user.a %t/nested-u.o
# RUN: llvm-ar --format=darwin rcs %t/chain-ref.a %t/chain-ref.o
# RUN: llvm-ar --format=darwin rcs %t/chain-weak.a %t/chain-weak.o

# RUN: %lld -o %t/t1/chain --threads=1 -undefined dynamic_lookup %t/chain-main.o %t/nested-user.a %t/nested-lib.a %t/chain-ref.a
# RUN: llvm-objdump --macho --bind --syms %t/t1/chain | FileCheck %s --check-prefix=STRONG
# RUN: %lld -o %t/t2/chain --threads=2 -undefined dynamic_lookup %t/chain-main.o %t/nested-user.a %t/nested-lib.a %t/chain-ref.a
# RUN: llvm-objdump --macho --bind --syms %t/t2/chain | FileCheck %s --check-prefix=STRONG
# RUN: %lld -o %t/t8/chain --threads=8 -undefined dynamic_lookup %t/chain-main.o %t/nested-user.a %t/nested-lib.a %t/chain-ref.a
# RUN: llvm-objdump --macho --bind --syms %t/t8/chain | FileCheck %s --check-prefix=STRONG
# RUN: cmp %t/t1/chain %t/t2/chain
# RUN: cmp %t/t1/chain %t/t8/chain

# RUN: %lld -o %t/t1/chain-lazy --threads=1 -undefined dynamic_lookup %t/chain-main.o --start-lib %t/nested-u.o %t/nested-m.o %t/chain-ref.o --end-lib
# RUN: llvm-objdump --macho --bind --syms %t/t1/chain-lazy | FileCheck %s --check-prefix=STRONG
# RUN: %lld -o %t/t2/chain-lazy --threads=2 -undefined dynamic_lookup %t/chain-main.o --start-lib %t/nested-u.o %t/nested-m.o %t/chain-ref.o --end-lib
# RUN: llvm-objdump --macho --bind --syms %t/t2/chain-lazy | FileCheck %s --check-prefix=STRONG
# RUN: %lld -o %t/t8/chain-lazy --threads=8 -undefined dynamic_lookup %t/chain-main.o --start-lib %t/nested-u.o %t/nested-m.o %t/chain-ref.o --end-lib
# RUN: llvm-objdump --macho --bind --syms %t/t8/chain-lazy | FileCheck %s --check-prefix=STRONG
# RUN: cmp %t/t1/chain-lazy %t/t2/chain-lazy
# RUN: cmp %t/t1/chain-lazy %t/t8/chain-lazy

# RUN: %lld -o %t/t1/chain-weak --threads=1 -undefined dynamic_lookup %t/chain-main.o %t/nested-user.a %t/nested-lib.a %t/chain-weak.a
# RUN: llvm-objdump --macho --bind --syms %t/t1/chain-weak | FileCheck %s --check-prefix=WEAK
# RUN: %lld -o %t/t2/chain-weak --threads=2 -undefined dynamic_lookup %t/chain-main.o %t/nested-user.a %t/nested-lib.a %t/chain-weak.a
# RUN: llvm-objdump --macho --bind --syms %t/t2/chain-weak | FileCheck %s --check-prefix=WEAK
# RUN: %lld -o %t/t8/chain-weak --threads=8 -undefined dynamic_lookup %t/chain-main.o %t/nested-user.a %t/nested-lib.a %t/chain-weak.a
# RUN: llvm-objdump --macho --bind --syms %t/t8/chain-weak | FileCheck %s --check-prefix=WEAK
# RUN: cmp %t/t1/chain-weak %t/t2/chain-weak
# RUN: cmp %t/t1/chain-weak %t/t8/chain-weak

# RUN: %lld -o %t/t1/chain-lazy-weak --threads=1 -undefined dynamic_lookup %t/chain-main.o --start-lib %t/nested-u.o %t/nested-m.o %t/chain-weak.o --end-lib
# RUN: llvm-objdump --macho --bind --syms %t/t1/chain-lazy-weak | FileCheck %s --check-prefix=WEAK
# RUN: %lld -o %t/t2/chain-lazy-weak --threads=2 -undefined dynamic_lookup %t/chain-main.o --start-lib %t/nested-u.o %t/nested-m.o %t/chain-weak.o --end-lib
# RUN: llvm-objdump --macho --bind --syms %t/t2/chain-lazy-weak | FileCheck %s --check-prefix=WEAK
# RUN: %lld -o %t/t8/chain-lazy-weak --threads=8 -undefined dynamic_lookup %t/chain-main.o --start-lib %t/nested-u.o %t/nested-m.o %t/chain-weak.o --end-lib
# RUN: llvm-objdump --macho --bind --syms %t/t8/chain-lazy-weak | FileCheck %s --check-prefix=WEAK
# RUN: cmp %t/t1/chain-lazy-weak %t/t2/chain-lazy-weak
# RUN: cmp %t/t1/chain-lazy-weak %t/t8/chain-lazy-weak

## Anchor the symbol flags and bind line: a weak import has both a "w"
## symbol flag and a trailing "(weak_import)" annotation in the bind table.
# STRONG-LABEL: SYMBOL TABLE:
# STRONG: {{^0+ +\*UND\* _cf$}}
# STRONG-LABEL: Bind table:
# STRONG-COUNT-2: {{^__DATA +__data .* pointer +0 +flat-namespace +_cf$}}

# WEAK-LABEL: SYMBOL TABLE:
# WEAK: {{^0+ +w +\*UND\* _cf$}}
# WEAK-LABEL: Bind table:
# WEAK-COUNT-2: {{^__DATA +__data .* pointer +0 +flat-namespace +_cf \(weak_import\)$}}

# BIND-LABEL: SYMBOL TABLE:
# BIND-DAG:   l     O __DATA_CONST,__cfstring _cf
# BIND-DAG:         *UND* _cf
# BIND-LABEL: Bind table:
# BIND:       __DATA __data {{.*}} pointer 0 flat-namespace _cf

# ARCH:      error: undefined symbol: _cf
# ARCH-NEXT: >>> referenced by {{.*}}user.a(u.o):(__data+0x0)

# OBJ:      error: undefined symbol: _cf
# OBJ-NEXT: >>> referenced by {{.*}}refcf.o:(__data+0x0)

# LAZY:      error: undefined symbol: _cf
# LAZY-NEXT: >>> referenced by {{.*}}u.o:(__data+0x0)

#--- main.s
.globl _main, ___CFConstantStringClassReference
_main:
  callq _f
  retq
.data
___CFConstantStringClassReference:
  .quad 0

#--- mainuser.s
.globl _main, ___CFConstantStringClassReference
_main:
  callq _f
  callq _g
  retq
.data
___CFConstantStringClassReference:
  .quad 0

#--- m.s
.globl _f, _cf
.text
_f:
  retq
.cstring
L_.str:
  .asciz "foo"
.section __DATA,__cfstring
.p2align 3
_cf:
  .quad ___CFConstantStringClassReference
  .long 1992
  .space 4
  .quad L_.str
  .quad 3

#--- u.s
.globl _g
.text
_g:
  retq
.data
  .quad _cf

#--- refcf.s
.data
  .quad _cf

#--- chain-main.s
.globl _main, ___CFConstantStringClassReference
_main:
callq _h
retq
.data
.p2align 3
___CFConstantStringClassReference:
.quad 0

#--- chain-ref.s
.ifdef WEAK
.weak_reference _cf
.endif
.globl _h
.text
_h:
retq
.data
.p2align 3
.quad _cf

#--- nested-m.s
.globl _f, _cf
.text
_f:
  callq _g
  retq
.cstring
L_.str:
  .asciz "foo"
.section __DATA,__cfstring
.p2align 3
_cf:
  .quad ___CFConstantStringClassReference
  .long 1992
  .space 4
  .quad L_.str
  .quad 3


#--- nested-u.s
.weak_reference _cf
.globl _g
.text
_g:
  retq
.data
  .quad _cf
