# REQUIRES: aarch64, x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-macos %t/main.s -o %t/arm.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-macos %t/target.s -o %t/arm-target.o
# RUN: llvm-ar crs %t/arm.a %t/arm-target.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/main.s -o %t/x86.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/target.s -o %t/x86-target.o
# RUN: llvm-ar crs %t/x86.a %t/x86-target.o
# RUN: %python %t/check.py ld64.lld %t %S/Inputs/MacOSX.sdk

## Exercise empty, single, just-below, exactly-at and just-above 4 KiB
## relocation capacities, including paired subtractors. Repeat real links
## and failed links in-process so context cleanup and reinitialization run.
#--- check.py
import os, pathlib, subprocess, sys
lld, tmp, sdk = sys.argv[1:]
p = pathlib.Path(tmp)
env = dict(os.environ, LLD_IN_TEST='2')
for arch, stem in [('arm64', 'arm'), ('x86_64', 'x86')]:
    common = [lld, '-arch', arch, '-platform_version', 'macos', '11.0', '11.0',
              '-syslibroot', sdk, '-lSystem', str(p/(stem+'.o')),
              '-o', str(p/'out'), '-map', str(p/'map'), '-why_load']
    reference = None
    for threads in [1, 2, 8, 16]:
        cmd = common + ['--threads='+str(threads)]
        failed = subprocess.run(cmd + ['--error-limit=1'], env=env, capture_output=True)
        assert failed.returncode == 1, failed.stderr
        assert b'undefined symbol: _target' in failed.stderr, failed.stderr
        linked = subprocess.run(cmd + [str(p/(stem+'.a'))], env=env, capture_output=True)
        assert linked.returncode == 0, linked.stderr
        # m91 map object numbering can differ across thread counts. Corpus
        # replay compares parent/trial maps at each count independently.
        assert b'_target' in (p/'map').read_bytes()
        result = ((p/'out').read_bytes(), linked.stdout, linked.stderr)
        if reference is not None:
            assert reference == result, (arch, threads)
        reference = result

#--- main.s
.text
.globl _main
_main:
  ret
.data
.globl _empty, _single, _below, _at, _above, _pair
_empty:
  .quad 0
_single:
  .quad _target
_below:
  .rept 255
  .quad _target
  .endr
_at:
  .rept 256
  .quad _target
  .endr
_above:
  .rept 257
  .quad _target
  .endr
_pair:
  .quad _main - _pair
.subsections_via_symbols

#--- target.s
.data
.globl _target
_target:
  .quad 42
.subsections_via_symbols
