#!/usr/bin/env python3
"""Compare lld-macho's parallel markLive drains on a many-core machine.

usage: marklive-bench.py [--work DIR] [--threads 1,8,...] [--rounds N]
                         [--workloads a,b,...] [--jobs N] [--skip-build]

Builds ld64.lld from int3/llvm-project for each variant, fetches public
Mach-O LinkerBench captures, then links every workload with every
(variant, thread count) pair in shuffled rounds. Each link records wall
time, the --time-trace markLive total and an output hash. A separate pass
runs each variant's stats build once per workload and thread count and
records what its drain did (levels, task splits or steals).

Writes DIR/report.md (paste it back) and DIR/samples.jsonl. Needs Python 3,
git, cmake, ninja, a C++17 compiler, curl and zstd. Python standard library
only.
"""
import argparse, hashlib, json, os, platform, random, re, shlex, shutil
import statistics, subprocess, sys, time
from pathlib import Path

REPO = 'https://github.com/int3/llvm-project.git'
BENCH_REPO = 'https://github.com/int3/macho-linker-bench.git'
# name: (branch to time, branch with LLD_MARKLIVE_STATS counters)
VARIANTS = {
    'levels': ('macho-parallel-marklive-merged', 'macho-parallel-marklive-stats'),
    'split': ('macho-marklive-split', 'macho-marklive-split-stats'),
    'steal': ('macho-marklive-steal', 'macho-marklive-steal-stats'),
}
BASE = 'levels'
# LinkerBench workload -> extra link arguments. rust-debug-ed adds
# -export_dynamic, which makes 285K symbols roots.
WORKLOADS = {
    'chromium-macos-sym1': [],
    'firefox-macos': [],
    'firefox-ios': [],
    'rust-debug': [],
    'rust-debug-ed': ['-export_dynamic'],
}


def run(argv, **kw):
    print('+', ' '.join(map(str, argv)), flush=True)
    return subprocess.run(argv, check=True, **kw)


def build(work, jobs):
    src, bld, bins = work / 'llvm-project', work / 'build', work / 'bin'
    bins.mkdir(parents=True, exist_ok=True)
    if not src.exists():
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', REPO, src])
    branches = [b for pair in VARIANTS.values() for b in pair]
    run(['git', '-C', src, 'fetch', '--force', 'origin',
         *[f'+refs/heads/{b}:refs/remotes/origin/{b}' for b in branches]])
    for name, pair in VARIANTS.items():
        for branch, suffix in zip(pair, ('', '-stats')):
            run(['git', '-C', src, 'checkout', '--force', '--detach', f'origin/{branch}'])
            if not (bld / 'build.ninja').exists():
                run(['cmake', '-G', 'Ninja', '-S', src / 'llvm', '-B', bld,
                     '-DCMAKE_BUILD_TYPE=Release', '-DLLVM_ENABLE_ASSERTIONS=OFF',
                     '-DLLVM_ENABLE_PROJECTS=lld', '-DLLVM_TARGETS_TO_BUILD=AArch64;X86',
                     '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF'])
            run(['ninja', '-C', bld, f'-j{jobs}', 'lld'])
            shutil.copy2(bld / 'bin' / 'lld', bins / f'ld64.lld-{name}{suffix}')
            rev = subprocess.run(['git', '-C', src, 'rev-parse', 'HEAD'], capture_output=True,
                                 text=True, check=True).stdout.strip()
            (bins / f'ld64.lld-{name}{suffix}.rev').write_text(rev + '\n')


def fetch(work, workloads):
    bench, corpora = work / 'macho-linker-bench', work / 'corpora'
    if not bench.exists():
        run(['git', 'clone', BENCH_REPO, bench])
    dirs = {}
    for w in workloads:
        cap = w.removesuffix('-ed')
        if not (corpora / cap).exists() or not list((corpora / cap).rglob('response.txt')):
            run([sys.executable, bench / 'fetch.py', '--public', '--extract', '--no-cache',
                 '--dir', corpora, cap])
        found = list((corpora / cap).rglob('response.txt'))
        if len(found) != 1:
            sys.exit(f'{cap}: expected one response.txt under {corpora / cap}, found {len(found)}')
        dirs[w] = found[0].parent
    return dirs


