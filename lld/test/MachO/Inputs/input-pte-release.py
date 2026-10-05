import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
mc, ar = sys.argv[2:4]
linker = sys.argv[4:]


def run(command):
    p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=120)
    assert p.returncode == 0, (command, p.returncode, p.stderr)
    assert not p.stderr, p.stderr
    return p.stdout


objects = []
for i in range(20):
    source = root / (str(i) + '.s')
    obj = source.with_suffix('.o')
    source.write_text('.data\n.globl _part%d\n_part%d:\n.space 2097171\n' % (i, i))
    run([mc, '-filetype=obj', '-triple=x86_64-apple-macos', str(source), '-o', str(obj)])
    objects.append(str(obj))
main = root / 'main.s'
main.write_text('.text\n.globl _main\n_main: ret\n.data\n' +
                ''.join('.quad _part%d\n' % i for i in range(20)))
run([mc, '-filetype=obj', '-triple=x86_64-apple-macos', str(main), '-o', str(root / 'main.o')])
archive = root / 'thin.a'
run([ar, 'rcsT', str(archive)] + objects)
# One large read-only owner, including a final partial page; referenced twice
# through the read cache. The tiny main object also covers heap-backed inputs.
blob = root / 'blob'
blob.write_bytes(b'B' * (34 * 1024 * 1024 + 73))
base = linker + [str(root / 'main.o'), str(archive), str(archive),
                 '-sectcreate', '__DATA', '__blob1', str(blob),
                 '-sectcreate', '__DATA', '__blob2', str(blob), '-why_load']


def dep_records(path):
    fields = path.read_bytes().split(b'\0')
    assert fields[0] == b''
    return fields[2:]  # opcode-zero version (the first byte is itself NUL)


reference = None
map_reference = None
for threads in (1, 2, 8, 16):
    for mode in ('plain', 'map', 'reproduce'):
        outdir = root / ('t%d-%s' % (threads, mode))
        outdir.mkdir()
        output, trace = outdir / 'out', outdir / 'trace.json'
        deps, mapfile = outdir / 'deps', outdir / 'map'
        command = base + ['--threads=%d' % threads, '-o', str(output),
                          '-dependency_info', str(deps),
                          '--time-trace=' + str(trace), '--time-trace-granularity=0']
        if mode == 'map':
            command += ['-map', str(mapfile)]
        if mode == 'reproduce':
            command += ['--reproduce=' + str(outdir / 'repro.tar')]
        stdout = run(command)
        assert b'thin.a' in stdout
        value = (hashlib.sha256(output.read_bytes()).hexdigest(), stdout,
                 # Output dependency paths differ by mode/thread directory.
                 tuple(x.replace(str(output).encode(), b'OUTPUT') for x in dep_records(deps)))
        if reference is None:
            reference = value
        assert value == reference, (threads, mode)
        names = [e['name'] for e in json.loads(trace.read_text())['traceEvents']]
        expected = threads != 1 and mode == 'plain'
        assert ('Release input PTEs' in names) == expected, (threads, mode)
        assert ('Release input buffers' in names) == expected, (threads, mode)
        if mode == 'map':
            contents = mapfile.read_text().replace(str(output), 'OUTPUT')
            if map_reference is None:
                map_reference = contents
            assert contents == map_reference, threads
        output.unlink()
        if mode == 'reproduce':
            (outdir / 'repro.tar').unlink()
