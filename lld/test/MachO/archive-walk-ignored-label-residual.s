# REQUIRES: x86
## An archive index advertises _cf, but its member defines only a local label.
## A reservation is not a reference, nor does extracting the indexed member
## establish a definition. Decline before mutation and preserve serial lazy
## state, extraction causes, reference strengths, and diagnostics. In particular:
## - an absent reference must not extract a later real definition;
## - a real reference must extract a later definition even after the ignored
##   provider was fetched, whether it is strong or weak;
## - duplicate ignored providers may remain unfetched when a later object
##   references the name (residual publication used to assert on the duplicate).
## Cover autolinked inputs and ordinary phase boundaries, with strict links as
## well as dynamic lookup, at t1/t2/t8/t16 and unstreamed t8.
# RUN: rm -rf %t; split-file %s %t
# RUN: %python %t/check.py %t llvm-mc llvm-ar llvm-nm llvm-objdump %lld

#--- check.py
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
mc, ar, nm, objdump = sys.argv[2:6]
lld = sys.argv[6:]

def assemble(name, source, weak=False):
    args = [mc, '-filetype=obj', '-triple=x86_64-apple-macos',
            str(root / source), '-o', str(root / (name + '.o'))]
    if weak:
        args.append('-defsym=WEAK=1')
    subprocess.run(args, check=True, timeout=60)

for name in ['main', 'main-auto', 'mainuser', 'mainuser-auto',
             'ignored', 'ref', 'user', 'real', 'empty']:
    assemble(name, name + '.s')
for name in ['ref', 'user']:
    assemble(name + '-weak', name + '.s', True)
for name, members in [('ignored', ['ignored']), ('duplicate', ['ignored']),
                      ('user', ['user']), ('user-weak', ['user-weak']),
                      ('libuser', ['user']), ('libuser-weak', ['user-weak']),
                      ('real', ['real']), ('libreal', ['real'])]:
    subprocess.run([ar, '--format=darwin', 'rcs', str(root / (name + '.a'))] +
                   [str(root / (m + '.o')) for m in members], check=True,
                   timeout=60)

cases = [
    ('absent-autolink', ['main-auto.o', 'ignored.a'], False, 'absent'),
    ('absent-boundary', ['main.o', 'ignored.a', 'empty.o', 'real.a'], False, 'absent'),
]
for weak in [False, True]:
    suffix = '-weak' if weak else ''
    user = 'user' + suffix + '.a'
    ref = 'ref' + suffix + '.o'
    for dynamic in [False, True]:
        tag = suffix + ('-dynamic' if dynamic else '-strict')
        cases += [
            ('definition-boundary' + tag,
             ['mainuser.o', 'ignored.a', user, 'real.a', 'empty.o'], dynamic, 'defined'),
            ('definition-autolink' + tag,
             ['mainuser-auto.o', 'ignored.a', user], dynamic, 'defined'),
            ('duplicate' + tag,
             ['main.o', 'ignored.a', 'duplicate.a', ref], dynamic, 'undefined'),
            ('missing-boundary' + tag,
             ['main.o', 'ignored.a', ref], dynamic, 'undefined'),
        ]
    # References first arrive in an autolinked member, after the phase.
    option = root / ('auto-user' + suffix + '.s')
    option.write_text('.linker_option "-luser' + suffix + '"\n' +
                      (root / 'mainuser.s').read_text())
    assemble('auto-user' + suffix, option.name)
    for dynamic in [False, True]:
        cases.append(('missing-autolink' + suffix + str(dynamic),
                      ['auto-user' + suffix + '.o', 'ignored.a'], dynamic, 'undefined'))

# The guard must also recognize all other shouldIgnoreLabel() categories, and
# must not rely on mandatory extraction, streaming, or linker options.
for section, body, symbol in [
    ('classrefs', '.section __DATA,__objc_classrefs\n.quad 0\n', '_cf'),
    ('selrefs', '.section __DATA,__objc_selrefs\n.quad 0\n', '_cf'),
    ('cstring', '.cstring\n.asciz "ignored"\n', 'lignored'),
    ('literal4', '.section __TEXT,__literal4,4byte_literals\n.long 42\n', 'lignored'),
    ('literal8', '.section __TEXT,__literal8,8byte_literals\n.quad 42\n', 'lignored'),
    ('literal16', '.section __TEXT,__literal16,16byte_literals\n.quad 42, 43\n', 'lignored'),
]:
    directive, data = body.split('\n', 1)
    src = root / (section + '.s')
    src.write_text('.globl _f, ' + symbol + '\n.text\n_f: retq\n' +
                   directive + '\n' + symbol + ':\n' + data)
    assemble(section, src.name)
    subprocess.run([ar, '--format=darwin', 'rcs', str(root / (section + '.a')),
                    str(root / (section + '.o'))], check=True, timeout=60)
    cases.append((section, ['main.o', section + '.a', 'empty.o'], False, None))

