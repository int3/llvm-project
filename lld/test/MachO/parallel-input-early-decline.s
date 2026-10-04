# REQUIRES: x86
## Autolink inputs follow the held phase. Residual index publication preserves
## the slots those inputs observe, so the archives can now be walked.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/opt1 %t/opt2 %t/opt8 %t/tt %t/mirror1 %t/mirror8 %t/mtt
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/plain.s -o %t/plain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/b.s -o %t/b.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/c.s -o %t/c.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/extra.s -o %t/extra.o
# RUN: llvm-ar --format=darwin rcs %t/abc.a %t/a.o %t/c.o %t/b.o
# RUN: %lld -dylib -o %t/libextra.dylib %t/extra.o

## main.o autolinks -lextra, so libextra.dylib loads after the input phase. The
## output and the extraction trace must be identical at one, two and eight
## threads.
# RUN: %lld -lSystem -L%t -o %t/opt1/out --threads=1 -why_load %t/main.o %t/abc.a > %t/opt1/why 2>&1
# RUN: %lld -lSystem -L%t -o %t/opt2/out --threads=2 -why_load %t/main.o %t/abc.a > %t/opt2/why 2>&1
# RUN: %lld -lSystem -L%t -o %t/opt8/out --threads=8 -why_load %t/main.o %t/abc.a > %t/opt8/why 2>&1
# RUN: cmp %t/opt1/out %t/opt2/out
# RUN: cmp %t/opt1/out %t/opt8/out
# RUN: cmp %t/opt1/why %t/opt2/why
# RUN: cmp %t/opt1/why %t/opt8/why
# RUN: FileCheck %s --check-prefix=WHYLOAD --input-file=%t/opt1/why
# RUN: llvm-objdump --macho --dylibs-used %t/opt1/out | FileCheck %s --check-prefix=AUTOLINK

## _a extracts a.o, whose reference to _c extracts c.o, whose definition of _b
## means b.o is never extracted.
# WHYLOAD:      _a forced load of {{.*}}abc.a(a.o)
# WHYLOAD-NEXT: _c forced load of {{.*}}abc.a(c.o)
# WHYLOAD-NOT:  (b.o)

# AUTOLINK: libextra.dylib

## Both the walk and residual publication run; the serial fallback does not.
# RUN: %lld -lSystem -L%t -o %t/tt/out --threads=8 --time-trace=%t/tt/trace.json \
# RUN:     --time-trace-granularity=0 -why_load %t/main.o %t/abc.a > %t/tt/why 2>&1
# RUN: cmp %t/opt1/out %t/tt/out
# RUN: FileCheck %s --check-prefix=RESIDUAL --input-file=%t/tt/trace.json \
# RUN:     --implicit-check-not="Publish archive indexes"
# RESIDUAL-DAG: Publish residual archive indexes
# RESIDUAL-DAG: Walk archives

## The mirror: the same link with no LC_LINKER_OPTION still walks, so the gate
## avoids residual publication. Dylibs after the archive do not close the phase.
# RUN: %lld -lSystem -L%t -o %t/mirror1/out --threads=1 -why_load %t/plain.o %t/abc.a > %t/mirror1/why 2>&1
# RUN: %lld -lSystem -L%t -o %t/mirror8/out --threads=8 -why_load %t/plain.o %t/abc.a > %t/mirror8/why 2>&1
# RUN: cmp %t/mirror1/out %t/mirror8/out
# RUN: cmp %t/mirror1/why %t/mirror8/why
# RUN: %lld -lSystem -L%t -o %t/mtt/out --threads=8 --time-trace=%t/mtt/trace.json \
# RUN:     --time-trace-granularity=0 %t/plain.o %t/abc.a
# RUN: cmp %t/mirror1/out %t/mtt/out
# RUN: FileCheck %s --check-prefix=WALK --input-file=%t/mtt/trace.json \
# RUN:     --implicit-check-not="Publish archive indexes"
# WALK: Walk archives
# WALK-NOT: Publish residual archive indexes

#--- main.s
.linker_option "-lextra"
.globl _main
.text
_main:
  callq _a
  callq _extra
  retq

#--- plain.s
.globl _main
.text
_main:
  callq _a
  retq

#--- a.s
.globl _a
.text
_a:
  callq _c
  retq

#--- b.s
.globl _b
.text
_b:
  retq

#--- c.s
.globl _b, _c
.text
_c:
  retq
_b:
  retq

#--- extra.s
.globl _extra
.text
_extra:
  retq
