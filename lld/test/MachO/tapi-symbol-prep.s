# REQUIRES: x86 && thread_support

## Preparing a TAPI file's symbols on a pipeline worker -- the final names with
## the ObjC prefixes, their hashes, and the pre-parsed "$ld$" records -- must
## publish the same names in the same order and keep every "$ld$" side effect
## and diagnostic that the ordered constructor had.

# RUN: rm -rf %t; split-file %s %t --no-leading-lines
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/objc.s -o %t/objc.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/use.s -o %t/use.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/hideuse.s -o %t/hideuse.o

## Every ObjC name kind a TBD can export is materialized with its prefix, and
## the published set and order is the same at one and eight threads.
# RUN: %lld -arch x86_64 -dylib -install_name /Objc -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=8 %t/libObjc.tbd %t/objc.o -o %t/objc8 2>%t/objc8.err
# RUN: %lld -arch x86_64 -dylib -install_name /Objc -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=1 %t/libObjc.tbd %t/objc.o -o %t/objc1 2>%t/objc1.err
# RUN: cmp %t/objc1 %t/objc8
# RUN: diff %t/objc1.err %t/objc8.err
# RUN: llvm-objdump --macho --bind %t/objc8 | FileCheck %s --check-prefix=OBJC
# OBJC-DAG: pointer {{.*}} _OBJC_CLASS_$_Cls
# OBJC-DAG: pointer {{.*}} _OBJC_METACLASS_$_Cls
# OBJC-DAG: pointer {{.*}} _OBJC_EHTYPE_$_Eh
# OBJC-DAG: pointer {{.*}} _OBJC_IVAR_$_Cls._var
# OBJC-DAG: pointer {{.*}} _plain
# OBJC-DAG: pointer {{.*}} _weak
# OBJC-DAG: {{.*}} _tlv

## $ld$previous with a symbol name creates the synthetic dylib; the entries for
## another platform and for a version window that excludes the target are
## dropped during preparation and must stay invisible.
# RUN: %lld -arch x86_64 -dylib -install_name /Prev -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=8 %t/libPrev.tbd %t/use.o -o %t/prev8 2>%t/prev8.err
# RUN: %lld -arch x86_64 -dylib -install_name /Prev -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=1 %t/libPrev.tbd %t/use.o -o %t/prev1 2>%t/prev1.err
# RUN: cmp %t/prev1 %t/prev8
# RUN: diff %t/prev1.err %t/prev8.err
# RUN: llvm-otool -L %t/prev8 > %t/prev8.libs
# RUN: FileCheck %s --check-prefix=PREV < %t/prev8.libs
# RUN: not grep libIos %t/prev8.libs
# RUN: not grep libWindow %t/prev8.libs
# PREV: /usr/lib/libOld.dylib

## A nameless $ld$previous applies before a named one -- the interface symbol
## order decides which, and the named record must see what the nameless one
## wrote -- so the compatibility version the named record falls back to is the
## 5.0 just written, not the interface's compatibility version of 1.
# RUN: %lld -arch x86_64 -dylib -install_name /Live -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=8 %t/libPrevLive.tbd %t/use.o -o %t/live8 2>%t/live8.err
# RUN: %lld -arch x86_64 -dylib -install_name /Live -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=1 %t/libPrevLive.tbd %t/use.o -o %t/live1 2>%t/live1.err
# RUN: cmp %t/live1 %t/live8
# RUN: diff %t/live1.err %t/live8.err
# RUN: llvm-otool -L %t/live8 > %t/live8.libs
# RUN: FileCheck %s --check-prefix=LIVE < %t/live8.libs
# LIVE: /usr/lib/libOldLive.dylib (compatibility version 5.0.0

## An applying $ld$install_name renames the dylib; one whose os version does not
## match the deployment target does not.
# RUN: %lld -arch x86_64 -dylib -install_name /Name -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=8 %t/libName.tbd %t/use.o -o %t/name8 2>%t/name8.err
# RUN: %lld -arch x86_64 -dylib -install_name /Name -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   --threads=1 %t/libName.tbd %t/use.o -o %t/name1 2>%t/name1.err
# RUN: cmp %t/name1 %t/name8
# RUN: diff %t/name1.err %t/name8.err
# RUN: llvm-otool -L %t/name8 > %t/name8.libs
# RUN: FileCheck %s --check-prefix=NAME < %t/name8.libs
# RUN: not grep libName %t/name8.libs
# NAME: /usr/lib/libRenamed.dylib
# RUN: %lld -arch x86_64 -dylib -install_name /Name -no_uuid -platform_version macos 12.0 12.0 \
# RUN:   --threads=8 %t/libName.tbd %t/use.o -o %t/name12-8
# RUN: %lld -arch x86_64 -dylib -install_name /Name -no_uuid -platform_version macos 12.0 12.0 \
# RUN:   --threads=1 %t/libName.tbd %t/use.o -o %t/name12-1
# RUN: cmp %t/name12-1 %t/name12-8
# RUN: llvm-otool -L %t/name12-8 > %t/name12.libs
# RUN: FileCheck %s --check-prefix=NAME12 < %t/name12.libs
# RUN: not grep libRenamed %t/name12.libs
# NAME12: /usr/lib/libName.dylib

