# REQUIRES: aarch64, x86
# RUN: rm -rf %t; split-file %s %t
# RUN: cp %t/one.tbd %t/two.tbd
# RUN: %lld -dylib -t %t/one.tbd %t/two.tbd -o %t/out.dylib 2>&1 | FileCheck %s --check-prefix=PATHS
# RUN: not %lld -arch arm64 -dylib %t/one.tbd %t/two.tbd -o %t/out.dylib 2>&1 | FileCheck %s --check-prefix=BAD
# PATHS: one.tbd
# PATHS: two.tbd
# BAD: error: {{.*}}one.tbd{{.*}} is incompatible with arm64
# BAD: error: {{.*}}two.tbd{{.*}} is incompatible with arm64

#--- one.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: /usr/lib/libexample.dylib
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _example ]
...
