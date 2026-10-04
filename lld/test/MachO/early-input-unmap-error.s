# REQUIRES: x86, thread_support, system-linux
## A fatal stream error inside commit() must not race the input releaser with
## global destruction. Many independent buffer owners make that race visible
## under ASan; checking only the expected nonzero exit status misses the bug.
# RUN: rm -rf %t; mkdir -p %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/input.o %s
# RUN: %python %S/Inputs/early-input-unmap-error.py %t %lld

.data
.space 65536
