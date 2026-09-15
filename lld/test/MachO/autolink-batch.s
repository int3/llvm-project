# REQUIRES: x86 && thread_support

## An autolink round's prepared dylib names are published as one batch, replayed
## across the symbol table's shards at the run boundary instead of inserted on
## the ordered thread. Every case below checks that the batched path (--threads
## 2 and 8) produces exactly what the inline path (--threads=1) produces, and
## checks the resolution rule the batch has to reproduce.

# RUN: rm -rf %t; split-file %s %t --no-leading-lines
# RUN: mkdir -p \
# RUN:   %t/Adup.framework \
# RUN:   %t/Bdup.framework \
# RUN:   %t/Awk.framework \
# RUN:   %t/Bwk.framework \
# RUN:   %t/Astr.framework \
# RUN:   %t/Bstr.framework \
# RUN:   %t/Wref.framework \
# RUN:   %t/Lzf.framework \
# RUN:   %t/Hid.framework \
# RUN:   %t/Aprev.framework \
# RUN:   %t/Pprev.framework \
# RUN:   %t/Bad.framework \
# RUN:   %t/Keep.framework \
# RUN:   %t/Drop.framework
# RUN: cp %t/Adup.tbd %t/Adup.framework/Adup.tbd
# RUN: cp %t/Bdup.tbd %t/Bdup.framework/Bdup.tbd
# RUN: cp %t/Awk.tbd %t/Awk.framework/Awk.tbd
# RUN: cp %t/Bwk.tbd %t/Bwk.framework/Bwk.tbd
# RUN: cp %t/Astr.tbd %t/Astr.framework/Astr.tbd
# RUN: cp %t/Bstr.tbd %t/Bstr.framework/Bstr.tbd
# RUN: cp %t/Wref.tbd %t/Wref.framework/Wref.tbd
# RUN: cp %t/Lzf.tbd %t/Lzf.framework/Lzf.tbd
# RUN: cp %t/Hid.tbd %t/Hid.framework/Hid.tbd
# RUN: cp %t/Aprev.tbd %t/Aprev.framework/Aprev.tbd
# RUN: cp %t/Pprev.tbd %t/Pprev.framework/Pprev.tbd
# RUN: cp %t/Bad.tbd %t/Bad.framework/Bad.tbd
# RUN: cp %t/Keep.tbd %t/Keep.framework/Keep.tbd
# RUN: cp %t/Drop.tbd %t/Drop.framework/Drop.tbd
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dup.s -o %t/dup.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wk.s -o %t/wk.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/str.s -o %t/str.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/wref.s -o %t/wref.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/lazy.s -o %t/lazy.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/member.s -o %t/member.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/boundary.s -o %t/boundary.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/shared.s -o %t/shared.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/hide.s -o %t/hide.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/prev.s -o %t/prev.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/bad.s -o %t/bad.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/strip.s -o %t/strip.o
# RUN: llvm-ar rcs %t/libLazy.a %t/member.o
# RUN: llvm-ar rcs %t/libBtwo.a %t/shared.o

## Case 1: two autolinked TBDs export the same name. The first one in the
## round's sorted order wins, at every thread count.
# RUN: %lld -arch x86_64 -dylib -install_name /Dup -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/dup.o -o %t/dup1
# RUN: %lld -arch x86_64 -dylib -install_name /Dup -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=2 %t/dup.o -o %t/dup2
# RUN: %lld -arch x86_64 -dylib -install_name /Dup -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/dup.o -o %t/dup8
# RUN: cmp %t/dup1 %t/dup2
# RUN: cmp %t/dup1 %t/dup8
# RUN: llvm-objdump --macho --bind %t/dup8 | FileCheck %s --check-prefix=DUP
# DUP: Adup  _dup

## Case 2: weak definition then strong definition -- the strong one replaces it.
# RUN: %lld -arch x86_64 -dylib -install_name /Wk -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/wk.o -o %t/wk1
# RUN: %lld -arch x86_64 -dylib -install_name /Wk -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/wk.o -o %t/wk8
# RUN: cmp %t/wk1 %t/wk8
# RUN: llvm-objdump --macho --bind %t/wk8 | FileCheck %s --check-prefix=WK
# WK: Bwk  _wk

