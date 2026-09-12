# REQUIRES: x86
## -ObjC makes an archive load members regardless of any reference: the ones an
## _OBJC_CLASS_$_ index entry names, in index order, then the ones carrying an
## __objc_catlist or __swift* section, in member order. The walker replays both
## passes as a prelude at the archive's own registration point, so the member set,
## the -why_load order and the reason strings must not depend on the thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/early.s -o %t/early.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/plain.s -o %t/plain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/cat.s -o %t/cat.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/klass.s -o %t/klass.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/later.s -o %t/later.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/swift.s -o %t/swift.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/unref.s -o %t/unref.o
# RUN: llvm-ar --format=darwin rcs %t/early.a %t/early.o
# RUN: llvm-ar --format=darwin rcs %t/objc.a %t/plain.o %t/cat.o %t/klass.o %t/later.o %t/swift.o %t/unref.o

# RUN: mkdir -p %t/o1 %t/o2 %t/o8
# RUN: %lld -lSystem -ObjC -o %t/o1/out --threads=1 -why_load %t/main.o %t/early.a %t/objc.a > %t/why1.txt
# RUN: %lld -lSystem -ObjC -o %t/o2/out --threads=2 -why_load %t/main.o %t/early.a %t/objc.a > %t/why2.txt
# RUN: %lld -lSystem -ObjC -o %t/o8/out --threads=8 -why_load %t/main.o %t/early.a %t/objc.a > %t/why8.txt
# RUN: cmp %t/o1/out %t/o2/out
# RUN: cmp %t/o1/out %t/o8/out
# RUN: diff %t/why1.txt %t/why2.txt
# RUN: diff %t/why1.txt %t/why8.txt
# RUN: FileCheck %s --check-prefix=WHY < %t/why1.txt

## The class-symbol pass runs first, in index order. Then the section pass, in
## member order: cat.o has no index entry at all, and its undefined reference to
## early.a's member is satisfied from an archive already registered, so that
## nested load is announced before cat.o itself. Its reference to _later cannot
## fetch yet, because objc.a's own index is not registered during the prelude;
## that member is loaded from its index entry afterwards, with the symbol as the
## reason. unref.o is never loaded.
# WHY:      _OBJC_CLASS_$_Foo forced load of {{.*}}objc.a(klass.o)
# WHY-NEXT: _early_helper forced load of {{.*}}early.a(early.o)
# WHY-NEXT: -ObjC forced load of {{.*}}objc.a(cat.o)
# WHY-NEXT: -ObjC forced load of {{.*}}objc.a(swift.o)
# WHY-NEXT: _plain forced load of {{.*}}objc.a(plain.o)
# WHY-NEXT: _later forced load of {{.*}}objc.a(later.o)
# WHY-NOT:  unref.o

# RUN: llvm-objdump --syms %t/o1/out | FileCheck %s --check-prefix=SYMS
# SYMS-DAG: __TEXT,pl _plain
# SYMS-DAG: __TEXT,kf _OBJC_CLASS_$_Foo
# SYMS-DAG: __TEXT,lt _later
# SYMS-DAG: __TEXT,sw _swifty
# SYMS-DAG: __TEXT,eh _early_helper
# SYMS-NOT: _unref

## A member the class pass forces in, duplicating a strong definition a direct
## object already made: the fetch happens anyway and the duplicate is diagnosed,
## in the same order at every thread count.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dupmain.s -o %t/dupmain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup.s -o %t/dup.o
# RUN: llvm-ar --format=darwin rcs %t/dup.a %t/dup.o
# RUN: not %lld -lSystem -ObjC -o %t/dup1 --threads=1 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -ObjC -o %t/dup2 --threads=2 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -ObjC -o %t/dup8 --threads=8 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP

# DUP:      error: duplicate symbol: _dup
# DUP-NEXT: >>> defined in {{.*}}dupmain.o
# DUP-NEXT: >>> defined in {{.*}}dup.a(dup.o)

