# REQUIRES: x86

## Mechanism 20's ordered replay no longer reads isec->relocs: the worker
## resolves each validated event into the exact synthetic-section call the
## replay makes (stub/GOT/TLV addEntry, addNonLazyBindingEntries, a rebase
## push), while events that can diagnose or mutate global state (Undefined
## referents, TLV-attribute mismatches, dead referents) keep the relocation
## index and the original path. Check that entries, bindings with addends,
## rebases, unwind preparation and every diagnostic are unchanged by thread
## count, across the 128-section chunk boundary.

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/foo.s -o %t/foo.o
# RUN: %lld -dylib -lSystem -install_name @rpath/libfoo.dylib -o %t/libfoo.dylib %t/foo.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/main.s -o %t/main.o

## Classic opcode bindings/rebases: dylib bindings carry their addends, the
## weak definition gets a weak binding, and section referents rebase.
# RUN: mkdir -p %t/t1 %t/t2 %t/t8
# RUN: %lld -no_uuid -lSystem %t/libfoo.dylib --threads=1 %t/main.o -o %t/t1/out
# RUN: %lld -no_uuid -lSystem %t/libfoo.dylib --threads=2 %t/main.o -o %t/t2/out
# RUN: %lld -no_uuid -lSystem %t/libfoo.dylib --threads=8 %t/main.o -o %t/t8/out
# RUN: cmp %t/t1/out %t/t2/out
# RUN: cmp %t/t1/out %t/t8/out
# RUN: llvm-objdump --macho --rebase --bind --lazy-bind --weak-bind %t/t1/out | \
# RUN:   FileCheck %s --check-prefix=CLASSIC

## Chained fixups take the addBinding()/addRebase() paths instead.
# RUN: mkdir -p %t/c1 %t/c8
# RUN: %lld -no_uuid -lSystem -fixup_chains %t/libfoo.dylib --threads=1 %t/main.o -o %t/c1/out
# RUN: %lld -no_uuid -lSystem -fixup_chains %t/libfoo.dylib --threads=8 %t/main.o -o %t/c8/out
# RUN: cmp %t/c1/out %t/c8/out
# RUN: llvm-objdump --macho --dyld-info %t/c1/out | FileCheck %s --check-prefix=CHAINED

## The unwind rider: UnwindInfoSection::prepare() walks only the slots flagged
## by addSymbol(), so a mix of functions with and without unwind entries, plus
## two symbols sharing one address where only the second has an entry, must
## produce the same __unwind_info at every thread count.
# RUN: llvm-objdump --macho --unwind-info %t/t1/out | FileCheck %s --check-prefix=UNWIND

## Unvalidated events keep the original path and its diagnostics at any thread
## count: a TLV attribute that disagrees with the symbol...
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/tlvbad.s -o %t/tlvbad.o
# RUN: not %lld -lSystem --threads=1 %t/tlvbad.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=TLVBAD
# RUN: not %lld -lSystem --threads=8 %t/tlvbad.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=TLVBAD

## ...and a dead referent under -init_offsets.
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos11.0 %t/deadref.s -o %t/deadref.o
# RUN: not %lld -lSystem -init_offsets --threads=1 %t/deadref.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DEADREF
# RUN: not %lld -lSystem -init_offsets --threads=8 %t/deadref.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DEADREF

## Ordered, not -DAG: these pin the replay's order and its arguments. The three
## bindings of _gfoo differ only in the addend the event carries.
# CLASSIC:      Rebase table:
## The local _weak definition, the local _main and the L_str section referent.
# CLASSIC:      __DATA   __data             0x100005020  pointer
# CLASSIC-NEXT: __DATA   __data             0x100005028  pointer
# CLASSIC-NEXT: __DATA   __data             0x100005030  pointer
# CLASSIC:      Bind table:
# CLASSIC:      __DATA_CONST __got {{.*}} 0 libfoo {{ *}}_tfoo
# CLASSIC-NEXT: __DATA_CONST __got {{.*}} 0 libfoo {{ *}}_gfoo
# CLASSIC-NEXT: __DATA   __data {{.*}} pointer {{ *}}0 libfoo {{ *}}_gfoo
# CLASSIC-NEXT: __DATA   __data {{.*}} pointer {{ *}}16 libfoo {{ *}}_gfoo
# CLASSIC-NEXT: __DATA   __data {{.*}} pointer {{ *}}32 libfoo {{ *}}_gfoo
# CLASSIC:      Lazy bind table:
# CLASSIC:      __DATA   __la_symbol_ptr {{.*}} _ffoo
# CLASSIC:      Weak bind table:
# CLASSIC:      __DATA   __data {{.*}} _weak

