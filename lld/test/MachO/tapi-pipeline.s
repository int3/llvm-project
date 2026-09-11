# REQUIRES: x86 && thread_support

## Pipelined TAPI preparation must not change what the ordered input walk reads,
## diagnoses or publishes.

# RUN: rm -rf %t; split-file %s %t --no-leading-lines
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/test.s -o %t/test.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/static.s -o %t/static.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/autolink.s -o %t/autolink.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/test2.s -o %t/test2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/test3.s -o %t/test3.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/leafuse.s -o %t/leafuse.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/lduse.s -o %t/lduse.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/ldhidden.s -o %t/ldhidden.o
# RUN: mkdir -p %t/ObjcObject.framework %t/Static.framework
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/objcframework.s -o %t/ObjcObject.framework/ObjcObject
# RUN: llvm-ar rcs %t/Static.framework/Static %t/static.o
# RUN: llvm-ar rcs %t/libarchive.a %t/static.o
# RUN: ln -s %t/libLeaf.tbd %t/libLeafLink.tbd

## A malformed TAPI input placed after objects is diagnosed at its own
## position, at every thread count.
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=1 \
# RUN:   %t/test.o %t/libBad.tbd %t/missing.tbd -o /dev/null 2>&1 | FileCheck %s --check-prefix=ORDER
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=2 \
# RUN:   %t/test.o %t/libBad.tbd %t/missing.tbd -o /dev/null 2>&1 | FileCheck %s --check-prefix=ORDER
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=8 \
# RUN:   %t/test.o %t/libBad.tbd %t/missing.tbd -o /dev/null 2>&1 | FileCheck %s --check-prefix=ORDER

# ORDER:      error: could not load TAPI file at {{.*}}libBad.tbd
# ORDER-NEXT: error: cannot open {{.*}}missing.tbd: No such file or directory

## The same TBD reached through a symlink and through its own path is one
## DylibFile, so its symbols are not duplicated.
# RUN: %lld -arch x86_64 -dylib -install_name /Symlink -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=8 %t/libLeafLink.tbd %t/libLeaf.tbd %t/leafuse.o -o %t/symlink8 2>&1 | count 0
# RUN: %lld -arch x86_64 -dylib -install_name /Symlink -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=1 %t/libLeafLink.tbd %t/libLeaf.tbd %t/leafuse.o -o %t/symlink1 2>&1 | count 0
# RUN: cmp %t/symlink1 %t/symlink8
# RUN: llvm-otool -L %t/symlink8 | FileCheck %s --check-prefix=SYMLINK
# SYMLINK-COUNT-1: /usr/lib/libLeaf.dylib
# SYMLINK-NOT: /usr/lib/libLeaf.dylib

## A -l resolving to an archive is not a TAPI input and still loads as one.
# RUN: %lld -arch x86_64 -dylib -install_name /Archive -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=8 -larchive %t/test2.o -o %t/archive8
# RUN: %lld -arch x86_64 -dylib -install_name /Archive -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=1 -larchive %t/test2.o -o %t/archive1
# RUN: cmp %t/archive1 %t/archive8

## A framework already loaded as an object file is not loaded again.
# RUN: %lld -arch x86_64 -dylib -install_name /ObjcObject -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -F%t --threads=8 %t/ObjcObject.framework/ObjcObject -framework ObjcObject %t/test.o -o %t/objcobj8
# RUN: %lld -arch x86_64 -dylib -install_name /ObjcObject -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -F%t --threads=1 %t/ObjcObject.framework/ObjcObject -framework ObjcObject %t/test.o -o %t/objcobj1
# RUN: cmp %t/objcobj1 %t/objcobj8