rows = []
for label, inputs, dynamic, kind in cases:
    expected = None
    for threads, unstreamed in [(1, False), (2, False), (8, False),
                                (16, False), (8, True)]:
        d = root / (label + '-t' + str(threads) + ('-unstreamed' if unstreamed else ''))
        d.mkdir(exist_ok=True)
        out = d / 'out'
        env = os.environ.copy()
        env.pop('LLD_NO_STREAM_MEMBER_PARSE', None)
        if unstreamed:
            env['LLD_NO_STREAM_MEMBER_PARSE'] = '1'
        args = lld + ['-L' + str(root), '-o', str(out), '-why_load',
                      '-map', str(d / 'map'), '--threads=' + str(threads),
                      '--time-trace=' + str(d / 'trace.json'),
                      '--time-trace-granularity=0']
        if dynamic:
            args += ['-undefined', 'dynamic_lookup']
        result = subprocess.run(args + [str(root / p) for p in inputs],
                                env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=60)
        failing = kind == 'undefined' and not dynamic
        assert result.returncode == (1 if failing else 0), (label, threads, result.stderr)
        actual = {'rc': result.returncode, 'why_load': result.stdout,
                  'stderr': result.stderr}
        if failing:
            assert b'undefined symbol: _cf' in result.stderr, (label, result.stderr)
        else:
            names = subprocess.check_output([nm, '-p', str(out)], timeout=60)
            binds = subprocess.check_output([objdump, '--macho', '--syms', '--bind',
                                             '--lazy-bind', str(out)], timeout=60)
            actual.update(output=out.read_bytes(), symbols=names,
                          binds=binds.replace(str(out).encode(), b'OUT'),
                          map=(d / 'map').read_bytes().replace(str(out).encode(), b'OUT'))
            external_cf = [line for line in names.splitlines()
                           if line.endswith(b' _cf') and line.split()[-2] in [b'D', b'U']]
            if kind == 'absent':
                assert not external_cf, (label, names)
                assert b'real.a(' not in result.stdout, (label, result.stdout)
            elif kind == 'defined':
                assert any(b' D _cf' in line for line in external_cf), (label, names)
                assert b'real.a(real.o)' in result.stdout, (label, result.stdout)
            elif kind == 'undefined':
                assert any(b' U _cf' in line for line in external_cf), (label, names)
                cf_binds = [line for line in binds.splitlines() if b'flat-namespace' in line and b'_cf' in line]
                assert cf_binds, (label, binds)
                assert all((b'(weak_import)' in line) == ('-weak' in label)
                           for line in cf_binds), (label, cf_binds)
        if expected is None:
            expected = actual
        else:
            for what in expected:
                assert expected[what] == actual[what], (label, threads, unstreamed, what)
        if threads != 1 and not failing:
            trace = (d / 'trace.json').read_text()
            assert '"Publish archive indexes"' in trace, (label, trace)
            assert '"Filter and fold archive indexes"' not in trace, label
            assert '"Publish residual archive indexes"' not in trace, label
        rows.append(dict(case=label, threads=threads, unstreamed=unstreamed,
                         **{k: hashlib.sha256(v).hexdigest() if isinstance(v, bytes) else v
                            for k, v in actual.items()}))
(root / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')

#--- main.s
.globl _main, ___CFConstantStringClassReference
_main:
  callq _f
  retq
.data
___CFConstantStringClassReference:
  .quad 0

#--- main-auto.s
.linker_option "-lreal"
.globl _main, ___CFConstantStringClassReference
_main:
  callq _f
  retq
.data
___CFConstantStringClassReference:
  .quad 0

#--- mainuser.s
.globl _main, ___CFConstantStringClassReference
_main:
  callq _f
  callq _g
  retq
.data
___CFConstantStringClassReference:
  .quad 0

#--- mainuser-auto.s
.linker_option "-lreal"
.globl _main, ___CFConstantStringClassReference
_main:
  callq _f
  callq _g
  retq
.data
___CFConstantStringClassReference:
  .quad 0

#--- ignored.s
.globl _f, _cf
.text
_f:
  retq
.cstring
L_.str:
  .asciz "foo"
.section __DATA,__cfstring
.p2align 3
_cf:
  .quad ___CFConstantStringClassReference
  .long 1992
  .space 4
  .quad L_.str
  .quad 3

#--- ref.s
.ifdef WEAK
.weak_reference _cf
.endif
.data
  .quad _cf

#--- user.s
.ifdef WEAK
.weak_reference _cf
.endif
.globl _g
.text
_g:
  retq
.data
  .quad _cf

#--- real.s
.globl _cf
.data
_cf:
  .quad 42

#--- empty.s
.text
