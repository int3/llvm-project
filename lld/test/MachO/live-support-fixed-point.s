# REQUIRES: x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/weak.s -o %t/weak.o
# RUN: %lld -dead_strip --threads=1 %t/main.o %t/weak.o -o %t/out
# RUN: cp %t/out %t/reference
# RUN: llvm-nm %t/out | FileCheck %s --implicit-check-not=_unref
# RUN: llvm-objdump --full-contents --section=__literals %t/out | FileCheck %s --check-prefix=WORDS
# RUN: llvm-objdump --macho --section=__TEXT,__cstring %t/out | FileCheck %s --check-prefix=STRINGS
# RUN: %lld -dead_strip --threads=2 %t/main.o %t/weak.o -o %t/out
# RUN: cmp %t/reference %t/out
# RUN: %lld -dead_strip --threads=8 %t/main.o %t/weak.o -o %t/out
# RUN: cmp %t/reference %t/out
# RUN: %lld -dead_strip --threads=16 %t/main.o %t/weak.o -o %t/out
# RUN: cmp %t/reference %t/out
## The diagnostic path still scans inputSections and acts as a serial oracle.
# RUN: %lld -dead_strip --threads=1 -why_live '_*' %t/main.o %t/weak.o -o %t/out > %t/why1
# RUN: cmp %t/reference %t/out
# RUN: %lld -dead_strip --threads=2 -why_live '_*' %t/main.o %t/weak.o -o %t/out > %t/why2
# RUN: cmp %t/reference %t/out
# RUN: cmp %t/why1 %t/why2
# RUN: %lld -dead_strip --threads=8 -why_live '_*' %t/main.o %t/weak.o -o %t/out > %t/why8
# RUN: cmp %t/reference %t/out
# RUN: cmp %t/why1 %t/why8
# RUN: %lld -dead_strip --threads=16 -why_live '_*' %t/main.o %t/weak.o -o %t/out > %t/why16
# RUN: cmp %t/reference %t/out
# RUN: cmp %t/why1 %t/why16
## Exercise library re-entry in the ordinary suite as well as when the complete
## test is run with --param RUN_LLD_MAIN_TWICE=True. Cleanup must drop candidates
## before the second link's arena allocation can reuse their addresses.
# RUN: env LLD_IN_TEST=2 %lld -dead_strip --threads=8 %t/main.o %t/weak.o -o %t/out
# RUN: cmp %t/reference %t/out
## No-dead-strip links must not depend on the candidate list.
# RUN: %lld --threads=8 %t/main.o %t/weak.o -o %t/out
# RUN: llvm-nm %t/out | FileCheck %s --check-prefix=NODEAD

# CHECK-DAG: _main
# CHECK-DAG: _root
# CHECK-DAG: _alias
# CHECK-DAG: _common
# CHECK-DAG: _round1
# CHECK-DAG: _round2
# CHECK-DAG: _round3
# CHECK-DAG: _round4
# CHECK-DAG: _after
# CHECK-DAG: _cycle_a
# CHECK-DAG: _cycle_b
# CHECK-DAG: _multiple
# CHECK-DAG: _late_target_a
# CHECK-DAG: _late_target_b
# CHECK-DAG: _word_support
# CHECK-DAG: _string_support
# CHECK-DAG: _common_support
# CHECK-DAG: _weak
# NODEAD-DAG: _unref_cycle_a
# NODEAD-DAG: _unref_cycle_b
# NODEAD-DAG: _unref_word_support
# NODEAD-DAG: _unref_string_support
# WORDS-NOT: 11111111
# WORDS: 22222222 22222222
# WORDS-NOT: 11111111
# STRINGS-NOT: dead
# STRINGS: live
# STRINGS-NOT: dead

#--- main.s
## Reverse dependency order needs four fixed-point passes. These precede their
## targets in inputSections; _after follows its target in the other direction.
.section __DATA,__before,regular,live_support
.globl _round1, _round2, _round3, _round4
_round4: .quad _round3
_round3: .quad _round2
_round2: .quad _round1
_round1: .quad _alias
_unref_cycle_a: .quad _unref_cycle_b
_unref_cycle_b: .quad _unref_cycle_a
_cycle_a: .quad _cycle_b, _root
_cycle_b: .quad _cycle_a
## The live target is in the middle in either relocation iteration direction.
## Both other referents must become live when the new support section drains.
_multiple: .quad _late_target_a, _root, _late_target_b
_word_support: .quad Lword_live
_unref_word_support: .quad Lwords
_string_support: .quad Lstring_live
_unref_string_support: .quad Lstrings
_common_support: .quad _common
.globl _weak
.weak_definition _weak
_weak: .quad _root

.text
.globl _main, _root, _alias
_main:
  leaq _root(%rip), %rax
  leaq _common(%rip), %rax
  leaq Lword_live(%rip), %rax
  leaq Lstring_live(%rip), %rax
  retq
_root: retq
_alias = _root
_late_target_a: retq
_late_target_b: retq
.comm _common,8,3

.section __DATA,__after,regular,live_support
_after: .quad _root

.section __TEXT,__literal8,8byte_literals
Lwords:
.quad 0x1111111111111111
Lword_live:
.quad 0x2222222222222222
.section __TEXT,__cstring,cstring_literals
Lstrings:
.asciz "dead"
Lstring_live:
.asciz "live"
.subsections_via_symbols

#--- weak.s
## The losing weak subsection has a live referent, but must never enter the
## support list or keep its otherwise unreferenced outgoing target alive.
.section __DATA,__before,regular,live_support
.globl _weak
.weak_definition _weak
_weak: .quad _root, _unref_weak_target
.text
_unref_weak_target: retq
.subsections_via_symbols