## A $ld$hide$os matching the target hides; one that does not match is dropped
## during preparation and must not hide. An unknown "$ld$" action is a no-op.
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=8 \
# RUN:   %t/libHide.tbd %t/hideuse.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=HIDE11
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=1 \
# RUN:   %t/libHide.tbd %t/hideuse.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=HIDE11
# HIDE11-DAG: error: undefined symbol: _hidden11
# HIDE11-DAG: error: undefined symbol: _hiddenAlways
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 11.0 11.0 --threads=8 \
# RUN:   %t/libHide.tbd %t/hideuse.o -o /dev/null 2>&1 | grep -c "undefined symbol" \
# RUN:   | FileCheck %s --check-prefix=HIDE11N
# HIDE11N: 2
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 10.0 10.0 --threads=8 \
# RUN:   %t/libHide.tbd %t/hideuse.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=HIDE10
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 10.0 10.0 --threads=1 \
# RUN:   %t/libHide.tbd %t/hideuse.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=HIDE10
# HIDE10-DAG: error: undefined symbol: _hidden10
# HIDE10-DAG: error: undefined symbol: _hiddenAlways
# RUN: not %lld -arch x86_64 -dylib -platform_version macos 10.0 10.0 --threads=8 \
# RUN:   %t/libHide.tbd %t/hideuse.o -o /dev/null 2>&1 | grep -c "undefined symbol" \
# RUN:   | FileCheck %s --check-prefix=HIDE10N
# HIDE10N: 2

## A hidden name is read again while a re-exported library publishes its
## symbols, after the interface it was parsed from is gone. libLeaf's install
## name is deliberately not one of the implicitly linked locations, so the leaf
## publishes through the umbrella as its exporting file and the umbrella's
## $ld$hide$_normal reaches the leaf's _normal: the reference is left undefined.
# RUN: not %lld -arch x86_64 -dylib -install_name /Reexp -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=8 %t/libUmbrella.tbd %t/use.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=REEXP
# RUN: not %lld -arch x86_64 -dylib -install_name /Reexp -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -L%t --threads=1 %t/libUmbrella.tbd %t/use.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=REEXP
# REEXP: error: undefined symbol: _normal
## Letting that reference resolve dynamically makes the link succeed, so the
## published set and the diagnostics can still be compared byte for byte.
# RUN: %lld -arch x86_64 -dylib -install_name /Reexp -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -undefined dynamic_lookup -L%t --threads=8 %t/libUmbrella.tbd %t/use.o \
# RUN:   -o %t/reexp8 2>%t/reexp8.err
# RUN: %lld -arch x86_64 -dylib -install_name /Reexp -no_uuid -platform_version macos 11.0 11.0 \
# RUN:   -undefined dynamic_lookup -L%t --threads=1 %t/libUmbrella.tbd %t/use.o \
# RUN:   -o %t/reexp1 2>%t/reexp1.err
# RUN: cmp %t/reexp1 %t/reexp8
# RUN: diff %t/reexp1.err %t/reexp8.err

