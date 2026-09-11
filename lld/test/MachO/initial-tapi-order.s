# REQUIRES: x86 && thread_support

# RUN: rm -rf %t; split-file %s %t --no-leading-lines
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/test.s -o %t/test.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/static.s -o %t/static.o
# RUN: mkdir -p %t/Static.framework
# RUN: llvm-ar rcs %t/Static.framework/Static %t/static.o

## Keep the default --read-workers=0 path. The TAPI inputs are separated only
## by inert options and carry repeated metadata. The static framework drains
## the preceding TAPI run before its archive is published.
# RUN: %lld -arch x86_64 -dylib -install_name /InitialTapiOrder -dead_strip_dylibs -platform_version macos 11.0 11.0 -L%t -F%t \
# RUN:   --threads=1 %t/libFirst.tbd -no_uuid -needed_library %t/libMetadata.tbd \
# RUN:   -weak_library %t/libMetadata.tbd -reexport_library %t/libReexport.tbd \
# RUN:   -no_uuid -framework First -needed_framework Needed -weak_framework Weak \
# RUN:   -framework Static %t/libEffect.tbd %t/test.o -o %t/threads1
# RUN: %lld -arch x86_64 -dylib -install_name /InitialTapiOrder -dead_strip_dylibs -platform_version macos 11.0 11.0 -L%t -F%t \
# RUN:   --threads=8 %t/libFirst.tbd -no_uuid -needed_library %t/libMetadata.tbd \
# RUN:   -weak_library %t/libMetadata.tbd -reexport_library %t/libReexport.tbd \
# RUN:   -no_uuid -framework First -needed_framework Needed -weak_framework Weak \
# RUN:   -framework Static %t/libEffect.tbd %t/test.o -o %t/threads8
# RUN: cmp %t/threads1 %t/threads8
# RUN: llvm-otool -L %t/threads8 | FileCheck %s --check-prefix=EFFECT
# RUN: llvm-otool -l %t/threads8 | FileCheck %s --check-prefix=METADATA

## An earlier malformed TAPI must diagnose before a later missing input lookup.
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 \
# RUN:   %t/libBad.tbd %t/missing.tbd -o /dev/null 2>&1 | FileCheck %s --check-prefix=BAD-MISSING
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 -F%t \
# RUN:   %t/libBad.tbd -framework Missing -o /dev/null 2>&1 | FileCheck %s --check-prefix=BAD-FRAMEWORK

## Opt-in read workers retain their deferred path; missing lookups must not use
## the default-reader drain helper.
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --read-workers=1 -L%t \
# RUN:   %t/libBad.tbd -lmissing -o /dev/null 2>&1 | FileCheck %s --check-prefix=WORKER-LIBRARY
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --read-workers=8 -F%t \
# RUN:   %t/libBad.tbd -framework Missing -o /dev/null 2>&1 | FileCheck %s --check-prefix=WORKER-FRAMEWORK

# EFFECT: /NewEffect
# BAD-MISSING: error: {{.*}}libBad.tbd
# BAD-MISSING-NEXT: error: cannot open {{.*}}missing.tbd: No such file or directory
# BAD-FRAMEWORK: error: {{.*}}libBad.tbd
# BAD-FRAMEWORK-NEXT: error: framework not found for -framework Missing
# WORKER-LIBRARY: error: library not found for -lmissing
# WORKER-FRAMEWORK: error: framework not found for -framework Missing
# METADATA: cmd LC_LOAD_WEAK_DYLIB
# METADATA: name /usr/lib/libMetadata.dylib
# METADATA: cmd LC_REEXPORT_DYLIB
# METADATA: name /usr/lib/libReexport.dylib

#--- test.s
.globl _root
_root:
  retq

.data
  .quad _first
  .quad _reexported
  .quad _leaf
  .quad _first_framework
  .quad _needed_framework
  .quad _weak_framework
  .quad _effect
  .quad _static

#--- static.s
.globl _static
_static:
  retq

#--- libFirst.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libFirst.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _first ]
...

#--- libMetadata.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libMetadata.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _metadata ]
...

#--- libReexport.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libReexport.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _reexported ]
    re-exports: [ libLeaf.dylib ]
...

#--- libLeaf.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libLeaf.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _leaf ]
...

#--- libEffect.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /OldEffect
exports:
  - archs: [ x86_64 ]
    symbols: [ '$ld$install_name$os11.0$/NewEffect', _effect ]
...

#--- libBad.tbd
--- !tapi-tbd-v3
invalid YAML

#--- First.framework/First.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /System/Library/Frameworks/First.framework/First
exports:
  - archs: [ x86_64 ]
    symbols: [ _first_framework ]
...

#--- Needed.framework/Needed.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /System/Library/Frameworks/Needed.framework/Needed
exports:
  - archs: [ x86_64 ]
    symbols: [ _needed_framework ]
...

#--- Weak.framework/Weak.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /System/Library/Frameworks/Weak.framework/Weak
exports:
  - archs: [ x86_64 ]
    symbols: [ _weak_framework ]
...
