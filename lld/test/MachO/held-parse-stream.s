# REQUIRES: x86 && thread_support
## The held object files are parsed on workers in chunks spawned behind the
## argument walk, so their folds run while the main thread is still publishing
## the dylibs and TBDs that follow them on the command line. The dylib exports
## names the objects define strongly, weakly, as commons, and leave undefined
## (strongly and weakly), and one name is left to -undefined dynamic_lookup.
## Whichever side reaches a slot first, the output, the map and -why_load must
## be the serial linker's. LLD_HELD_PARSE_CHUNK=1 makes every object its own
## task, and LLD_HELD_PARSE_DELAY_US holds each parse back so that dylib
## publication lands in the middle of the folds.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/refs.s -o %t/refs.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/strong.s -o %t/strong.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/weak.s -o %t/weak.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/common.s -o %t/common.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/undef.s -o %t/undef.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/more.s -o %t/more.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/autolink.s -o %t/autolink.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/mach.s -o %t/mach.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/member.s -o %t/member.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/auto.s -o %t/auto.o
# RUN: %lld -dylib -install_name /usr/lib/libMach.dylib %t/mach.o -o %t/libMach.dylib
# RUN: %lld -dylib -install_name /usr/lib/libAuto.dylib %t/auto.o -o %t/lib/libAuto.dylib
# RUN: llvm-ar --format=darwin rcs %t/libArch.a %t/member.o

## Objects first, then the TBD and the Mach-O dylib, then an archive, as in
## every corpus.
# RUN: %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=1 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: mv %t/out %t/out.ref && mv %t/map %t/map.ref && mv %t/why %t/why.ref
# RUN: FileCheck %s --check-prefix=WHY < %t/why.ref
# RUN: llvm-nm -m %t/out.ref | FileCheck %s --check-prefix=NM

# RUN: %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=2 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=1 %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=2 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=2 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=2 LLD_HELD_PARSE_DELAY_US=5000 %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: env LLD_NO_STREAM_HELD_PARSE=1 %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why
# RUN: env LLD_REVERSE_HELD_PARSE=1 %lld -dylib -install_name /Stream -undefined dynamic_lookup -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/out.ref %t/out && cmp %t/map.ref %t/map && cmp %t/why.ref %t/why

# WHY: _member forced load of {{.*}}libArch.a(member.o)

## The strong, weak and common definitions prevail over both dylibs; the
## undefined names bind to the first dylib that exports them; _dl is left to
## dynamic lookup.
# NM-DAG: (__TEXT,__text) external _s1
# NM-DAG: (__TEXT,__text) weak external _w1
# NM-DAG: (__DATA,__common) external _c1
# NM-DAG: (undefined) external _u1 (from libTbd)
# NM-DAG: (undefined) weak external _wr (from libTbd)
# NM-DAG: (undefined) external _u2 (from libMach)
# NM-DAG: (undefined) external _dl (dynamically looked up)

## While the parse streams, the command-line dylibs' names are batched and
## published after the parse is joined. The batch must reproduce the inline
## order: a $ld$hide in a re-exported child of a name its umbrella already
## published (the umbrella's stays), a $ld$hide in a TBD of a name a Mach-O
## dylib also exports (in both file orders), a $ld$previous symbol, and a
## Mach-O dylib's LC_REEXPORT_DYLIB.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/refs2.s -o %t/refs2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/inner.s -o %t/inner.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/outer.s -o %t/outer.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/h1.s -o %t/h1.o
# RUN: %lld -dylib -install_name %t/libInner.dylib %t/inner.o -o %t/libInner.dylib
# RUN: %lld -dylib -install_name /usr/lib/libOuter.dylib %t/outer.o -reexport_library %t/libInner.dylib -o %t/libOuter.dylib
# RUN: %lld -dylib -install_name /usr/lib/libH1.dylib %t/h1.o -o %t/libH1.dylib
# RUN: %lld -dylib -install_name /Batch -o %t/out --threads=1 -map %t/map -why_load -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libHideH1.tbd %t/libH1.dylib %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: mv %t/out %t/b.ref && mv %t/map %t/bmap.ref && mv %t/why %t/bwhy.ref
# RUN: llvm-objdump --macho --bind %t/b.ref | FileCheck %s --check-prefix=BIND
# RUN: %lld -dylib -install_name /Batch -o %t/out --threads=8 -map %t/map -why_load -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libHideH1.tbd %t/libH1.dylib %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/b.ref %t/out && cmp %t/bmap.ref %t/map && cmp %t/bwhy.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=1 %lld -dylib -install_name /Batch -o %t/out --threads=2 -map %t/map -why_load -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libHideH1.tbd %t/libH1.dylib %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/b.ref %t/out && cmp %t/bmap.ref %t/map && cmp %t/bwhy.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Batch -o %t/out --threads=8 -map %t/map -why_load -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libHideH1.tbd %t/libH1.dylib %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/b.ref %t/out && cmp %t/bmap.ref %t/map && cmp %t/bwhy.ref %t/why
# RUN: env LLD_NO_STREAM_HELD_PARSE=1 %lld -dylib -install_name /Batch -o %t/out --threads=8 -map %t/map -why_load -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libHideH1.tbd %t/libH1.dylib %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/b.ref %t/out && cmp %t/bmap.ref %t/map && cmp %t/bwhy.ref %t/why

