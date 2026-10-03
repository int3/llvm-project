# REQUIRES: x86
# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-as %t/root.ll -o %t/root.o
# RUN: llvm-as %t/malformed.ll -o %t/malformed.o
# RUN: mkdir -p %t/Obj.framework
# RUN: llvm-mc -triple=x86_64-apple-macos -filetype=obj %t/obj.s -o %t/Obj.framework/Obj
# RUN: %lld -no_uuid -lSystem -weak-lA -F%t -L%t %t/root.o -o %t/default
# RUN: %lld -no_uuid --threads=1 --read-workers=4 -lSystem -weak-lA -F%t -L%t %t/root.o -o %t/parallel
# RUN: cmp %t/default %t/parallel
# RUN: llvm-objdump --macho --all-headers %t/default | FileCheck --check-prefix=WEAK %s
# RUN: not %lld -no_uuid -lSystem -L%t %t/malformed.o -o %t/malformed 2>&1 | FileCheck --check-prefix=MALFORMED %s
# RUN: not %lld -no_uuid --threads=1 --read-workers=4 -lSystem -L%t %t/malformed.o -o %t/malformed 2>&1 | FileCheck --check-prefix=MALFORMED %s

# WEAK:      cmd LC_LOAD_WEAK_DYLIB
# WEAK-NEXT: cmdsize
# WEAK-NEXT:    name /usr/lib/libA.dylib

# MALFORMED: error: could not load TAPI file at {{.*}}libBad.tbd: unsupported file type

#--- root.ll
target triple = "x86_64-apple-macosx10.15.0"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

!0 = !{!"-framework", !"Obj"}
!1 = !{!"-lA"}
!2 = !{!"-lB"}
!llvm.linker.options = !{!0, !1, !2}

define i32 @main() {
  call void @obj()
  %a = call i32 @a()
  %b = call i32 @b()
  %sum = add i32 %a, %b
  ret i32 %sum
}

declare void @obj()
declare i32 @a()
declare i32 @b()

#--- malformed.ll
target triple = "x86_64-apple-macosx10.15.0"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

!0 = !{!"-lBad"}
!llvm.linker.options = !{!0}

define i32 @main() {
  ret i32 0
}

#--- libBad.tbd
--- !tapi-tbd-v3
invalid YAML

#--- obj.s
.subsections_via_symbols
.globl _obj
_obj:
  retq

#--- libA.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libA.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _a ]
...

#--- libB.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ x86_64-macos ]
install-name:     '/usr/lib/libB.dylib'
exports:
  - targets:      [ x86_64-macos ]
    symbols:      [ _b ]
...