def link(binary, d, extra, threads, scratch, env=None):
    out, trace = scratch / 'out', scratch / 'trace.json'
    argv = [str(binary), '@response.txt', *extra, f'--threads={threads}',
            f'--time-trace={trace}', '-o', str(out)]
    toks = shlex.split((d / 'response.txt').read_text())
    for flag in ('-map', '-dependency_info', '-object_path_lto'):
        if flag in toks:
            argv += [flag, str(scratch / flag[1:])]
    errf = scratch / 'stderr'
    with open(errf, 'w') as ef:
        t0 = time.perf_counter()
        p = subprocess.Popen(argv, cwd=d, stdout=subprocess.DEVNULL, stderr=ef, env=env)
        _, status, ru = os.wait4(p.pid, 0)
        wall = time.perf_counter() - t0
    stderr = errf.read_text()
    code = os.waitstatus_to_exitcode(status)
    if code:
        sys.exit(f'{binary} {d} t{threads}: exit {code}\n{stderr[-2000:]}')
    cpu = ru.ru_utime + ru.ru_stime
    # ru_maxrss is KiB on Linux and bytes on macOS.
    rss_mib = ru.ru_maxrss / (1 << 20 if sys.platform == 'darwin' else 1 << 10)
    ev = json.load(open(trace))['traceEvents']
    ml = sum(e['dur'] for e in ev if e.get('name') == 'markLive' and e.get('ph') == 'X') / 1000
    h = hashlib.sha256()
    with open(out, 'rb') as f:
        while b := f.read(1 << 22):
            h.update(b)
    out.unlink()
    trace.unlink()
    return dict(wall=wall, ml=ml, cpu=cpu, rss=rss_mib, hash=h.hexdigest()), stderr


