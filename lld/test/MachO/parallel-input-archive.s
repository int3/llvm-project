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

## A member's undefined names are enumerated from its LC_DYSYMTAB undefined
## range, and each one gets its own position, so the enumeration has to match the
## undefined records ObjFile::parseSymbols() applies, one for one and in order.
## dys.o exercises the two ways that can go wrong: a common symbol, which is also
## N_UNDF and sits inside the undefined range but is not an undefined record, and
## a nested extraction between two of its undefineds, whose positions are
## allocated in between.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dysmain.s -o %t/dysmain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dyshead.s -o %t/dyshead.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dysn1.s -o %t/dysn1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dysn2.s -o %t/dysn2.o
# RUN: llvm-ar --format=darwin rcs %t/dys.a %t/dyshead.o %t/dysn1.o %t/dysn2.o
# RUN: mkdir -p %t/dys1 %t/dys2 %t/dys8 %t/dys16
# RUN: %lld -lSystem -o %t/dys1/out --threads=1 -why_load %t/dysmain.o %t/dys.a 2>&1 | FileCheck %s --check-prefix=DYSWHY
# RUN: %lld -lSystem -o %t/dys2/out --threads=2 -why_load %t/dysmain.o %t/dys.a 2>&1 | FileCheck %s --check-prefix=DYSWHY
# RUN: %lld -lSystem -o %t/dys8/out --threads=8 -why_load %t/dysmain.o %t/dys.a 2>&1 | FileCheck %s --check-prefix=DYSWHY
# RUN: %lld -lSystem -o %t/dys16/out --threads=16 -why_load %t/dysmain.o %t/dys.a 2>&1 | FileCheck %s --check-prefix=DYSWHY
# RUN: cmp %t/dys1/out %t/dys2/out
# RUN: cmp %t/dys1/out %t/dys8/out
# RUN: cmp %t/dys1/out %t/dys16/out
# RUN: llvm-objdump --syms %t/dys1/out | FileCheck %s --check-prefix=DYSSYMS

# DYSWHY:      _head forced load of {{.*}}dys.a(dyshead.o)
# DYSWHY-NEXT: _n1 forced load of {{.*}}dys.a(dysn1.o)
# DYSWHY-NEXT: _n2 forced load of {{.*}}dys.a(dysn2.o)

# DYSSYMS-DAG: __TEXT,dh _head
# DYSSYMS-DAG: __TEXT,d1 _n1
# DYSSYMS-DAG: __TEXT,d2 _n2
# DYSSYMS-DAG: _shared_common

## The record ordinal's three fields share 63 bits of one integer, so a link big
## enough to overflow one of them has to be handled rather than truncated.
## LLD_ORDINAL_BITS narrows the widths so that both paths can be reached here.
# RUN: mkdir -p %t/narrowwalk
## Too few bits for the archive's walk events: the walk cannot number them, so it
## declines and the archives publish their indexes the ordinary way. Same output.
# RUN: env LLD_ORDINAL_BITS=17,1,23 %lld -lSystem -o %t/narrowwalk/out --threads=8 %t/dysmain.o %t/dys.a
# RUN: cmp %t/dys1/out %t/narrowwalk/out
## Too few bits for the command line: a clear error, not a wrong ordinal.
## The top-level-ordinal overflow diagnostic is in
## parallel-input-ordinal-overflow.s: it comes from fatal(), which the
## LLD_IN_TEST=2 harness cannot show.

## Design 7's "Stage 2 position rule" minimal test. a.o references _foo; the
## member defining it has undefineds _bar then _qux in nlist order; the TBD is
## listed *after* the archive and exports _qux before _bar. History creates _bar
## then _qux, at the member's undefined events, because the archive's registration
## precedes the dylib on the command line. If the walk numbered its events from
## the command-line counter instead of the archive's own, the dylib's exports
## would sort first and the two stubs would swap.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/pos-main.s -o %t/pos-main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/pos-foo.s -o %t/pos-foo.o
# RUN: llvm-ar --format=darwin rcs %t/pos.a %t/pos-foo.o
# RUN: mkdir -p %t/pos1 %t/pos2 %t/pos8
# RUN: %lld -lSystem -dylib -install_name /usr/lib/libposout.dylib -o %t/pos1/out --threads=1 %t/pos-main.o %t/pos.a %t/pos.tbd
# RUN: %lld -lSystem -dylib -install_name /usr/lib/libposout.dylib -o %t/pos2/out --threads=2 %t/pos-main.o %t/pos.a %t/pos.tbd
# RUN: %lld -lSystem -dylib -install_name /usr/lib/libposout.dylib -o %t/pos8/out --threads=8 %t/pos-main.o %t/pos.a %t/pos.tbd
# RUN: cmp %t/pos1/out %t/pos2/out
# RUN: cmp %t/pos1/out %t/pos8/out
## symVector order is what the symbol table emitter walks, so the undefined
## entries come out in the order their slots were created: _bar before _qux.
# RUN: llvm-objdump --macho --syms %t/pos1/out | FileCheck %s --check-prefix=POSORD

