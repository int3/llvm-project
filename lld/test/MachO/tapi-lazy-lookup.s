# REQUIRES: x86, aarch64
# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: %lld -dylib -L%t -lfirst -lsecond %t/main.o -o %t/out.dylib
# RUN: llvm-objdump --macho --syms %t/out.dylib | FileCheck %s --check-prefix=ORDER
# RUN: llvm-objdump --macho --lazy-bind %t/out.dylib | FileCheck %s --check-prefix=BINDS
# RUN: cp %t/out.dylib %t/default.dylib
# RUN: %lld --threads=1 -dylib -L%t -lfirst -lsecond %t/main.o -o %t/out.dylib
# RUN: cmp %t/default.dylib %t/out.dylib
# RUN: %lld -dylib %t/main.o -L%t -lfirst -lsecond -o %t/after.dylib
# RUN: llvm-objdump --macho --lazy-bind %t/after.dylib | FileCheck %s --check-prefix=BINDS

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/late.s -o %t/late.o
# RUN: %lld -arch arm64 -dylib %t/late.tbd %t/late.o -o %t/late.dylib
# RUN: llvm-objdump --macho --syms %t/late.dylib | FileCheck %s --check-prefix=LATE
# LATE: *UND* _objc_msgSend{{$}}
# LATE-NEXT: _zulu

# ORDER: dyld_stub_binder
# ORDER-NEXT: _first
# ORDER-NEXT: _same
# ORDER-NEXT: _second
# BINDS-DAG: libsecond        _same
# BINDS-DAG: libfirst         _first
# BINDS-DAG: libsecond        _second

#--- libfirst.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: libfirst.dylib
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _first ]
    weak-symbols: [ _same ]
...
#--- libsecond.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: libsecond.dylib
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _same, _second ]
...
#--- main.s
.globl _entry
_entry:
  callq _second
  callq _first
  callq _same
  ret

#--- late.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ arm64-macos ]
install-name: liblate.dylib
exports:
  - targets: [ arm64-macos ]
    symbols: [ _objc_msgSend, _zulu ]
...
#--- late.s
.globl _entry
_entry:
  bl _zulu
  bl _objc_msgSend$foo
  ret
.subsections_via_symbols