def ci(base, new, rng):
    """95% bootstrap CI of (mean(new) - mean(base)) / mean(base), in percent."""
    est = []
    for _ in range(2000):
        b = [rng.choice(base) for _ in base]
        n = [rng.choice(new) for _ in new]
        est.append((statistics.fmean(n) / statistics.fmean(b) - 1) * 100)
    est.sort()
    return est[50], est[1949]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ncpu = os.cpu_count()
    ap.add_argument('--work', type=Path, default=Path('marklive-bench'))
    ap.add_argument('--threads', default=','.join(str(t) for t in sorted({1, 8, 16, 32, 64, 128, ncpu}) if t <= ncpu))
    ap.add_argument('--rounds', type=int, default=15)
    ap.add_argument('--warmups', type=int, default=2)
    ap.add_argument('--workloads', default=','.join(WORKLOADS))
    ap.add_argument('--jobs', type=int, default=ncpu)
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--scratch', type=Path, help='output directory for links; default: a tmpfs under /dev/shm on Linux')
    a = ap.parse_args()
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    threads = [int(t) for t in a.threads.split(',')]
    workloads = a.workloads.split(',')
    if not a.skip_build:
        build(work, a.jobs)
    dirs = fetch(work, workloads)
    scratch = a.scratch or (Path('/dev/shm') / f'marklive-bench-{os.getpid()}'
                            if Path('/dev/shm').is_dir() else work / 'scratch')
    scratch.mkdir(parents=True, exist_ok=True)
    bins = work / 'bin'
    rows = work / 'samples.jsonl'
    rows.unlink(missing_ok=True)
    rng = random.Random(20261110)
    combos = [(v, t) for v in VARIANTS for t in threads]
    samples, stats = [], []
    try:
        for w in workloads:
            extra = WORKLOADS.get(w, [])
            for v, t in combos * a.warmups:
                link(bins / f'ld64.lld-{v}', dirs[w], extra, t, scratch)
            for i in range(a.rounds):
                order = combos[:]
                rng.shuffle(order)
                for v, t in order:
                    m, _ = link(bins / f'ld64.lld-{v}', dirs[w], extra, t, scratch)
                    r = dict(w=w, v=v, t=t, round=i, **m)
                    samples.append(r)
                    with open(rows, 'a') as f:
                        f.write(json.dumps(r) + '\n')
                print(f'{w} round {i + 1}/{a.rounds} {time.strftime("%H:%M:%S")}', flush=True)
            env = dict(os.environ, LLD_MARKLIVE_STATS='1')
            for v, t in combos:
                _, err = link(bins / f'ld64.lld-{v}-stats', dirs[w], extra, t, scratch, env)
                for line in err.splitlines():
                    if line.startswith('marklive-stats '):
                        stats.append(dict(w=w, v=v, t=t, **dict(kv.split('=', 1) for kv in line.split()[1:])))
            print(f'{w} done {time.strftime("%H:%M:%S")}', flush=True)
    finally:
        if not a.scratch:
            shutil.rmtree(scratch, ignore_errors=True)
    (work / 'stats.json').write_text(json.dumps(stats, indent=1))

    def cpu_model():
        try:
            for line in open('/proc/cpuinfo'):
                if line.startswith('model name'):
                    return line.split(':', 1)[1].strip()
        except OSError:
            pass
        return platform.processor()

    revs = {v: (bins / f'ld64.lld-{v}.rev').read_text().strip() for v in VARIANTS}
    out = [f'# markLive drains: {", ".join(VARIANTS)}', '',
           f'- Machine: {cpu_model()}, {ncpu} logical CPUs, {platform.platform()}',
           f'- Revisions: ' + ', '.join(f'{v} `{r[:12]}`' for v, r in revs.items()),
           f'- {a.rounds} rounds, {a.warmups} warmups; CIs are 95% bootstrap intervals of the '
           f'difference of means against `{BASE}`.', '']
    for w in workloads:
        hashes = {s['hash'] for s in samples if s['w'] == w}
        out += [f'## {w}', '', f'Distinct output hashes: {len(hashes)}'
                + ('' if len(hashes) == 1 else ' (OUTPUT DIFFERS)'), '']
        for metric, label, scale, unit in (('ml', 'markLive', 1, 'ms'), ('wall', 'wall', 1, 's'),
                                           ('cpu', 'CPU (user+sys)', 1, 's'), ('rss', 'peak RSS', 1, 'MiB')):
            out += [f'{label}, median {unit} (change vs {BASE}):', '',
                    '| threads | ' + ' | '.join(VARIANTS) + ' |',
                    '|---|' + '---|' * len(VARIANTS)]
            for t in threads:
                cell = {v: [s[metric] * scale for s in samples if s['w'] == w and s['v'] == v and s['t'] == t]
                        for v in VARIANTS}
                row = []
                for v in VARIANTS:
                    m = statistics.median(cell[v])
                    if v == BASE:
                        row.append(f'{m:.3f}' if unit == 's' else f'{m:.1f}')
                    else:
                        lo, hi = ci(cell[BASE], cell[v], rng)
                        row.append((f'{m:.3f}' if unit == 's' else f'{m:.1f}') + f' ({lo:+.1f}..{hi:+.1f}%)')
                    del m
                out.append(f'| {t} | ' + ' | '.join(row) + ' |')
            out.append('')
        out += ['Drain stats (stats builds, one link each):', '']
        for s in stats:
            if s['w'] == w:
                out.append('- ' + ' '.join(f'{k}={v}' for k, v in s.items() if k not in ('w',)))
        out.append('')
    (work / 'report.md').write_text('\n'.join(out) + '\n')
    print(f'DONE: {work / "report.md"}')


if __name__ == '__main__':
    main()
