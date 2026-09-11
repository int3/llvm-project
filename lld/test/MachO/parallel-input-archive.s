# REQUIRES: x86
## Archive extraction causality under the parallel input phase: which member is
## fetched, in what order, and how a dylib definition interacts with it, must not
## depend on the thread count.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/pre1 %t/pre8 %t/pre16 %t/cas1 %t/cas2 %t/cas4 %t/cas8 %t/cas16
# RUN: mkdir -p %t/dus1 %t/dus8 %t/dsu1 %t/dsu8 %t/uds1 %t/uds8
# RUN: mkdir -p %t/uad1 %t/uad8 %t/aud1 %t/aud8 %t/adu1 %t/adu8
# RUN: mkdir -p %t/wdus1 %t/wdus8 %t/wdsu1 %t/wdsu8 %t/wuds1 %t/wuds8
# RUN: mkdir -p %t/wuad1 %t/wuad8 %t/waud1 %t/waud8 %t/wadu1 %t/wadu8
# RUN: mkdir -p %t/objc1 %t/objc8

# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/pad.s -o %t/pad.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/weakdirect.s -o %t/weakdirect.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/b.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/c.s -o %t/c.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/maindy.s -o %t/maindy.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dylib-strong.s -o %t/dylib-strong.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dylib-weak.s -o %t/dylib-weak.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/uses-dysym.s -o %t/uses-dysym.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/ar-dysym.s -o %t/ar-dysym.o

# RUN: llvm-ar --format=darwin rcs %t/abc.a %t/a.o %t/c.o %t/b.o
# RUN: llvm-ar --format=darwin rcs %t/dysym.a %t/ar-dysym.o
# RUN: %lld -dylib -o %t/libstrong.dylib %t/dylib-strong.o
# RUN: %lld -dylib -o %t/libweak.dylib %t/dylib-weak.o

## An archive listed before an object that also defines the fetched name: main.o's
## undefined reference precedes the archive, so the member must still be fetched
## and its strong definition must beat the later weak direct one.
# RUN: %lld -lSystem -o %t/pre1/out --threads=1 %t/main.o %t/abc.a %t/weakdirect.o
# RUN: %lld -lSystem -o %t/pre8/out --threads=8 %t/main.o %t/abc.a %t/weakdirect.o
# RUN: %lld -lSystem -o %t/pre16/out --threads=16 %t/main.o %t/abc.a %t/weakdirect.o
# RUN: cmp %t/pre1/out %t/pre8/out
# RUN: cmp %t/pre1/out %t/pre16/out
# RUN: llvm-objdump --syms %t/pre1/out | FileCheck %s --check-prefix=FROMARCHIVE

# FROMARCHIVE: __TEXT,fa _a

## _a pulls in a.o, whose reference to _c pulls in c.o, whose definition of _b
## means b.o is never fetched.
# RUN: %lld -lSystem -o %t/cas1/out --threads=1 -why_load %t/main.o %t/pad.o %t/abc.a 2>&1 | FileCheck %s --check-prefix=WHYLOAD
# RUN: %lld -lSystem -o %t/cas2/out --threads=2 -why_load %t/main.o %t/pad.o %t/abc.a 2>&1 | FileCheck %s --check-prefix=WHYLOAD
# RUN: %lld -lSystem -o %t/cas4/out --threads=4 -why_load %t/main.o %t/pad.o %t/abc.a 2>&1 | FileCheck %s --check-prefix=WHYLOAD
# RUN: %lld -lSystem -o %t/cas8/out --threads=8 -why_load %t/main.o %t/pad.o %t/abc.a 2>&1 | FileCheck %s --check-prefix=WHYLOAD
# RUN: %lld -lSystem -o %t/cas16/out --threads=16 -why_load %t/main.o %t/pad.o %t/abc.a 2>&1 | FileCheck %s --check-prefix=WHYLOAD
# RUN: cmp %t/cas1/out %t/cas8/out
# RUN: cmp %t/cas1/out %t/cas16/out

# WHYLOAD:      _a forced load of {{.*}}abc.a(a.o)
# WHYLOAD-NEXT: _c forced load of {{.*}}abc.a(c.o)
# WHYLOAD-NOT:  (b.o)

