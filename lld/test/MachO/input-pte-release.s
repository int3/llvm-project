# REQUIRES: x86, thread_support, system-linux
## Cross the advice threshold using both a large owner and walker-owned thin
## members. Repeated archive and sectcreate references must not create new
## owners or advice ranges. The helper checks all release gates and t1/2/8/16
## binary/map/why-load equality, including real library cleanup/re-entry.
# RUN: rm -rf %t; mkdir -p %t
# RUN: %python %S/Inputs/input-pte-release.py %t llvm-mc llvm-ar %lld
