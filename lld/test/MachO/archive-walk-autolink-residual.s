# REQUIRES: x86
## An autolinked archive first references a walked archive's residual entry.
## Two command-line archives advertise that name: publication must preserve
## its earliest provider as well as its original symbol-vector position.
## Exercise object and member options, the mandatory -ObjC prelude, streaming,
## and weak/strong dylib collisions before and after the walked archives.
# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/Foo.framework
# RUN: cp %t/foo.tbd %t/Foo.framework/Foo.tbd
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/option.s -o %t/option.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/early.s -o %t/early.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/duplicate.s -o %t/duplicate.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/bar.s -o %t/bar.o
# RUN: llvm-ar --format=darwin rcs %t/early.a %t/early.o
# RUN: llvm-ar --format=darwin rcs %t/second.a %t/duplicate.o
# RUN: llvm-ar --format=darwin rcs %t/option.a %t/option.o
# RUN: llvm-ar --format=darwin rcs %t/libbar.a %t/bar.o %t/duplicate.o
# RUN: %python %t/check.py %t llvm-nm llvm-objdump %lld

#--- check.py
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1])
nm, objdump = sys.argv[2:4]
lld = sys.argv[4:]
for option, objc in [("option.o", False), ("option.a", False), ("option.a", True)]:
    for kind, position in [("none", "none"), ("weak", "before"),
                           ("weak", "after"), ("strong", "before"),
                           ("strong", "after")]:
        label = "%s-%s-%s-%s" % (option, objc, kind, position)
        inputs = [root / "main.o", root / option, root / "early.a",
                  root / "second.a"]
        if kind != "none":
            inputs.insert(1 if position == "before" else len(inputs),
                          root / (kind + ".tbd"))
        expected = None
        for threads, unstreamed in [(1, False), (2, False), (8, False), (8, True)]:
            d = root / (label + "-%s-%s" % (threads, unstreamed))
            d.mkdir()
            env = os.environ.copy()
            if unstreamed:
                env["LLD_NO_STREAM_MEMBER_PARSE"] = "1"
            args = lld + ["-lSystem", "-F" + str(root), "-L" + str(root),
                          "-o", str(d / "out"), "-map", str(d / "map"),
                          "-why_load", "--threads=" + str(threads),
                          "--time-trace=" + str(d / "trace.json"),
                          "--time-trace-granularity=0"]
            if objc:
                args.append("-ObjC")
            result = subprocess.run(args + list(map(str, inputs)), env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            assert result.returncode == 0, (label, result.stderr.decode())
            out = d / "out"
            # Only the output path header varies between directories.
            maptext = (d / "map").read_text().replace(str(out), "OUT")
            names = subprocess.check_output([nm, "-p", str(out)])
            binds = subprocess.check_output([objdump, "--macho", "--bind",
                                            "--lazy-bind", str(out)])
            binds = binds.replace(str(out).encode(), b"OUT")
            actual = (out.read_bytes(), result.stdout, result.stderr,
                      maptext, names, binds)
            if expected is None:
                expected = actual
            else:
                for what, a, b in zip(["output", "why_load", "stderr", "map",
                                      "symbols", "binds"], expected, actual):
                    assert a == b, (label, threads, unstreamed, what)
            if threads != 1:
                trace = (d / "trace.json").read_text()
                assert "Walk archives" in trace
                assert "Publish residual archive indexes" in trace
                assert '"Publish archive indexes"' not in trace

#--- main.s
.globl _main
_main:
  callq _option
  callq _bar
  retq

#--- option.s
.linker_option "-framework", "Foo"
.linker_option "-lbar"
.globl _option
_option:
  retq
## The -ObjC prelude selects this member even without a reference to _option.
.section __DATA,__objc_catlist,regular,no_dead_strip
.quad 0

#--- early.s
.globl _late, _collision
_late:
  callq _foo
  retq
_collision:
  retq

#--- duplicate.s
.globl _late
_late:
  int3

#--- bar.s
.globl _bar
_bar:
  callq _late
  callq _collision
  retq

#--- foo.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/System/Library/Frameworks/Foo.framework/Foo'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _foo, _collision ]
...

#--- weak.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libcollision.dylib'
exports:
  - targets: [ x86_64-macos ]
    weak-symbols: [ _collision ]
...

#--- strong.tbd
--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libcollision.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _collision ]
...
