# REQUIRES: x86
## A thin archive's members are separate files, so the walker maps each one
## itself on a worker. Going through Archive::Child::getMemoryBufferRef() put
## the mapping in the Archive's own ThinBuffers vector, which no lock guards,
## so two workers filling two members of one thin archive corrupted the heap.
## The links are repeated because the corruption is timing dependent: with
## this fixture the unfixed linker aborted or segfaulted in about one -ObjC
## link in six. Every output must also equal the one-thread output byte for
## byte, and the output basenames match so the ad-hoc signature does too.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/t1 %t/t8 %t/objc1 %t/objc8
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/main.o %t/main.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m0.o %t/m0.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m1.o %t/m1.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m2.o %t/m2.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m3.o %t/m3.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m4.o %t/m4.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m5.o %t/m5.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m6.o %t/m6.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m7.o %t/m7.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m8.o %t/m8.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m9.o %t/m9.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m10.o %t/m10.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m11.o %t/m11.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m12.o %t/m12.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m13.o %t/m13.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m14.o %t/m14.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m15.o %t/m15.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m16.o %t/m16.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m17.o %t/m17.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m18.o %t/m18.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m19.o %t/m19.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m20.o %t/m20.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m21.o %t/m21.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m22.o %t/m22.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m23.o %t/m23.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m24.o %t/m24.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m25.o %t/m25.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m26.o %t/m26.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m27.o %t/m27.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m28.o %t/m28.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m29.o %t/m29.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m30.o %t/m30.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m31.o %t/m31.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m32.o %t/m32.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m33.o %t/m33.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m34.o %t/m34.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m35.o %t/m35.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m36.o %t/m36.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m37.o %t/m37.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m38.o %t/m38.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m39.o %t/m39.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m40.o %t/m40.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m41.o %t/m41.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m42.o %t/m42.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m43.o %t/m43.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m44.o %t/m44.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m45.o %t/m45.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m46.o %t/m46.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m47.o %t/m47.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m48.o %t/m48.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m49.o %t/m49.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m50.o %t/m50.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m51.o %t/m51.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m52.o %t/m52.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m53.o %t/m53.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m54.o %t/m54.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m55.o %t/m55.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m56.o %t/m56.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m57.o %t/m57.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m58.o %t/m58.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m59.o %t/m59.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m60.o %t/m60.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m61.o %t/m61.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m62.o %t/m62.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/m63.o %t/m63.s
# RUN: llvm-ar csrT %t/thin.a %t/m0.o %t/m1.o %t/m2.o %t/m3.o %t/m4.o %t/m5.o %t/m6.o %t/m7.o %t/m8.o %t/m9.o %t/m10.o %t/m11.o %t/m12.o %t/m13.o %t/m14.o %t/m15.o %t/m16.o %t/m17.o %t/m18.o %t/m19.o %t/m20.o %t/m21.o %t/m22.o %t/m23.o %t/m24.o %t/m25.o %t/m26.o %t/m27.o %t/m28.o %t/m29.o %t/m30.o %t/m31.o %t/m32.o %t/m33.o %t/m34.o %t/m35.o %t/m36.o %t/m37.o %t/m38.o %t/m39.o %t/m40.o %t/m41.o %t/m42.o %t/m43.o %t/m44.o %t/m45.o %t/m46.o %t/m47.o %t/m48.o %t/m49.o %t/m50.o %t/m51.o %t/m52.o %t/m53.o %t/m54.o %t/m55.o %t/m56.o %t/m57.o %t/m58.o %t/m59.o %t/m60.o %t/m61.o %t/m62.o %t/m63.o

# RUN: %lld -lSystem -o %t/t1/out --threads=1 %t/main.o %t/thin.a
# RUN: %lld -lSystem -o %t/objc1/out --threads=1 -ObjC %t/main.o %t/thin.a
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/t8/out --threads=8 %t/main.o %t/thin.a
# RUN: cmp %t/t1/out %t/t8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out
# RUN: %lld -lSystem -o %t/objc8/out --threads=8 -ObjC %t/main.o %t/thin.a
# RUN: cmp %t/objc1/out %t/objc8/out

# RUN: llvm-nm %t/t1/out | FileCheck %s
# CHECK-DAG: _f0
# CHECK-DAG: _f63

#--- main.s
.globl _main
_main:
  callq _f0
  callq _f1
  callq _f2
  callq _f3
  callq _f4
  callq _f5
  callq _f6
  callq _f7
  callq _f8
  callq _f9
  callq _f10
  callq _f11
  callq _f12
  callq _f13
  callq _f14
  callq _f15
  callq _f16
  callq _f17
  callq _f18
  callq _f19
  callq _f20
  callq _f21
  callq _f22
  callq _f23
  callq _f24
  callq _f25
  callq _f26
  callq _f27
  callq _f28
  callq _f29
  callq _f30
  callq _f31
  callq _f32
  callq _f33
  callq _f34
  callq _f35
  callq _f36
  callq _f37
  callq _f38
  callq _f39
  callq _f40
  callq _f41
  callq _f42
  callq _f43
  callq _f44
  callq _f45
  callq _f46
  callq _f47
  callq _f48
  callq _f49
  callq _f50
  callq _f51
  callq _f52
  callq _f53
  callq _f54
  callq _f55
  callq _f56
  callq _f57
  callq _f58
  callq _f59
  callq _f60
  callq _f61
  callq _f62
  callq _f63
  retq