## Case 3: strong definition then weak definition -- the strong one keeps it.
# RUN: %lld -arch x86_64 -dylib -install_name /Str -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/str.o -o %t/str1
# RUN: %lld -arch x86_64 -dylib -install_name /Str -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/str.o -o %t/str8
# RUN: cmp %t/str1 %t/str8
# RUN: llvm-objdump --macho --bind %t/str8 | FileCheck %s --check-prefix=STR
# STR: Astr  _str

## Case 4: an Undefined with a weak reference hands its RefState to the dylib
## symbol, so -dead_strip_dylibs keeps the framework that satisfies it.
# RUN: %lld -arch x86_64 -dylib -install_name /Wref -no_uuid -dead_strip_dylibs \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/wref.o -o %t/wref1
# RUN: %lld -arch x86_64 -dylib -install_name /Wref -no_uuid -dead_strip_dylibs \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/wref.o -o %t/wref8
# RUN: cmp %t/wref1 %t/wref8
# RUN: llvm-otool -L %t/wref8 | FileCheck %s --check-prefix=WREF
# WREF: /Wref.framework/Wref

## Case 5: a lazy archive symbol is left alone by a later dylib export, so the
## member is not fetched.
# RUN: %lld -arch x86_64 -dylib -install_name /Lz -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/lazy.o %t/libLazy.a -o %t/lz1
# RUN: %lld -arch x86_64 -dylib -install_name /Lz -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/lazy.o %t/libLazy.a -o %t/lz8
# RUN: cmp %t/lz1 %t/lz8
# RUN: llvm-nm %t/lz8 | FileCheck %s --check-prefix=LZ
# LZ-NOT: _memberMarker

## Case 6: an autolinked archive between two autolinked TBDs. The TBD sorted
## before it has already published _shared when the archive's index is read, so
## the member is never fetched; a batch carried across the archive would fetch
## it.
# RUN: %lld -arch x86_64 -dylib -install_name /Bd -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/boundary.o -o %t/bd1
# RUN: %lld -arch x86_64 -dylib -install_name /Bd -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/boundary.o -o %t/bd8
# RUN: cmp %t/bd1 %t/bd8
# RUN: llvm-objdump --macho --bind %t/bd8 | FileCheck %s --check-prefix=BD
# RUN: llvm-nm %t/bd8 | FileCheck %s --check-prefix=BDNM
# BD: libAone  _shared
# BDNM-NOT: _memberShared

## Case 7: a re-export child's $ld$hide of a name the umbrella already
## published. The umbrella's own _hidden stays; the child's is dropped.
# RUN: %lld -arch x86_64 -dylib -install_name /Hid -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/hide.o -o %t/hide1
# RUN: %lld -arch x86_64 -dylib -install_name /Hid -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/hide.o -o %t/hide8
# RUN: cmp %t/hide1 %t/hide8
# RUN: llvm-objdump --macho --bind %t/hide8 | FileCheck %s --check-prefix=HIDE
# HIDE-DAG: Hid  _hidden
# HIDE-DAG: Hid  _leaf

## Case 8: a $ld$previous symbol colliding with an earlier autolinked file's
## export. The earlier export owns the name, so the synthetic dylib does not
## take it and is not loaded.
# RUN: %lld -arch x86_64 -dylib -install_name /Prev -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/prev.o -o %t/prev1
# RUN: %lld -arch x86_64 -dylib -install_name /Prev -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/prev.o -o %t/prev8
# RUN: cmp %t/prev1 %t/prev8
# RUN: llvm-objdump --macho --bind %t/prev8 | FileCheck %s --check-prefix=PREV
# PREV: Aprev  _prevsym

## Case 9: an incompatible-platform TBD in the middle of the batch. Its names
## are dropped and the error is emitted at its own position, before the later
## framework's, at every thread count.
# RUN: not %lld -arch x86_64 -dylib -install_name /Bad -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 %t/bad.o \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: not %lld -arch x86_64 -dylib -install_name /Bad -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 %t/bad.o \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=BAD
# BAD:      Bad.framework/Bad) is incompatible with x86_64 (macOS11.0)
# BAD-NEXT: undefined symbol: _bad