## A missing re-export is reported once, for the re-exporting file, in order.
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=8 \
# RUN:   %t/libMissingReexport.tbd %t/libBad.tbd -o /dev/null 2>&1 | FileCheck %s --check-prefix=REEXPORT
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=1 \
# RUN:   %t/libMissingReexport.tbd %t/libBad.tbd -o /dev/null 2>&1 | FileCheck %s --check-prefix=REEXPORT
# REEXPORT:      error: {{.*}}libMissingReexport.tbd{{.*}}: unable to locate re-export with install name /usr/lib/libNotThere.dylib
# REEXPORT-NEXT: error: could not load TAPI file at {{.*}}libBad.tbd

## $ld$hide and $ld$previous in a prepared file behave as before.
# RUN: %lld -arch x86_64 -dylib -install_name /Ld -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=8 %t/libLd.tbd %t/lduse.o -o %t/ld8
# RUN: %lld -arch x86_64 -dylib -install_name /Ld -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=1 %t/libLd.tbd %t/lduse.o -o %t/ld1
# RUN: cmp %t/ld1 %t/ld8
# RUN: llvm-otool -L %t/ld8 | FileCheck %s --check-prefix=LDSYM
# LDSYM: /usr/lib/libOld.dylib
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=8 \
# RUN:   %t/libLd.tbd %t/ldhidden.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=HIDDEN
# HIDDEN: error: undefined symbol: _hidden

## --reproduce records the same members whether or not the pipeline could run.
# RUN: rm -f %t/repro8.tar %t/repro1.tar
# RUN: %lld -arch x86_64 -dylib -install_name /Repro -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=8 --reproduce %t/repro8.tar %t/libFirst.tbd %t/libReexport.tbd %t/test3.o -o %t/repro8
# RUN: %lld -arch x86_64 -dylib -install_name /Repro -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=1 --reproduce %t/repro1.tar %t/libFirst.tbd %t/libReexport.tbd %t/test3.o -o %t/repro1
# RUN: tar tf %t/repro8.tar | sed -e 's,^[^/]*/,,' | sort > %t/repro8.list
# RUN: tar tf %t/repro1.tar | sed -e 's,^[^/]*/,,' | sort > %t/repro1.list
# RUN: diff %t/repro1.list %t/repro8.list

## An autolinked framework whose name is also on the command line resolves once.
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=8 -lFirst %t/autolink.o %t/test.o -o %t/auto8
# RUN: %lld -arch x86_64 -dylib -install_name /Auto -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=1 -lFirst %t/autolink.o %t/test.o -o %t/auto1
# RUN: cmp %t/auto1 %t/auto8
# RUN: llvm-otool -L %t/auto8 | FileCheck %s --check-prefix=AUTO
# AUTO-COUNT-1: /usr/lib/libFirst.dylib
# AUTO-NOT: /usr/lib/libFirst.dylib

#--- test.s
.globl _root
_root:
  retq

#--- test2.s
.globl _root2
_root2:
  retq
.data
  .quad _static

#--- test3.s
.globl _root3
_root3:
  retq
.data
  .quad _first
  .quad _reexported

#--- leafuse.s
.globl _leafroot
_leafroot:
  retq
.data
  .quad _leaf

#--- lduse.s
.globl _ldroot
_ldroot:
  retq
.data
  .quad _normal

#--- ldhidden.s
.globl _hiddenroot
_hiddenroot:
  retq
.data
  .quad _hidden

#--- static.s
.globl _static
_static:
  retq

#--- autolink.s
.globl _autoroot
_autoroot:
  retq
.section __DATA,__objc_catlist
.quad 0
.linker_option "-lFirst"

#--- objcframework.s
.globl _objcsym
_objcsym:
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

#--- libMissingReexport.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libMissingReexport.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _present ]
    re-exports: [ /usr/lib/libNotThere.dylib ]
...

#--- libLd.tbd
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /usr/lib/libLd.dylib
exports:
  - archs: [ x86_64 ]
    symbols: [ _normal, _hidden, '$ld$hide$os11.0$_hidden',
                '$ld$previous$/usr/lib/libOld.dylib$$1$11.0$12.0$_normal$' ]
...

#--- libBad.tbd
--- !tapi-tbd-v3
invalid YAML