#--- m0.s
.globl _f0
_f0:
  callq _f1
  retq

#--- m1.s
.globl _f1
_f1:
  callq _f2
  retq

#--- m2.s
.globl _f2
_f2:
  callq _f3
  retq

#--- m3.s
.globl _f3
_f3:
  callq _f4
  retq

#--- m4.s
.globl _f4
_f4:
  callq _f5
  retq

#--- m5.s
.globl _f5
_f5:
  callq _f6
  retq

#--- m6.s
.globl _f6
_f6:
  callq _f7
  retq

#--- m7.s
.globl _f7
_f7:
  callq _f8
  retq

#--- m8.s
.globl _f8
_f8:
  callq _f9
  retq

#--- m9.s
.globl _f9
_f9:
  callq _f10
  retq

#--- m10.s
.globl _f10
_f10:
  callq _f11
  retq

#--- m11.s
.globl _f11
_f11:
  callq _f12
  retq

#--- m12.s
.globl _f12
_f12:
  callq _f13
  retq

#--- m13.s
.globl _f13
_f13:
  callq _f14
  retq

#--- m14.s
.globl _f14
_f14:
  callq _f15
  retq

#--- m15.s
.globl _f15
_f15:
  callq _f16
  retq

#--- m16.s
.globl _f16
_f16:
  callq _f17
  retq

#--- m17.s
.globl _f17
_f17:
  callq _f18
  retq

#--- m18.s
.globl _f18
_f18:
  callq _f19
  retq

#--- m19.s
.globl _f19
_f19:
  callq _f20
  retq

#--- m20.s
.globl _f20
_f20:
  callq _f21
  retq

#--- m21.s
.globl _f21
_f21:
  callq _f22
  retq

#--- m22.s
.globl _f22
_f22:
  callq _f23
  retq

#--- m23.s
.globl _f23
_f23:
  callq _f24
  retq

#--- m24.s
.globl _f24
_f24:
  callq _f25
  retq

#--- m25.s
.globl _f25
_f25:
  callq _f26
  retq

#--- m26.s
.globl _f26
_f26:
  callq _f27
  retq

#--- m27.s
.globl _f27
_f27:
  callq _f28
  retq

#--- m28.s
.globl _f28
_f28:
  callq _f29
  retq

#--- m29.s
.globl _f29
_f29:
  callq _f30
  retq

#--- m30.s
.globl _f30
_f30:
  callq _f31
  retq

#--- m31.s
.globl _f31
_f31:
  callq _f32
  retq

#--- m32.s
.globl _f32
_f32:
  callq _f33
  retq

#--- m33.s
.globl _f33
_f33:
  callq _f34
  retq

#--- m34.s
.globl _f34
_f34:
  callq _f35
  retq

#--- m35.s
.globl _f35
_f35:
  callq _f36
  retq

#--- m36.s
.globl _f36
_f36:
  callq _f37
  retq

#--- m37.s
.globl _f37
_f37:
  callq _f38
  retq

#--- m38.s
.globl _f38
_f38:
  callq _f39
  retq

#--- m39.s
.globl _f39
_f39:
  callq _f40
  retq

#--- m40.s
.globl _f40
_f40:
  callq _f41
  retq

#--- m41.s
.globl _f41
_f41:
  callq _f42
  retq

#--- m42.s
.globl _f42
_f42:
  callq _f43
  retq

#--- m43.s
.globl _f43
_f43:
  callq _f44
  retq

#--- m44.s
.globl _f44
_f44:
  callq _f45
  retq

#--- m45.s
.globl _f45
_f45:
  callq _f46
  retq

#--- m46.s
.globl _f46
_f46:
  callq _f47
  retq

#--- m47.s
.globl _f47
_f47:
  callq _f48
  retq

#--- m48.s
.globl _f48
_f48:
  callq _f49
  retq

#--- m49.s
.globl _f49
_f49:
  callq _f50
  retq

#--- m50.s
.globl _f50
_f50:
  callq _f51
  retq

#--- m51.s
.globl _f51
_f51:
  callq _f52
  retq

#--- m52.s
.globl _f52
_f52:
  callq _f53
  retq

#--- m53.s
.globl _f53
_f53:
  callq _f54
  retq

#--- m54.s
.globl _f54
_f54:
  callq _f55
  retq

#--- m55.s
.globl _f55
_f55:
  callq _f56
  retq

#--- m56.s
.globl _f56
_f56:
  callq _f57
  retq

#--- m57.s
.globl _f57
_f57:
  callq _f58
  retq

#--- m58.s
.globl _f58
_f58:
  callq _f59
  retq

#--- m59.s
.globl _f59
_f59:
  callq _f60
  retq

#--- m60.s
.globl _f60
_f60:
  callq _f61
  retq

#--- m61.s
.globl _f61
_f61:
  callq _f62
  retq

#--- m62.s
.globl _f62
_f62:
  callq _f63
  retq

#--- m63.s
.globl _f63
_f63:
  callq _f0
  retq