# POSORD: *UND* _bar
# POSORD: *UND* _qux

## The symmetric case: a name the member *defines* is also exported by the TBD
## listed after the archive, so its position is the index entry's, not the
## export's.
# RUN: mkdir -p %t/possym1 %t/possym8
# RUN: %lld -lSystem -dylib -install_name /usr/lib/libposout.dylib -o %t/possym1/out --threads=1 %t/pos-main.o %t/pos.a %t/pos-foo.tbd
# RUN: %lld -lSystem -dylib -install_name /usr/lib/libposout.dylib -o %t/possym8/out --threads=8 %t/pos-main.o %t/pos.a %t/pos-foo.tbd
# RUN: cmp %t/possym1/out %t/possym8/out

## overridesWeakDef: libweak exports _x weakly and nothing references it at the
## point wo.a is registered, so the serial linker's index entry replaced the weak
## dylib symbol with a lazy one and the member's later definition set no override.
## The only reference to _x comes from a member of the second archive, so the walk
## still has the weak dylib symbol in the slot when the definition folds.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wo-main.s -o %t/wo-main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wo-def.s -o %t/wo-def.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wo-ref.s -o %t/wo-ref.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wo-dylib.s -o %t/wo-dylib.o
# RUN: %lld -dylib -o %t/libwo.dylib %t/wo-dylib.o
# RUN: llvm-ar --format=darwin rcs %t/wo-def.a %t/wo-def.o
# RUN: llvm-ar --format=darwin rcs %t/wo-ref.a %t/wo-ref.o
# RUN: mkdir -p %t/wo1 %t/wo8
# RUN: %lld -lSystem -o %t/wo1/out --threads=1 %t/wo-main.o %t/libwo.dylib %t/wo-def.a %t/wo-ref.a
# RUN: %lld -lSystem -o %t/wo8/out --threads=8 %t/wo-main.o %t/libwo.dylib %t/wo-def.a %t/wo-ref.a
# RUN: cmp %t/wo1/out %t/wo8/out
# RUN: llvm-objdump --macho --weak-bind %t/wo1/out | FileCheck %s --check-prefix=NOWEAKBIND
# RUN: llvm-objdump --macho --weak-bind %t/wo8/out | FileCheck %s --check-prefix=NOWEAKBIND

# NOWEAKBIND-NOT: _x

## A name provided both by a walked command-line archive and by an autolinked
## archive published after the phase, first referenced by a member the autolink
## pass itself brings in. libalearly.a's index entry for _shared created the slot
## at its own command-line position, so the serial linker fetched from there and
## liballate.a's later lazy symbol never owned it. The walk leaves no lazy symbol
## behind, so addUndefined() has to ask the provider map before taking the lazy
## symbol it does find.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/al-main.s -o %t/al-main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/al-early.s -o %t/al-early.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/al-sharedlate.s -o %t/al-sharedlate.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/al-late.s -o %t/al-late.o
# RUN: llvm-ar --format=darwin rcs %t/libalearly.a %t/al-early.o
# RUN: mkdir -p %t/allate
# RUN: llvm-ar --format=darwin rcs %t/allate/liballate.a %t/al-sharedlate.o %t/al-late.o
# RUN: mkdir -p %t/al1 %t/al2 %t/al8
# RUN: %lld -lSystem -L%t/allate -o %t/al1/out --threads=1 -why_load %t/al-main.o %t/libalearly.a 2>&1 | FileCheck %s --check-prefix=ALWHY
# RUN: %lld -lSystem -L%t/allate -o %t/al2/out --threads=2 -why_load %t/al-main.o %t/libalearly.a 2>&1 | FileCheck %s --check-prefix=ALWHY
# RUN: %lld -lSystem -L%t/allate -o %t/al8/out --threads=8 -why_load %t/al-main.o %t/libalearly.a 2>&1 | FileCheck %s --check-prefix=ALWHY
# RUN: cmp %t/al1/out %t/al2/out
# RUN: cmp %t/al1/out %t/al8/out