## The other file order for the hidden _h1: the Mach-O dylib first.
# RUN: %lld -dylib -install_name /Batch -o %t/out --threads=1 -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libH1.dylib %t/libHideH1.tbd %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a
# RUN: mv %t/out %t/c.ref
# RUN: llvm-objdump --macho --bind %t/c.ref | FileCheck %s --check-prefix=BIND
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Batch -o %t/out --threads=8 -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libH1.dylib %t/libHideH1.tbd %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a
# RUN: cmp %t/c.ref %t/out
# RUN: %lld -dylib -install_name /Batch -o %t/out --threads=2 -L%t \
# RUN:   %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libH1.dylib %t/libHideH1.tbd %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a
# RUN: cmp %t/c.ref %t/out

# BIND-DAG: libHidU{{ +}}_hidden
# BIND-DAG: libHidU{{ +}}_leaf
# BIND-DAG: libPrevOld{{ +}}_prevsym
# BIND-DAG: libH1{{ +}}_h1
# BIND-DAG: {{libOuter|libInner}}{{ +}}_inner

## A TBD the pipeline did not prepare (--reproduce turns the pipeline off) is
## prepared on the main thread and defers its names like a prepared one, so the
## streamed parse is not drained: the held object after it keeps streaming, and
## the batch (the Mach-O dylib before it and the TBD) is published at the join.
# RUN: %lld -dylib -install_name /Drain -undefined dynamic_lookup -o %t/out --threads=1 -map %t/map -why_load \
# RUN:   %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/libMach.dylib %t/libTbd.tbd %t/more.o %t/libArch.a > %t/why
# RUN: mv %t/out %t/d.ref && mv %t/map %t/dmap.ref && mv %t/why %t/dwhy.ref
# RUN: rm -f %t/drain.tar
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Drain -undefined dynamic_lookup -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   --time-trace --time-trace-granularity=0 --time-trace=%t/drain.json --reproduce %t/drain.tar %t/refs.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/libMach.dylib %t/libTbd.tbd %t/more.o %t/libArch.a > %t/why
# RUN: cmp %t/d.ref %t/out && cmp %t/dmap.ref %t/map && cmp %t/dwhy.ref %t/why
# RUN: FileCheck %s --check-prefix=DRAIN < %t/drain.json
# DRAIN-NOT: Drain streamed held parse
# DRAIN: Prepare TBD symbols on the walk
# DRAIN-NOT: Drain streamed held parse

## The same for TBDs carrying $ld$hide (in a re-exported child, and of a name a
## Mach-O dylib also exports) and $ld$previous: unprepared under --reproduce,
## they go through the batch with the same hidden-name stamps and the same
## synthetic dylib as the pipeline-prepared files above.
# RUN: rm -f %t/batch.tar
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Batch -o %t/out --threads=8 -map %t/map -why_load -L%t \
# RUN:   --time-trace --time-trace-granularity=0 --time-trace=%t/batch.json \
# RUN:   --reproduce %t/batch.tar %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libHideH1.tbd %t/libH1.dylib %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/b.ref %t/out && cmp %t/bmap.ref %t/map && cmp %t/bwhy.ref %t/why
# RUN: FileCheck %s --check-prefix=LIVE < %t/batch.json
# LIVE-NOT: Drain streamed held parse
# LIVE: Prepare TBD symbols on the walk
# LIVE-NOT: Drain streamed held parse
# RUN: rm -f %t/batch.tar
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Batch -o %t/out --threads=8 -L%t \
# RUN:   --reproduce %t/batch.tar %t/refs2.o %t/strong.o %t/weak.o %t/common.o %t/more.o %t/libHidU.tbd %t/libPrev.tbd %t/libH1.dylib %t/libHideH1.tbd %t/libOuter.dylib %t/libTbd.tbd %t/libMach.dylib %t/libArch.a
# RUN: cmp %t/c.ref %t/out