## Every malformed "$ld$" string still warns, with the same text, at the same
## position, at one and eight threads.
# RUN: %no-fatal-warnings-lld -arch x86_64 -dylib -install_name /W -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 --threads=8 %t/libWarnStart.tbd %t/libWarnEnd.tbd \
# RUN:   %t/libWarnCompat.tbd %t/libWarnOs.tbd %t/libWarnHidden.tbd %t/use.o \
# RUN:   -o %t/warn8 2>%t/warn8.err
# RUN: %no-fatal-warnings-lld -arch x86_64 -dylib -install_name /W -no_uuid \
# RUN:   -platform_version macos 11.0 11.0 --threads=1 %t/libWarnStart.tbd %t/libWarnEnd.tbd \
# RUN:   %t/libWarnCompat.tbd %t/libWarnOs.tbd %t/libWarnHidden.tbd %t/use.o \
# RUN:   -o %t/warn1 2>%t/warn1.err
# RUN: cmp %t/warn1 %t/warn8
# RUN: diff %t/warn1.err %t/warn8.err
# RUN: FileCheck %s --check-prefix=WARN < %t/warn8.err
# WARN-DAG: libWarnStart.tbd{{.*}}: failed to parse start version, symbol '$ld$previous$/usr/lib/libOld.dylib$$1$bad$12.0$_normal$' ignored
# WARN-DAG: libWarnEnd.tbd{{.*}}: failed to parse end version, symbol '$ld$previous$/usr/lib/libOld.dylib$$1$11.0$bad$_normal$' ignored
# WARN-DAG: libWarnCompat.tbd{{.*}}: failed to parse compatibility version, symbol '$ld$previous$/usr/lib/libOld.dylib$bad$1$11.0$12.0$_normal$' ignored
# WARN-DAG: libWarnOs.tbd{{.*}}: failed to parse os version, symbol '$ld$install_name$osbad$/usr/lib/libRenamed.dylib' ignored
# WARN-DAG: libWarnHidden.tbd{{.*}}: failed to parse hidden version, symbol `$ld$hide$osbad$_normal` ignored.

#--- objc.s
.globl _objcroot
_objcroot:
  movq _tlv@TLVP(%rip), %rax
  retq
.data
  .quad _OBJC_CLASS_$_Cls
  .quad _OBJC_METACLASS_$_Cls
  .quad _OBJC_EHTYPE_$_Eh
  .quad _OBJC_IVAR_$_Cls._var
  .quad _plain
  .quad _weak

#--- use.s
.globl _useroot
_useroot:
  retq
.data
  .quad _normal

#--- hideuse.s
.globl _hideroot
_hideroot:
  retq
.data
  .quad _hidden11
  .quad _hidden10
  .quad _hiddenAlways
  .quad _visible

#--- libObjc.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libObjc.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _plain ]
    weak-symbols: [ _weak ]
    thread-local-symbols: [ _tlv ]
    objc-classes: [ Cls ]
    objc-eh-types: [ Eh ]
    objc-ivars: [ Cls._var ]
...

#--- libPrev.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libPrev.dylib'
compatibility-version: 1
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _normal,
               '$ld$previous$/usr/lib/libOld.dylib$$1$11.0$12.0$_normal$',
               '$ld$previous$/usr/lib/libIos.dylib$$2$1.0$99.0$$',
               '$ld$previous$/usr/lib/libWindow.dylib$$1$1.0$2.0$$' ]
...

#--- libPrevLive.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libPrevLive.dylib'
compatibility-version: 1
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _normal,
               '$ld$previous$/usr/lib/libNameless.dylib$5.0$1$11.0$12.0$$',
               '$ld$previous$/usr/lib/libOldLive.dylib$$1$11.0$12.0$_normal$' ]
...

#--- libName.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libName.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _normal, '$ld$install_name$os11.0$/usr/lib/libRenamed.dylib' ]
...

#--- libHide.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libHide.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _hidden11, _hidden10, _hiddenAlways, _visible,
               '$ld$hide$os11.0$_hidden11', '$ld$hide$os10.0$_hidden10',
               '$ld$hide$_hiddenAlways', '$ld$bogusAction$_visible' ]
...

#--- libUmbrella.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libUmbrella.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ '$ld$hide$_normal' ]
reexported-libraries:
  - targets: [ x86_64-macos ]
    libraries: [ '/Leaf/libLeaf.dylib' ]
...

#--- libLeaf.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/Leaf/libLeaf.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _normal ]
...

#--- libWarnStart.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libWarnStart.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ '$ld$previous$/usr/lib/libOld.dylib$$1$bad$12.0$_normal$' ]
...

#--- libWarnEnd.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libWarnEnd.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ '$ld$previous$/usr/lib/libOld.dylib$$1$11.0$bad$_normal$' ]
...

#--- libWarnCompat.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libWarnCompat.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ '$ld$previous$/usr/lib/libOld.dylib$bad$1$11.0$12.0$_normal$' ]
...

#--- libWarnOs.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libWarnOs.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _normal, '$ld$install_name$osbad$/usr/lib/libRenamed.dylib' ]
...

#--- libWarnHidden.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libWarnHidden.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _normal, '$ld$hide$osbad$_normal' ]
...