# CHAINED:      dyld information:
# CHAINED:      __DATA_CONST __got {{.*}} bind {{ *}}0x0 {{ *}}libfoo _tfoo
# CHAINED-NEXT: __DATA_CONST __got {{.*}} bind {{ *}}0x0 {{ *}}libfoo _gfoo
# CHAINED-NEXT: __DATA_CONST __got {{.*}} bind {{ *}}0x0 {{ *}}libfoo _ffoo
# CHAINED-NEXT: __DATA       __data {{.*}} bind {{ *}}0x0 {{ *}}libfoo _gfoo
# CHAINED-NEXT: __DATA       __data {{.*}} bind {{ *}}0x10 {{ *}}libfoo _gfoo
# CHAINED-NEXT: __DATA       __data {{.*}} bind {{ *}}0x20 {{ *}}libfoo _gfoo
# CHAINED-NEXT: __DATA       __data {{.*}} bind {{ *}}0x0 {{ *}}weak {{ *}}_weak
# CHAINED-NEXT: __DATA       __data {{.*}} rebase
# CHAINED-NEXT: __DATA       __data {{.*}} rebase

## 152 functions carry an unwind entry (_main, _shared and 150 _padding), and
## 151 do not (_noentry and 150 _plain), each of which terminates the preceding
## entry with encoding 0: 303 second-level entries in all. _sharedalt shares
## _shared's address and adds no entry of its own. A slot wrongly left
## unflagged, or flagged twice, changes these counts.
# UNWIND:      Contents of __unwind_info section:
# UNWIND:        Number of common encodings in array:       0x7f
# UNWIND:        Number of personality functions in array:  0x0
# UNWIND:        Number of indices in array:                0x2
# UNWIND:        Common encodings: (count = 127)
# UNWIND:        Personality functions: (count = 0)
# UNWIND:        Top level indices: (count = 2)
# UNWIND:        Second level indices:
# UNWIND:          Second level index[0]: {{.*}} base function offset=0x000005f0
## _main, then _noentry with no entry, then _shared (once, not twice), then
## _padding0 and _plain0.
# UNWIND:            [0]: function offset=0x000005f0, encoding[127]=0x04000018
# UNWIND-NEXT:       [1]: function offset=0x00000613, encoding[0]=0x00000000
# UNWIND-NEXT:       [2]: function offset=0x00000614, encoding[128]=0x04000038
# UNWIND-NEXT:       [3]: function offset=0x00000615, encoding[129]=0x04000058
# UNWIND-NEXT:       [4]: function offset=0x00000622, encoding[0]=0x00000000
## The last entry pins the total at 303.
# UNWIND:            [302]: function offset=0x00001131, encoding[0]=0x00000000
# UNWIND-NOT:        [303]:

# TLVBAD: error: {{.*}}tlvbad.o:(symbol _main+0x3): TLV relocation requires that symbol _notatlv be thread-local

# DEADREF: error: {{.*}}deadref.o:(symbol _main+0x3): cannot reference _init_slot defined in __mod_init_func when -init_offsets is used

#--- foo.s
.globl _ffoo
_ffoo:
  retq

.section __DATA,__data
.globl _gfoo
_gfoo:
  .quad 1
  .quad 2
  .quad 3
  .quad 4
  .quad 5

.section __DATA,__thread_data,thread_local_regular
_tfoo$tlv$init:
  .quad 123
.section __DATA,__thread_vars,thread_local_variables
.globl _tfoo
_tfoo:
  .quad __tlv_bootstrap
  .quad 0
  .quad _tfoo$tlv$init

#--- main.s
.subsections_via_symbols

.section __TEXT,__text,regular,pure_instructions
.globl _main
_main:
  .cfi_startproc
  callq _ffoo
  movq _gfoo@GOTPCREL(%rip), %rax
  movq _tfoo@TLVP(%rip), %rax
  callq _padding0
  callq _noentry
  callq _shared
  retq
  .cfi_endproc

## A function with no unwind entry at all: its slot must stay unflagged.
.globl _noentry
_noentry:
  retq

## Two symbols at one address; only the second carries the unwind entry, so the
## slot's flag has to be the OR over both addSymbol() calls.
.globl _shared, _sharedalt
_shared:
_sharedalt:
  .cfi_startproc
  retq
  .cfi_endproc

## 300 sections so the classification spans three 128-section chunks; half of
## them have unwind entries and half do not.
.macro emit_padding
.globl _padding\@
_padding\@:
  .cfi_startproc
  callq _ffoo
  movq _gfoo@GOTPCREL(%rip), %rax
  retq
  .cfi_endproc
.globl _plain\@
_plain\@:
  callq _ffoo
  retq
.endm
.rept 150
emit_padding
.endr

## A weak definition referenced by an UNSIGNED relocation: a bindingEntry event
## whose replay adds a rebase plus a weak binding.
.globl _weak
.weak_definition _weak
_weak:
  retq

.section __TEXT,__cstring
L_str:
  .asciz "str"

.section __DATA,__data
.globl _ptrs
_ptrs:
## Dylib bindings with distinct nonzero addends: the event must carry the
## addend, not re-read the relocation.
  .quad _gfoo
  .quad _gfoo + 16
  .quad _gfoo + 32
## A local definition and a weak definition.
  .quad _weak
  .quad _main
## A section referent (no symbol): a plain rebase push.
  .quad L_str

#--- tlvbad.s
.text
.globl _main
_main:
  leaq _notatlv@TLVP(%rip), %rax
  retq

.data
_notatlv:
  .quad 0

#--- deadref.s
.globl _main
.text
_main:
  leaq _init_slot(%rip), %rax
  retq

.section __DATA,__mod_init_func,mod_init_funcs
_init_slot:
  .quad _main
