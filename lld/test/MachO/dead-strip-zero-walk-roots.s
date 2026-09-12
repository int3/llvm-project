# REQUIRES: x86

## markLive() no longer walks every symbol slot of every object file to find the
## locals marked .no_dead_strip, and no longer walks all of inputSections to find
## the sections that are roots on their own. parseSymbols() records the local
## roots as nlist indices while it already has the nlist entry in hand, and
## gatherInputSections() collects the section roots on the workers that already
## read each section's flags. The enqueue order must be unchanged, so the
## generated input creates the marked locals in a per-section order that differs
## from their nlist order, and spreads every kind of root over two object files.
## The two links use the same output basename so nothing that embeds the output
## path can mask a difference.

# RUN: rm -rf %t; mkdir -p %t/one %t/eight %t/io1 %t/io8
# RUN: %python %S/Inputs/gen-dead-strip-zero-walk.py a 64 > %t/a.s
# RUN: %python %S/Inputs/gen-dead-strip-zero-walk.py b 48 > %t/b.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/b.s -o %t/b.o

# RUN: %lld -lSystem -dead_strip -no_uuid --threads=1 %t/a.o %t/b.o -o %t/one/out
# RUN: %lld -lSystem -dead_strip -no_uuid --threads=8 %t/a.o %t/b.o -o %t/eight/out
# RUN: cmp %t/one/out %t/eight/out

## Every marked local keeps its referent alive, as do the no_dead_strip section
## and the mod_init_funcs / mod_term_funcs sections. Unmarked, unreferenced
## definitions are still stripped.
# RUN: llvm-objdump --syms %t/eight/out \
# RUN:   | FileCheck %s --implicit-check-not _unrefR
# CHECK-DAG: _targetPa000
# CHECK-DAG: _targetQa000
# CHECK-DAG: _targetPa063
# CHECK-DAG: _targetQa063
# CHECK-DAG: _targetPb047
# CHECK-DAG: _targetQb047
# CHECK-DAG: _targetSectiona
# CHECK-DAG: _targetSectionb
# CHECK-DAG: _initFna
# CHECK-DAG: _termFna
# CHECK-DAG: _initFnb
# CHECK-DAG: _termFnb
## _extLabel is an N_EXT label in a __DATA,__cfstring section, which
## shouldIgnoreLabel() sends down createDefined()'s local path: it is a root even
## though its nlist entry has N_EXT set. A scope test of just `!(n_type & N_EXT)`
## would miss it and strip its referent.
# CHECK-DAG: _targetIgnoreda
# CHECK-DAG: _targetIgnoredb

## -why_live reports as it marks, so it observes the root order directly.
# RUN: %lld -lSystem -dead_strip -no_uuid -why_live _targetPa000 \
# RUN:   -why_live _targetSectionb --threads=8 %t/a.o %t/b.o -o %t/why 2>&1 \
# RUN:   | FileCheck %s --check-prefix=WHY
# WHY-DAG: _targetPa000
# WHY-DAG: _targetSectionb

## The order of the local roots themselves, which is the part the mechanism can
## break. _keepQa000 lives in __DATA,__secb and _keepPa001 in __DATA,__seca, so
## their nlist order and their creation order disagree: the generator interleaves
## the two sections, so the nlist has _keepPa000, _keepQa000, _keepPa001, ...,
## putting Q a000 before P a001; parseSymbols() creates section symbols per
## section in address order, finishing all of __seca before __secb, which puts
## P a001 before Q a000. markLive() must enqueue in nlist order. The worklist is
## drained LIFO, so the root enqueued *later* is marked first: in nlist order
## that is _keepPa001, and _targetPa001 -- which only _keepPa001 keeps alive --
## is therefore reported before _targetQa000. Drop the sort over
## `noDeadStripLocals` and the creation order enqueues _keepQa000 last instead,
## so the two reports swap. Ordered checks, not -DAG; each root's own line is
## pinned as the chain step under its target, so a report attributed to the wrong
## root fails too.
# RUN: %lld -lSystem -dead_strip -no_uuid -why_live _targetQa000 \
# RUN:   -why_live _targetPa001 --threads=8 %t/a.o %t/b.o -o %t/whyorder 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ORDER
# ORDER: _targetPa001 from
# ORDER-NEXT: _keepPa001 from
# ORDER-NEXT: _targetQa000 from
# ORDER-NEXT: _keepQa000 from
# ORDER-NOT: _targetPa001

## With -init_offsets the __mod_init_func inputs go to the synthetic section
## instead of inputSections, so they must not appear among the gathered section
## roots; markLive() reaches them through in.initOffsets->inputs() as before.
# RUN: %lld -lSystem -dead_strip -no_uuid -init_offsets --threads=1 \
# RUN:   %t/a.o %t/b.o -o %t/io1/out
# RUN: %lld -lSystem -dead_strip -no_uuid -init_offsets --threads=8 \
# RUN:   %t/a.o %t/b.o -o %t/io8/out
# RUN: cmp %t/io1/out %t/io8/out
# RUN: llvm-objdump --syms %t/io8/out | FileCheck %s --check-prefix=INITOFF
# INITOFF-DAG: _initFna
# INITOFF-DAG: _initFnb
# INITOFF-DAG: _termFna
# INITOFF-DAG: _termFnb
