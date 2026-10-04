# REQUIRES: x86, thread_support
## At more than one thread the writer unmaps the input files on a helper thread
## while the UUID, the code signature and output mapping release run (releaseInputBuffers()).
## The output must equal the one-thread output byte for byte, with and without
## chained fixups (buildFixupChains() reads input section names on its error
## path, so the release starts after it), with -sectcreate data (copied from an
## input buffer by writeSections()), and with a thin archive, whose members the
## walker maps itself. The map file thread and -dependency_info read input-backed
## names after the release point, so they turn the release off; --reproduce is
## conservatively gated too. Their output must still be complete. The time trace shows whether the
## release ran. The output basenames match so the ad-hoc signature does too.

# RUN: rm -rf %t; split-file %s %t; mkdir -p %t/t1 %t/t4
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/main.o %t/main.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/foo.o %t/foo.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/bar.o %t/bar.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos -o %t/baz.o %t/baz.s
# RUN: llvm-ar csr %t/bar.a %t/bar.o
# RUN: llvm-ar csr --thin %t/baz.a %t/baz.o
# RUN: %lld -dylib -o %t/libfoo.dylib %t/foo.o

# RUN: %lld --threads=1 -adhoc_codesign %t/main.o %t/bar.a %t/baz.a \
# RUN:   %t/libfoo.dylib -lSystem -sectcreate __DATA __blob %t/blob.txt \
# RUN:   -o %t/t1/out --time-trace-granularity=0 --time-trace=%t/t1/out.json
# RUN: %lld --threads=4 -adhoc_codesign %t/main.o %t/bar.a %t/baz.a \
# RUN:   %t/libfoo.dylib -lSystem -sectcreate __DATA __blob %t/blob.txt \
# RUN:   -o %t/t4/out --time-trace-granularity=0 --time-trace=%t/t4/out.json
# RUN: cmp %t/t1/out %t/t4/out
# RUN: FileCheck --check-prefix=RELEASED %s < %t/t4/out.json
# RUN: FileCheck --check-prefix=KEPT %s < %t/t1/out.json

# RUN: %lld --threads=1 -adhoc_codesign -fixup_chains \
# RUN:   -platform_version macos 13.0 13.0 %t/main.o %t/bar.a %t/baz.a \
# RUN:   %t/libfoo.dylib -lSystem -o %t/t1/chained
# RUN: %lld --threads=4 -adhoc_codesign -fixup_chains \
# RUN:   -platform_version macos 13.0 13.0 %t/main.o %t/bar.a %t/baz.a \
# RUN:   %t/libfoo.dylib -lSystem -o %t/t4/chained
# RUN: cmp %t/t1/chained %t/t4/chained

# RUN: %lld --threads=1 -adhoc_codesign %t/main.o %t/bar.a %t/baz.a \
# RUN:   %t/libfoo.dylib -lSystem -map %t/t1/map -dependency_info %t/t1/deps \
# RUN:   -o %t/t1/named
# RUN: %lld --threads=4 -adhoc_codesign %t/main.o %t/bar.a %t/baz.a \
# RUN:   %t/libfoo.dylib -lSystem -map %t/t4/map -dependency_info %t/t4/deps \
# RUN:   -o %t/t4/named
# RUN: cmp %t/t1/named %t/t4/named
# RUN: FileCheck --check-prefix=MAP %s < %t/t4/map
# RUN: %python %S/Inputs/DependencyDump.py %t/t4/deps | FileCheck --check-prefix=DEPS %s

## Each consumer on its own also keeps the inputs mapped.
# RUN: %lld --threads=4 %t/main.o %t/bar.a %t/baz.a %t/libfoo.dylib -lSystem \
# RUN:   -map %t/t4/map-only -o %t/t4/map-only.out \
# RUN:   --time-trace-granularity=0 --time-trace=%t/t4/map-only.json
# RUN: FileCheck --check-prefix=MAP %s < %t/t4/map-only
# RUN: FileCheck --check-prefix=KEPT %s < %t/t4/map-only.json
# RUN: %lld --threads=4 %t/main.o %t/bar.a %t/baz.a %t/libfoo.dylib -lSystem \
# RUN:   -dependency_info %t/t4/deps-only -o %t/t4/deps-only.out \
# RUN:   --time-trace-granularity=0 --time-trace=%t/t4/deps-only.json
# RUN: %python %S/Inputs/DependencyDump.py %t/t4/deps-only | FileCheck --check-prefix=DEPS %s
# RUN: FileCheck --check-prefix=KEPT %s < %t/t4/deps-only.json
# RUN: cd %t && %lld --threads=4 main.o bar.a baz.a libfoo.dylib -lSystem \
# RUN:   --reproduce repro.tar -o %t/t4/repro.out \
# RUN:   --time-trace-granularity=0 --time-trace=%t/t4/repro.json
# RUN: FileCheck --check-prefix=KEPT %s < %t/t4/repro.json
# RUN: tar tf %t/repro.tar | FileCheck --check-prefix=TAR %s

# RELEASED: "Release input buffers"
# KEPT-NOT: "Release input buffers"

# MAP:      # Object files:
# MAP-DAG:  ] {{.*}}main.o
# MAP-DAG:  ] {{.*}}bar.a(bar.o)
# MAP-DAG:  ] {{.*}}baz.a(baz.o)
# MAP:      # Symbols:
# MAP-DAG:  ] _main
# MAP-DAG:  ] _bar
# MAP-DAG:  ] _baz

# DEPS-DAG: input-file: {{.*}}{{[/\]}}bar.a
# DEPS-DAG: input-file: {{.*}}{{[/\]}}libfoo.dylib
# DEPS-DAG: input-file: {{.*}}{{[/\]}}main.o
# DEPS-DAG: input-file: {{.*}}bar.o
# DEPS-DAG: input-file: {{.*}}baz.o
# DEPS:     output-file: {{.*}}

# TAR-DAG: repro/{{.*}}main.o
# TAR-DAG: repro/{{.*}}baz.o

#--- foo.s
.globl _foo
_foo:
  ret

#--- bar.s
.globl _bar
_bar:
  ret

#--- baz.s
.globl _baz
_baz:
  ret

#--- main.s
.globl _main
_main:
  callq _foo
  callq _bar
  callq _baz
  ret

.data
.quad _foo

#--- blob.txt
some section contents
