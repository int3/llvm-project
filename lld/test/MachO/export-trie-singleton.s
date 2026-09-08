# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t.o
# RUN: %lld -dylib %t.o -o %t.dylib
# RUN: llvm-objdump --syms --exports-trie %t.dylib | FileCheck %s

# A singleton needs one compressed edge from its current trie node, rather
# than a partition step for every byte. The shared prefix pair and the prefix
# pair exercise the same shortcut below a branching node.
# CHECK-LABEL: Exports trie:
# CHECK-DAG: _a_very_long_singleton_export_name_for_the_export_trie{{$}}
# CHECK-DAG: _shared_prefix_for_fast_trie_left_suffix{{$}}
# CHECK-DAG: _shared_prefix_for_fast_trie_right_suffix{{$}}
# CHECK-DAG: _foo{{$}}
# CHECK-DAG: _foobar{{$}}
# CHECK-DAG: _weak_singleton_export [weak_def]{{$}}

.text
.globl _a_very_long_singleton_export_name_for_the_export_trie
_a_very_long_singleton_export_name_for_the_export_trie:
  ret

.globl _shared_prefix_for_fast_trie_left_suffix
_shared_prefix_for_fast_trie_left_suffix:
  ret

.globl _shared_prefix_for_fast_trie_right_suffix
_shared_prefix_for_fast_trie_right_suffix:
  ret

.globl _foo
_foo:
  ret

.globl _foobar
_foobar:
  ret

.globl _weak_singleton_export
.weak_definition _weak_singleton_export
_weak_singleton_export:
