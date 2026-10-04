import collections
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
linker = sys.argv[2:]


def run(command, failure=False, single=False):
    env = dict(os.environ, LLD_IN_TEST='1') if single else None
    result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=120, env=env)
    stderr = result.stderr.decode(errors='replace')
    assert 'AddressSanitizer' not in stderr, stderr
    assert (result.returncode != 0) == failure, stderr
    return stderr


for name in ['main', 'a', 'b', 'thin', 'fat', 'dyn']:
    run(['llvm-mc', '-filetype=obj', '-triple=x86_64-apple-macos',
         name + '.s', '-o', name + '.o'])
for name in ['a', 'b']:
    (root / name).mkdir()
    (root / name / 'same.o').write_bytes((root / (name + '.o')).read_bytes())
    run(['llvm-ar', 'csr', name + '.a', name + '/same.o'])
run(['llvm-ar', 'csr', '--thin', 'thin.a', 'thin.o'])
run(['llvm-ar', 'csr', 'fat-x86.a', 'fat.o'])
run(['llvm-mc', '-filetype=obj', '-triple=arm64-apple-macos',
     'fat.s', '-o', 'fat-arm.o'])
run(['llvm-ar', 'csr', 'fat-arm.a', 'fat-arm.o'])
run(['llvm-lipo', '-create', 'fat-x86.a', 'fat-arm.a', '-output', 'fat.a'])
run(['llvm-as', 'bc.ll', '-o', 'bc.o'])
run(linker + ['-dylib', 'dyn.o', '-o', 'libdyn.dylib'])
(root / 'missing').mkdir()
base = ['main.o', 'a.a', 'b.a', 'thin.a', 'fat.a', 'bc.o',
        '-Lmissing', '-L.', '-ldyn', '-lSystem',
        '-sectcreate', '__DATA', '__blob', 'blob.txt', '-o', 'out']


def records(path):
    data = path.read_bytes()
    result = []
    while data:
        end = data.index(b'\0', 1)
        result.append((data[0], data[1:end]))
        data = data[end + 1:]
    return result


reference = None
for threads in [1, 2, 8, 16]:
    trace = root / ('trace-' + str(threads) + '.json')
    stderr = run(linker + base + ['--threads=' + str(threads),
                 '-dependency_info', 'deps', '--time-trace=' + str(trace),
                 '--time-trace-granularity=0'])
    assert not stderr, stderr
    deps = records(root / 'deps')
    names = [name for opcode, name in deps if opcode == 0x10]
    assert names == sorted(names)
    assert collections.Counter(names)[b'same.o'] == 2, names
    for name in [b'main.o', b'a.a', b'b.a', b'thin.a', b'fat.a',
                 b'fat.o', b'bc.o', b'blob.txt']:
        assert name in names, (name, names)
    assert any(name.endswith(b'thin.o') for name in names), names
    assert any(name.endswith(b'libdyn.dylib') for name in names), names
    assert any(b'lto' in name for name in names), names
    assert any(op == 0x11 and b'missing' in name for op, name in deps), deps
    assert deps[-1] == (0x40, b'out'), deps[-1]
    current = ((root / 'out').read_bytes(), deps)
    if reference is None:
        reference = current
    else:
        assert current == reference, threads
    released = any(e['name'] == 'Release input buffers'
                   for e in json.loads(trace.read_text())['traceEvents'])
    assert released == (threads > 1), threads

# Consumers that still need live inputs must retain their gates, and produce
# identical dependency bytes through the live-input fallback.
for extra in [['-map', 'out.map'], ['--reproduce', 'out.tar'],
              ['-map', 'out.map', '--reproduce', 'out.tar']]:
    run(linker + base + ['--threads=8', '-dependency_info', 'deps',
        '--time-trace=gate.json', '--time-trace-granularity=0'] + extra)
    assert records(root / 'deps') == reference[1]
    assert (root / 'out').read_bytes() == reference[0]
    assert not any(e['name'] == 'Release input buffers'
                   for e in json.loads((root / 'gate.json').read_text())['traceEvents'])

# Open failure stays a post-writer warning, with an otherwise successful link.
stderr = run(linker + base + ['--threads=8', '-dependency_info',
             'absent/deps'], single=True)
assert 'Error writing dependency info to file' in stderr, stderr
assert (root / 'out').read_bytes() == reference[0]

# A writer early return must use the live names even though no snapshot exists.
for threads in [1, 8]:
    stderr = run(linker + base + ['--threads=' + str(threads),
                 '-u', '_undefined', '-dependency_info', 'early-deps'],
                 failure=True, single=True)
    assert 'undefined symbol' in stderr, stderr
    assert records(root / 'early-deps') == reference[1]

# An unwritable existing path disables tracking at construction. A directory
# instead exercises the deferred open failure portably, without root assumptions.
(root / 'dep-directory').mkdir()
stderr = run(linker + base + ['--threads=8', '-dependency_info',
             'dep-directory'], single=True)
assert 'Error writing dependency info to file' in stderr, stderr

# A dependency stream write failure also occurs after the releaser was joined.
if sys.platform.startswith('linux'):
    stderr = run(linker + base + ['--threads=8', '-dependency_info',
                 '/dev/full'], failure=True, single=True)
    assert 'IO failure on output stream' in stderr, stderr
