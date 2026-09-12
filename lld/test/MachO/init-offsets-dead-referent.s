# REQUIRES: x86
## Split out of parallel-relocation-scan-events.s because the diagnostic comes
## from fatal(): the LLD_IN_TEST=2 harness suppresses the output of every
## iteration but the last, and a fatal() leaves the linker context undestroyed,
## so the harness exits after the first iteration with its output discarded.
# UNSUPPORTED: main-run-twice

## A relocation whose referent is a dead __mod_init_func symbol is one of the
## scan-relocations replay events that keeps the original serial path, so its
## diagnostic must be identical at every thread count.

# RUN: rm -rf %t; mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %s -o %t/deadref.o
# RUN: not %lld -lSystem -init_offsets --threads=1 %t/deadref.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DEADREF
# RUN: not %lld -lSystem -init_offsets --threads=8 %t/deadref.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DEADREF

# DEADREF: error: {{.*}}deadref.o:(symbol _main+0x3): cannot reference _init_slot defined in __mod_init_func when -init_offsets is used

.globl _main
.text
_main:
  leaq _init_slot(%rip), %rax
  retq

.section __DATA,__mod_init_func,mod_init_funcs
_init_slot:
  .quad _main
