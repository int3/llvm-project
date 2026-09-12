# REQUIRES: x86 && thread_support

## Autolinked TBDs are prepared by the TAPI pipeline from the object parse that
## saw the LC_LINKER_OPTION, and their re-exports are prepared too. Resolution,
## diagnostics and publication still happen in resolveLCLinkerOptions()' sorted
## order, at every thread count.

# RUN: rm -rf %t; split-file %s %t --no-leading-lines
# RUN: mkdir -p %t/Alpha.framework %t/Beta.framework %t/Gamma.framework
# RUN: cp %t/Alpha.tbd %t/Alpha.framework/Alpha.tbd
# RUN: cp %t/Beta.tbd %t/Beta.framework/Beta.tbd
# RUN: cp %t/Gamma.tbd %t/Gamma.framework/Gamma.tbd
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/one.s -o %t/one.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/two.s -o %t/two.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/three.s -o %t/three.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/missing.s -o %t/missing.o

## Several objects autolink three frameworks, each of which re-exports a
## library, plus one plain library. Names repeat across the objects, so the
## collection-time dedupe is exercised as well.
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 \
# RUN:   %t/one.o %t/two.o %t/three.o -o %t/auto1
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=2 \
# RUN:   %t/one.o %t/two.o %t/three.o -o %t/auto2
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 \
# RUN:   %t/one.o %t/two.o %t/three.o -o %t/auto8
# RUN: cmp %t/auto1 %t/auto2
# RUN: cmp %t/auto1 %t/auto8

## The re-exported leaves the pipeline followed supply the symbols the objects
## reference, and each dylib is recorded exactly once even though two objects
## autolink Alpha, two autolink Beta and two autolink -lPlain.
# RUN: llvm-otool -L %t/auto8 | FileCheck %s --check-prefix=LOAD
# LOAD-COUNT-1: /usr/lib/libAlphaLeaf.dylib
# LOAD-COUNT-1: /usr/lib/libBetaLeaf.dylib
# LOAD-COUNT-1: /System/Library/Frameworks/Gamma.framework/Gamma
# LOAD-COUNT-1: /usr/lib/libPlain.dylib
# LOAD-NOT: libAlphaLeaf
# LOAD-NOT: libBetaLeaf
# LOAD-NOT: Gamma
# LOAD-NOT: libPlain

## A name that is autolinked and also given on the command line: createFiles()
## consumes the prepared entry for the explicit -framework Alpha, so the round
## that resolveLCLinkerOptions() runs for the objects finds Alpha already in
## loadedDylibs and short-circuits. Same bytes at one and eight threads.
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=1 \
# RUN:   -framework Alpha %t/one.o %t/two.o %t/three.o -o %t/both1
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 -F%t -L%t --threads=8 \
# RUN:   -framework Alpha %t/one.o %t/two.o %t/three.o -o %t/both8
# RUN: cmp %t/both1 %t/both8
# RUN: llvm-otool -L %t/both8 | FileCheck %s --check-prefix=BOTH
# BOTH-COUNT-1: /System/Library/Frameworks/Alpha.framework/Alpha
# BOTH-NOT: Alpha.framework

## A missing autolinked framework and a missing autolinked library are reported
## in resolveLCLinkerOptions()' order (frameworks before libraries, each sorted)
## no matter which worker parsed the object that named them, and no matter in
## which order the object named them.
# RUN: not %lld -arch x86_64 -dylib -install_name /Missing -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 --strict-auto-link -F%t -L%t \
# RUN:   --threads=1 %t/missing.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING
# RUN: not %lld -arch x86_64 -dylib -install_name /Missing -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 --strict-auto-link -F%t -L%t \
# RUN:   --threads=2 %t/missing.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING
# RUN: not %lld -arch x86_64 -dylib -install_name /Missing -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 --strict-auto-link -F%t -L%t \
# RUN:   --threads=8 %t/missing.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING

# MISSING:      auto-linked framework not found for -framework AbsentA
# MISSING-NEXT: auto-linked framework not found for -framework AbsentB
# MISSING-NEXT: auto-linked library not found for -labsent

#--- one.s
.linker_option "-framework", "Alpha"
.linker_option "-framework", "Beta"
.globl _one
_one:
  callq _alphaLeaf
  retq

#--- two.s
.linker_option "-framework", "Beta"
.linker_option "-framework", "Gamma"
.linker_option "-lPlain"
.globl _two
_two:
  callq _betaLeaf
  callq _gamma
  retq

#--- three.s
.linker_option "-framework", "Alpha"
.linker_option "-lPlain"
.globl _three
_three:
  callq _plain
  retq

#--- missing.s
.linker_option "-framework", "AbsentB"
.linker_option "-labsent"
.linker_option "-framework", "AbsentA"
.globl _missing
_missing:
  retq

#--- Alpha.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/System/Library/Frameworks/Alpha.framework/Alpha'
reexported-libraries:
  - targets:      [ x86_64-macos ]
    libraries:    [ '/usr/lib/libAlphaLeaf.dylib' ]
...

#--- Beta.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/System/Library/Frameworks/Beta.framework/Beta'
reexported-libraries:
  - targets:      [ x86_64-macos ]
    libraries:    [ '/usr/lib/libBetaLeaf.dylib' ]
...

#--- Gamma.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/System/Library/Frameworks/Gamma.framework/Gamma'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _gamma ]
...

#--- libAlphaLeaf.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libAlphaLeaf.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _alphaLeaf ]
...

#--- libBetaLeaf.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libBetaLeaf.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _betaLeaf ]
...

#--- libPlain.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libPlain.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _plain ]
...