## Case 10: -t prints the round's files in the ordered walk's sorted order, and
## -dead_strip_dylibs prunes the framework nothing references. Both depend on
## the per-file reference counts the batch updates from several shards.
# RUN: %lld -arch x86_64 -dylib -install_name /Strip -no_uuid -dead_strip_dylibs \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 -t %t/strip.o \
# RUN:   -o %t/strip1 | FileCheck %s --check-prefix=TORDER
# RUN: %lld -arch x86_64 -dylib -install_name /Strip -no_uuid -dead_strip_dylibs \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 -t %t/strip.o \
# RUN:   -o %t/strip8 | FileCheck %s --check-prefix=TORDER
# RUN: cmp %t/strip1 %t/strip8
# TORDER:      strip.o
# TORDER-NEXT: Drop.framework/Drop
# TORDER-NEXT: Keep.framework/Keep
# RUN: llvm-otool -L %t/strip8 | FileCheck %s --check-prefix=STRIP
# STRIP: /Keep.framework/Keep
# STRIP-NOT: /Drop.framework/Drop

#--- dup.s
.linker_option "-framework", "Bdup"
.linker_option "-framework", "Adup"
.globl _dupmain
_dupmain:
  retq
.data
.quad _dup

#--- wk.s
.linker_option "-framework", "Awk"
.linker_option "-framework", "Bwk"
.globl _wkmain
_wkmain:
  retq
.data
.quad _wk

#--- str.s
.linker_option "-framework", "Astr"
.linker_option "-framework", "Bstr"
.globl _strmain
_strmain:
  retq
.data
.quad _str

#--- wref.s
.linker_option "-framework", "Wref"
.weak_reference _wref
.globl _wrefmain
_wrefmain:
  retq
.data
.quad _wref

#--- lazy.s
.linker_option "-framework", "Lzf"
.globl _lzmain
_lzmain:
  retq

#--- member.s
.globl _lz, _memberMarker
_lz:
  retq
_memberMarker:
  retq

#--- boundary.s
.linker_option "-lAone"
.linker_option "-lBtwo"
.linker_option "-lCthree"
.globl _bdmain
_bdmain:
  retq
.data
.quad _shared
.quad _cthree

#--- shared.s
.globl _shared, _memberShared
_shared:
  retq
_memberShared:
  retq

#--- hide.s
.linker_option "-framework", "Hid"
.globl _hidemain
_hidemain:
  retq
.data
.quad _hidden
.quad _leaf

#--- prev.s
.linker_option "-framework", "Aprev"
.linker_option "-framework", "Pprev"
.globl _prevmain
_prevmain:
  retq
.data
.quad _prevsym
.quad _pprev

#--- bad.s
.linker_option "-framework", "Bad"
.globl _badmain
_badmain:
  retq
.data
.quad _bad

#--- strip.s
.linker_option "-framework", "Drop"
.linker_option "-framework", "Keep"
.globl _stripmain
_stripmain:
  retq
.data
.quad _keep

#--- Adup.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Adup.framework/Adup'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _dup ]
...

#--- Bdup.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Bdup.framework/Bdup'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _dup ]
...

#--- Awk.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Awk.framework/Awk'
exports:
  - targets:      [ x86_64-macos ]
    weak-symbols: [ _wk ]
...

#--- Bwk.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Bwk.framework/Bwk'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _wk ]
...

#--- Astr.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Astr.framework/Astr'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _str ]
...

#--- Bstr.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Bstr.framework/Bstr'
exports:
  - targets:      [ x86_64-macos ]
    weak-symbols: [ _str ]
...

#--- Wref.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Wref.framework/Wref'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _wref ]
...

#--- Lzf.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Lzf.framework/Lzf'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _lz ]
...

#--- libAone.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libAone.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _shared ]
...

#--- libCthree.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libCthree.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _cthree, _shared ]
...

#--- Hid.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Hid.framework/Hid'
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

#--- Aprev.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Aprev.framework/Aprev'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _prevsym ]
...

#--- Pprev.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Pprev.framework/Pprev'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ '$ld$previous$/usr/lib/libPrev.dylib$$1$3.0$14.0$_prevsym$',
                    _pprev ]
...

#--- Bad.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-ios ]
install-name:     '/Bad.framework/Bad'
exports:
  - targets:      [ x86_64-ios ]
    symbols:      [ _bad ]
...

#--- Keep.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Keep.framework/Keep'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _keep ]
...

#--- Drop.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/Drop.framework/Drop'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _drop ]
...