## The six orderings of a strong dylib definition, an undefined reference and an
## archive that also provides the name. The dylib suppresses the fetch only when
## it precedes the archive.
# RUN: %lld -lSystem -o %t/dus1/out --threads=1 %t/maindy.o %t/libstrong.dylib %t/uses-dysym.o %t/dysym.a
# RUN: %lld -lSystem -o %t/dus8/out --threads=8 %t/maindy.o %t/libstrong.dylib %t/uses-dysym.o %t/dysym.a
# RUN: cmp %t/dus1/out %t/dus8/out
# RUN: %lld -lSystem -o %t/dsu1/out --threads=1 %t/maindy.o %t/libstrong.dylib %t/dysym.a %t/uses-dysym.o
# RUN: %lld -lSystem -o %t/dsu8/out --threads=8 %t/maindy.o %t/libstrong.dylib %t/dysym.a %t/uses-dysym.o
# RUN: cmp %t/dsu1/out %t/dsu8/out
# RUN: %lld -lSystem -o %t/uds1/out --threads=1 %t/maindy.o %t/uses-dysym.o %t/libstrong.dylib %t/dysym.a
# RUN: %lld -lSystem -o %t/uds8/out --threads=8 %t/maindy.o %t/uses-dysym.o %t/libstrong.dylib %t/dysym.a
# RUN: cmp %t/uds1/out %t/uds8/out
# RUN: %lld -lSystem -o %t/uad1/out --threads=1 %t/maindy.o %t/uses-dysym.o %t/dysym.a %t/libstrong.dylib
# RUN: %lld -lSystem -o %t/uad8/out --threads=8 %t/maindy.o %t/uses-dysym.o %t/dysym.a %t/libstrong.dylib
# RUN: cmp %t/uad1/out %t/uad8/out
# RUN: %lld -lSystem -o %t/aud1/out --threads=1 %t/maindy.o %t/dysym.a %t/uses-dysym.o %t/libstrong.dylib
# RUN: %lld -lSystem -o %t/aud8/out --threads=8 %t/maindy.o %t/dysym.a %t/uses-dysym.o %t/libstrong.dylib
# RUN: cmp %t/aud1/out %t/aud8/out
# RUN: %lld -lSystem -o %t/adu1/out --threads=1 %t/maindy.o %t/dysym.a %t/libstrong.dylib %t/uses-dysym.o
# RUN: %lld -lSystem -o %t/adu8/out --threads=8 %t/maindy.o %t/dysym.a %t/libstrong.dylib %t/uses-dysym.o
# RUN: cmp %t/adu1/out %t/adu8/out

## The dylib-precedes-archive orderings keep the dylib definition...
# RUN: llvm-objdump --syms %t/dus1/out | FileCheck %s --check-prefix=NOFETCH
# RUN: llvm-objdump --syms %t/dsu1/out | FileCheck %s --check-prefix=NOFETCH
# RUN: llvm-objdump --syms %t/uds1/out | FileCheck %s --check-prefix=NOFETCH
## ...and the archive-precedes-dylib orderings fetch the member.
# RUN: llvm-objdump --syms %t/uad1/out | FileCheck %s --check-prefix=FETCHED
# RUN: llvm-objdump --syms %t/aud1/out | FileCheck %s --check-prefix=FETCHED
# RUN: llvm-objdump --syms %t/adu1/out | FileCheck %s --check-prefix=FETCHED

# NOFETCH-NOT: __TEXT,fromarchive _dysym
# FETCHED:     __TEXT,fromarchive _dysym