## An object with an LC_LINKER_OPTION among the held objects: the walker
## declines and the archive publishes its index the ordinary way, after the
## streamed parse has been joined.
# RUN: %lld -dylib -install_name /Stream -undefined dynamic_lookup -L%t/lib -o %t/out --threads=1 -map %t/map -why_load \
# RUN:   %t/refs.o %t/autolink.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: mv %t/out %t/lco.ref && mv %t/map %t/lcomap.ref && mv %t/why %t/lcowhy.ref
# RUN: FileCheck %s --check-prefix=WHY < %t/lcowhy.ref
# RUN: llvm-otool -L %t/lco.ref | FileCheck %s --check-prefix=LCO
# RUN: %lld -dylib -install_name /Stream -undefined dynamic_lookup -L%t/lib -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/autolink.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/lco.ref %t/out && cmp %t/lcomap.ref %t/map && cmp %t/lcowhy.ref %t/why
# RUN: env LLD_HELD_PARSE_CHUNK=1 LLD_HELD_PARSE_DELAY_US=20000 %lld -dylib -install_name /Stream -undefined dynamic_lookup -L%t/lib -o %t/out --threads=8 -map %t/map -why_load \
# RUN:   %t/refs.o %t/autolink.o %t/strong.o %t/weak.o %t/common.o %t/undef.o %t/more.o %t/libTbd.tbd %t/libMach.dylib %t/libArch.a > %t/why
# RUN: cmp %t/lco.ref %t/out && cmp %t/lcomap.ref %t/map && cmp %t/lcowhy.ref %t/why

# LCO: /usr/lib/libAuto.dylib

#--- refs.s
.globl _root
.text
_root:
  retq
.data
  .quad _s1
  .quad _w1
  .quad _c1
  .quad _u1
  .quad _u2
  .quad _dl
  .quad _member

#--- strong.s
.globl _s1
.text
_s1:
  retq

#--- weak.s
.globl _w1
.weak_definition _w1
.text
_w1:
  retq

#--- common.s
.comm _c1, 8, 3

#--- undef.s
.weak_reference _wr
.data
  .quad _u1
  .quad _wr
  .quad _s1

#--- more.s
.data
  .quad _u2
  .quad _w1
  .quad _c1

#--- autolink.s
.linker_option "-lAuto"
.data
  .quad _auto

#--- auto.s
.globl _auto
.text
_auto:
  retq

#--- member.s
.globl _member
.text
_member:
  retq

#--- mach.s
.globl _s1, _w1, _c1, _u1, _u2
.weak_definition _w1
.text
_s1:
  retq
_w1:
  retq
_c1:
  retq
_u1:
  retq
_u2:
  retq

#--- libTbd.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libTbd.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _s1, _c1, _u1, _wr ]
    weak-def-symbols: [ _w1 ]
...

#--- refs2.s
.globl _root2
.text
_root2:
  retq
.data
  .quad _s1
  .quad _w1
  .quad _c1
  .quad _u1
  .quad _hidden
  .quad _leaf
  .quad _prevsym
  .quad _h1
  .quad _inner
  .quad _member

#--- inner.s
.globl _inner
.text
_inner:
  retq

#--- outer.s
.globl _outer
.text
_outer:
  retq

#--- h1.s
.globl _h1
.text
_h1:
  retq

#--- libHidU.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libHidU.dylib'
reexported-libraries:
  - targets:      [ x86_64-macos ]
    libraries:    [ '/other/libHidLeaf.dylib' ]
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _hidden ]
...

#--- libHidLeaf.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/other/libHidLeaf.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ '$ld$hide$os11.0$_hidden', _hidden, _leaf ]
...

#--- libPrev.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libPrev.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ '$ld$previous$/usr/lib/libPrevOld.dylib$$1$3.0$14.0$_prevsym$',
                    _prevsym ]
...

#--- libHideH1.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libHideH1.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ '$ld$hide$os11.0$_h1', _h1 ]
...