## The nested fetch is reported first, because the member that triggered it is
## only announced once its own parse -- and so every extraction inside it -- has
## finished.
# ALWHY: _shared forced load of {{.*}}libalearly.a(al-early.o)
# ALWHY-NEXT: _needs_late forced load of {{.*}}liballate.a(al-late.o)
# ALWHY-NOT: al-sharedlate.o

## Inline unwind replay order. The parent member owns a weak _wp and its nested
## child owns a weak _wc, both with __eh_frame entries and no -dead_strip, and a
## third member extracted by the parent's *second* undefined defines both
## strongly. So both members are flagged for inline unwind registration (design 7
## section 3), the displacing records sort between the child's completion and the
## parent's, and the parent is adopted first while completing last. Replaying in
## adoption order strips the child's frame; it has to be completion order. The
## members are laid out so that the parent's index entry comes last, because an
## undefined can only extract from an entry the walk has already passed.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/gp-main.s -o %t/gp-main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/gp-parent.s -o %t/gp-parent.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/gp-child.s -o %t/gp-child.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/gp-strong.s -o %t/gp-strong.o
# RUN: llvm-ar --format=darwin rcs %t/gp.a %t/gp-child.o %t/gp-strong.o %t/gp-parent.o
# RUN: mkdir -p %t/gp1 %t/gp2 %t/gp8 %t/gp16
# RUN: %lld -lSystem -o %t/gp1/out --threads=1 %t/gp-main.o %t/gp.a
# RUN: %lld -lSystem -o %t/gp2/out --threads=2 %t/gp-main.o %t/gp.a
# RUN: %lld -lSystem -o %t/gp8/out --threads=8 %t/gp-main.o %t/gp.a
# RUN: %lld -lSystem -o %t/gp16/out --threads=16 %t/gp-main.o %t/gp.a
# RUN: cmp %t/gp1/out %t/gp2/out
# RUN: cmp %t/gp1/out %t/gp8/out
# RUN: cmp %t/gp1/out %t/gp16/out

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

#--- dysmain.s
.globl _main
_main:
  callq _head
  retq

#--- dyshead.s
## The undefined range holds _n1, _n2 and the common symbol _shared_common; only
## _n1 and _n2 are undefined records, and each extracts a member.
.globl _head
.comm _shared_common,8,3
.section __TEXT,dh
_head:
  callq _n1
  callq _n2
  retq

#--- dysn1.s
.globl _n1
.comm _shared_common,8,3
.section __TEXT,d1
_n1:
  retq

#--- dysn2.s
.globl _n2
.section __TEXT,d2
_n2:
  retq

#--- pos-main.s
.globl _entry
_entry:
  callq _foo
  retq

#--- pos-foo.s
.globl _foo
.section __TEXT,pf
_foo:
  callq _bar
  callq _qux
  retq

#--- pos.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libpos.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _qux, _bar ]
...

#--- pos-foo.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libposfoo.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _foo, _qux, _bar ]
...

#--- wo-main.s
.globl _main
_main:
  callq _trigger
  retq

#--- wo-dylib.s
.globl _x
.weak_definition _x
_x:
  retq

#--- wo-def.s
.globl _x
.section __TEXT,wd
_x:
  retq

#--- wo-ref.s
.globl _trigger
.section __TEXT,wr
_trigger:
  callq _x
  retq

#--- al-main.s
.linker_option "-lallate"
.globl _main
_main:
  callq _needs_late
  retq

#--- al-early.s
.globl _shared
.section __TEXT,ae
_shared:
  retq

#--- al-sharedlate.s
.globl _shared
.section __TEXT,asl
_shared:
  retq

#--- al-late.s
.globl _needs_late
.section __TEXT,al
_needs_late:
  callq _shared
  retq

#--- gp-main.s
.globl _main
_main:
  callq _parent
  retq

#--- gp-parent.s
.globl _parent
_parent:
  callq _child
  callq _strong
  retq
.globl _wp
.weak_definition _wp
_wp:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  retq
  .cfi_endproc

#--- gp-child.s
.globl _child
_child:
  retq
.globl _wc
.weak_definition _wc
_wc:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  retq
  .cfi_endproc

#--- gp-strong.s
.globl _strong
_strong:
  retq
.globl _wp
_wp:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  retq
  .cfi_endproc
.globl _wc
_wc:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  retq
  .cfi_endproc