## A weak dylib definition never suppresses the fetch, in any of the six orders.
# RUN: %lld -lSystem -o %t/wdus1/out --threads=1 %t/maindy.o %t/libweak.dylib %t/uses-dysym.o %t/dysym.a
# RUN: %lld -lSystem -o %t/wdus8/out --threads=8 %t/maindy.o %t/libweak.dylib %t/uses-dysym.o %t/dysym.a
# RUN: cmp %t/wdus1/out %t/wdus8/out
# RUN: llvm-objdump --syms %t/wdus1/out | FileCheck %s --check-prefix=FETCHED
# RUN: %lld -lSystem -o %t/wdsu1/out --threads=1 %t/maindy.o %t/libweak.dylib %t/dysym.a %t/uses-dysym.o
# RUN: %lld -lSystem -o %t/wdsu8/out --threads=8 %t/maindy.o %t/libweak.dylib %t/dysym.a %t/uses-dysym.o
# RUN: cmp %t/wdsu1/out %t/wdsu8/out
# RUN: llvm-objdump --syms %t/wdsu1/out | FileCheck %s --check-prefix=FETCHED
# RUN: %lld -lSystem -o %t/wuds1/out --threads=1 %t/maindy.o %t/uses-dysym.o %t/libweak.dylib %t/dysym.a
# RUN: %lld -lSystem -o %t/wuds8/out --threads=8 %t/maindy.o %t/uses-dysym.o %t/libweak.dylib %t/dysym.a
# RUN: cmp %t/wuds1/out %t/wuds8/out
# RUN: llvm-objdump --syms %t/wuds1/out | FileCheck %s --check-prefix=FETCHED
# RUN: %lld -lSystem -o %t/wuad1/out --threads=1 %t/maindy.o %t/uses-dysym.o %t/dysym.a %t/libweak.dylib
# RUN: %lld -lSystem -o %t/wuad8/out --threads=8 %t/maindy.o %t/uses-dysym.o %t/dysym.a %t/libweak.dylib
# RUN: cmp %t/wuad1/out %t/wuad8/out
# RUN: llvm-objdump --syms %t/wuad1/out | FileCheck %s --check-prefix=FETCHED
# RUN: %lld -lSystem -o %t/waud1/out --threads=1 %t/maindy.o %t/dysym.a %t/uses-dysym.o %t/libweak.dylib
# RUN: %lld -lSystem -o %t/waud8/out --threads=8 %t/maindy.o %t/dysym.a %t/uses-dysym.o %t/libweak.dylib
# RUN: cmp %t/waud1/out %t/waud8/out
# RUN: llvm-objdump --syms %t/waud1/out | FileCheck %s --check-prefix=FETCHED
# RUN: %lld -lSystem -o %t/wadu1/out --threads=1 %t/maindy.o %t/dysym.a %t/libweak.dylib %t/uses-dysym.o
# RUN: %lld -lSystem -o %t/wadu8/out --threads=8 %t/maindy.o %t/dysym.a %t/libweak.dylib %t/uses-dysym.o
# RUN: cmp %t/wadu1/out %t/wadu8/out
# RUN: llvm-objdump --syms %t/wadu1/out | FileCheck %s --check-prefix=FETCHED

## -ObjC fetches members in archive order; that order must not change either.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/objc1.s -o %t/objc1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/objc2.s -o %t/objc2.o
# RUN: llvm-ar --format=darwin rcs %t/objc.a %t/objc1.o %t/objc2.o
# RUN: %lld -lSystem -ObjC -o %t/objc1/out --threads=1 -why_load %t/main.o %t/pad.o %t/abc.a %t/objc.a 2>&1 | FileCheck %s --check-prefix=OBJC
# RUN: %lld -lSystem -ObjC -o %t/objc8/out --threads=8 -why_load %t/main.o %t/pad.o %t/abc.a %t/objc.a 2>&1 | FileCheck %s --check-prefix=OBJC
# RUN: cmp %t/objc1/out %t/objc8/out

# OBJC:      -ObjC forced load of {{.*}}objc.a(objc1.o)
# OBJC-NEXT: -ObjC forced load of {{.*}}objc.a(objc2.o)

#--- main.s
.globl _main
_main:
  callq _a
  retq

#--- pad.s
.globl _pad
_pad:
  retq

#--- weakdirect.s
.globl _a
.weak_definition _a
.section __TEXT,direct
_a:
  retq

#--- a.s
.globl _a
.section __TEXT,fa
_a:
  callq _c
  retq

#--- b.s
.globl _b, _c
.section __TEXT,fb
_b:
_c:
  retq

#--- c.s
.globl _c, _b
.section __TEXT,fc
_c:
_b:
  retq

#--- maindy.s
.globl _main
_main:
  callq _dysym
  callq _uses
  retq

#--- dylib-strong.s
.globl _dysym
_dysym:
  retq

#--- dylib-weak.s
.globl _dysym
.weak_definition _dysym
_dysym:
  retq

#--- uses-dysym.s
.globl _uses
_uses:
  callq _dysym
  retq

#--- ar-dysym.s
.globl _dysym
.section __TEXT,fromarchive
_dysym:
  retq

#--- objc1.s
.globl _objc1
.section __DATA,__objc_catlist
.quad 0
.section __TEXT,o1
_objc1:
  retq

#--- objc2.s
.globl _objc2
.section __DATA,__objc_catlist
.quad 0
.section __TEXT,o2
_objc2:
  retq
