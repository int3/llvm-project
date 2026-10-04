# REQUIRES: x86, aarch64
## The index prepass runs on workers before the walk decides whether it can walk
## at all, and buildIndex() makes the same first-member architecture check that
## addLazySymbols() makes. The incompatible mixed.a makes the prepass decline;
## its warning must not be replayed on top of the serial fallback's warning.
## opt.a also exercises that fallback with an LC_LINKER_OPTION member.

# RUN: rm -rf %t; split-file %s %t
## The two links write the same basename in separate directories: the output
## basename is embedded in the linked bytes, so naming them out1 and out8 would
## make cmp fail for that reason alone.
# RUN: mkdir -p %t/t1 %t/t8
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-macos %t/first.s -o %t/first_arm64.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/second.s -o %t/second.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/opt.s -o %t/opt.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/main.s -o %t/main.o
# RUN: llvm-ar --format=darwin rcs %t/mixed.a %t/first_arm64.o %t/second.o
# RUN: llvm-ar --format=darwin rcs %t/opt.a %t/opt.o

## The warning names the archive once, whatever the thread count.
# RUN: %no-fatal-warnings-lld -arch x86_64 -lSystem -o %t/t1/out --threads=1 %t/main.o %t/mixed.a %t/opt.a 2>&1 \
# RUN:   | FileCheck %s --check-prefix=WARN --implicit-check-not=warning:
# RUN: %no-fatal-warnings-lld -arch x86_64 -lSystem -o %t/t8/out --threads=8 %t/main.o %t/mixed.a %t/opt.a 2>&1 \
# RUN:   | FileCheck %s --check-prefix=WARN --implicit-check-not=warning:
# RUN: cmp %t/t1/out %t/t8/out

# WARN: warning: {{.*}}mixed.a has architecture arm64 which is incompatible with target architecture x86_64

#--- first.s
.globl _unused_arm64
_unused_arm64:
  ret

#--- second.s
.globl _unused_x86
_unused_x86:
  ret

#--- opt.s
.globl _fromArchive
_fromArchive:
  ret
.linker_option "-lSystem"

#--- main.s
.globl _main
_main:
  callq _fromArchive
  ret