## The publish path: an object after the archives ends the input phase, so the
## held indexes are published at the end of it rather than walked. The mandatory
## members still have to load at each archive's own position, before its index.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/trailing.s -o %t/trailing.o
# RUN: mkdir -p %t/p1 %t/p2 %t/p8
# RUN: %lld -lSystem -ObjC -o %t/p1/out --threads=1 -why_load %t/main.o %t/early.a %t/objc.a %t/trailing.o > %t/pwhy1.txt
# RUN: %lld -lSystem -ObjC -o %t/p2/out --threads=2 -why_load %t/main.o %t/early.a %t/objc.a %t/trailing.o > %t/pwhy2.txt
# RUN: %lld -lSystem -ObjC -o %t/p8/out --threads=8 -why_load %t/main.o %t/early.a %t/objc.a %t/trailing.o > %t/pwhy8.txt
# RUN: cmp %t/p1/out %t/p2/out
# RUN: cmp %t/p1/out %t/p8/out
# RUN: diff %t/pwhy1.txt %t/pwhy2.txt
# RUN: diff %t/pwhy1.txt %t/pwhy8.txt
# RUN: FileCheck %s --check-prefix=WHY < %t/pwhy1.txt

## The decline path: a member carrying an LC_LINKER_OPTION loads a library after
## the phase, which the walk only learns from its own prepass, so it gives the
## whole archive set back to addLazySymbols() -- and the mandatory members have to
## load there too, still before the index.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/lomain.s -o %t/lomain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/lo-objc.s -o %t/lo-objc.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/lo-opt.s -o %t/lo-opt.o
# RUN: llvm-ar --format=darwin rcs %t/lo.a %t/lo-objc.o %t/lo-opt.o
# RUN: mkdir -p %t/l1 %t/l2 %t/l8
# RUN: %lld -lSystem -ObjC -o %t/l1/out --threads=1 -why_load %t/lomain.o %t/lo.a > %t/lwhy1.txt
# RUN: %lld -lSystem -ObjC -o %t/l2/out --threads=2 -why_load %t/lomain.o %t/lo.a > %t/lwhy2.txt
# RUN: %lld -lSystem -ObjC -o %t/l8/out --threads=8 -why_load %t/lomain.o %t/lo.a > %t/lwhy8.txt
# RUN: cmp %t/l1/out %t/l2/out
# RUN: cmp %t/l1/out %t/l8/out
# RUN: diff %t/lwhy1.txt %t/lwhy2.txt
# RUN: diff %t/lwhy1.txt %t/lwhy8.txt
# RUN: FileCheck %s --check-prefix=LOWHY < %t/lwhy1.txt

# LOWHY:      -ObjC forced load of {{.*}}lo.a(lo-objc.o)
# LOWHY-NEXT: _loplain forced load of {{.*}}lo.a(lo-opt.o)

#--- main.s
.globl _main
_main:
  callq _plain
  retq

#--- early.s
.globl _early_helper
.section __TEXT,eh
_early_helper:
  retq

#--- plain.s
.globl _plain
.section __TEXT,pl
_plain:
  retq

## No global symbol at all, so this member has no index entry: only the member
## walk of the -ObjC pass can reach it.
#--- cat.s
.section __DATA,__objc_catlist
.quad 0
.section __TEXT,ct
Lcat:
  callq _later
  callq _early_helper
  retq

#--- klass.s
.globl _OBJC_CLASS_$_Foo
.section __TEXT,kf
_OBJC_CLASS_$_Foo:
  retq

#--- later.s
.globl _later
.section __TEXT,lt
_later:
  retq

#--- swift.s
.section __TEXT,__swift5_typeref
.quad 0
.globl _swifty
.section __TEXT,sw
_swifty:
  retq

#--- unref.s
.globl _unref
.section __TEXT,un
_unref:
  retq

#--- dupmain.s
.globl _main
_main:
  retq
.globl _dup
.section __TEXT,dm
_dup:
  retq

#--- dup.s
.globl _OBJC_CLASS_$_Dup
.section __TEXT,dk
_OBJC_CLASS_$_Dup:
  retq
.globl _dup
.section __TEXT,da
_dup:
  retq

#--- trailing.s
.globl _trailing
.section __TEXT,tr
_trailing:
  retq

#--- lomain.s
.globl _main
_main:
  callq _loplain
  retq

#--- lo-objc.s
.section __DATA,__objc_catlist
.quad 0
.globl _loobjc
.section __TEXT,lo
_loobjc:
  retq

#--- lo-opt.s
.linker_option "-lSystem"
.globl _loplain
.section __TEXT,lp
_loplain:
  retq
