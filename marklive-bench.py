#!/usr/bin/env python3
"""Compare lld-macho's parallel markLive drains on a many-core machine.

usage: marklive-bench.py [--work DIR] [--threads 1,8,...] [--rounds N]
                         [--workloads a,b,...] [--resume] [--report-only]

Downloads prebuilt static x86_64 Linux ld64.lld binaries for each variant and
the public Mach-O LinkerBench captures, then links every workload with every
(variant, thread count) pair in shuffled rounds. Each link records wall
time, the --time-trace markLive total and an output hash. A separate pass
runs each variant's stats build once per workload and thread count and
records what its drain did (levels, task splits or steals).

Writes DIR/report.md (paste it back), DIR/samples.jsonl and DIR/stats.jsonl.
If the run is interrupted, rerun with --resume: finished workloads are kept.
Needs Python 3, curl and tar; the download includes a static zstd.
"""
import argparse, hashlib, json, os, platform, random, re, shlex, shutil
import statistics, subprocess, sys, tarfile, time, urllib.request
from pathlib import Path

# name: (branch to time, branch with LLD_MARKLIVE_STATS counters). `levels` is
# #15 (parallel roots, level-synchronous drain); `split` (#22) and `steal` (#23)
# replace its drain. Each timed branch is its PR plus one commit that adds a
# "markLive drain" time-trace scope.
VARIANTS = {
    'levels': ('macho-marklive-parallel-roots-v2-bench', 'macho-marklive-parallel-roots-v2-stats'),
    'split': ('macho-marklive-split-bench', 'macho-marklive-split-stats'),
    'steal': ('macho-marklive-steal-bench', 'macho-marklive-steal-stats'),
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


BUNDLE_URL = ('https://github.com/int3/llvm-project/releases/download/'
              'marklive-repro-v1/marklive-repro-linux-x86_64.tar.gz')
BUNDLE_SHA256 = 'e95eea5c95473f99e826cb4e9b2feaf2e06381b48de0cf41c35e397adbfe67cb'


def download_bundle(work):
    """Unpacks the binaries, a static zstd and LinkerBench's fetch.py into work/bin."""
    bins = work / 'bin'
    if (bins / 'fetch.py').exists():
        return
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        sys.exit(f'the binaries are x86_64 Linux only, not {platform.system()} {platform.machine()}')
    tgz = work / 'marklive-repro.tar.gz'
    print(f'+ download {BUNDLE_URL}', flush=True)
    with urllib.request.urlopen(BUNDLE_URL) as r, open(tgz, 'wb') as f:
        shutil.copyfileobj(r, f)
    digest = hashlib.sha256(tgz.read_bytes()).hexdigest()
    if digest != BUNDLE_SHA256:
        sys.exit(f'{tgz}: sha256 {digest}, expected {BUNDLE_SHA256}')
    bins.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tgz) as t:
        for m in t.getmembers():
            if m.isfile():
                dst = bins / Path(m.name).name
                dst.write_bytes(t.extractfile(m).read())
                dst.chmod(0o755)
    tgz.unlink()


def fetch(work, workloads):
    bins, corpora = work / 'bin', work / 'corpora'
    dirs = {}
    for w in workloads:
        cap = w.removesuffix('-ed')
        if not (corpora / cap).exists() or not list((corpora / cap).rglob('response.txt')):
            run([sys.executable, bins / 'fetch.py', '--public', '--extract', '--no-cache',
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
    drain = sum(e['dur'] for e in ev if e.get('name') == 'markLive drain' and e.get('ph') == 'X') / 1000
    h = hashlib.sha256()
    with open(out, 'rb') as f:
        while b := f.read(1 << 22):
            h.update(b)
    out.unlink()
    trace.unlink()
    return dict(wall=wall, ml=ml, drain=drain, cpu=cpu, rss=rss_mib, hash=h.hexdigest()), stderr


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
    ap.add_argument('--scratch', type=Path, help='output directory for links; default: a tmpfs under /dev/shm on Linux')
    ap.add_argument('--resume', action='store_true',
                    help='keep earlier results in --work and redo only unfinished workloads')
    ap.add_argument('--report-only', action='store_true', help='rewrite report.md from saved results')
    a = ap.parse_args()
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    threads = [int(t) for t in a.threads.split(',')]
    workloads = a.workloads.split(',')
    bins = work / 'bin'
    rows, statsf, donef = work / 'samples.jsonl', work / 'stats.jsonl', work / 'done.txt'
    rng = random.Random(20261110)

    def load(path):
        return [json.loads(l) for l in open(path)] if path.exists() else []

    if not a.report_only:
        if not a.resume:
            for f in (rows, statsf, donef):
                f.unlink(missing_ok=True)
        done = set(donef.read_text().split()) if donef.exists() else set()
        # A workload whose timing was interrupted is redone from scratch.
        kept = [r for r in load(rows) if r['w'] in done]
        rows.write_text(''.join(json.dumps(r) + '\n' for r in kept))
        have_stats = {s['w'] for s in load(statsf)}
        download_bundle(work)
        # fetch.py runs zstd; use the bundled static one.
        os.environ['PATH'] = f'{bins}{os.pathsep}{os.environ.get("PATH", "")}'
        dirs = fetch(work, workloads)
        scratch = a.scratch or (Path('/dev/shm') / f'marklive-bench-{os.getpid()}'
                                if Path('/dev/shm').is_dir() else work / 'scratch')
        scratch.mkdir(parents=True, exist_ok=True)
        combos = [(v, t) for v in VARIANTS for t in threads]
        try:
            for w in workloads:
                extra = WORKLOADS.get(w, [])
                if w not in done:
                    for v, t in combos * a.warmups:
                        link(bins / f'ld64.lld-{v}', dirs[w], extra, t, scratch)
                    for i in range(a.rounds):
                        order = combos[:]
                        rng.shuffle(order)
                        for v, t in order:
                            m, _ = link(bins / f'ld64.lld-{v}', dirs[w], extra, t, scratch)
                            with open(rows, 'a') as f:
                                f.write(json.dumps(dict(w=w, v=v, t=t, round=i, **m)) + '\n')
                        print(f'{w} round {i + 1}/{a.rounds} {time.strftime("%H:%M:%S")}', flush=True)
                    with open(donef, 'a') as f:
                        f.write(w + '\n')
                if w not in have_stats:
                    env = dict(os.environ, LLD_MARKLIVE_STATS='1')
                    for v, t in combos:
                        _, err = link(bins / f'ld64.lld-{v}-stats', dirs[w], extra, t, scratch, env)
                        for line in err.splitlines():
                            if line.startswith('marklive-stats '):
                                kv = dict(x.split('=', 1) for x in line.split()[1:])
                                with open(statsf, 'a') as f:
                                    f.write(json.dumps(dict(w=w, v=v, t=t, **kv)) + '\n')
                print(f'{w} done {time.strftime("%H:%M:%S")}', flush=True)
        finally:
            if not a.scratch:
                shutil.rmtree(scratch, ignore_errors=True)

    samples, stats = load(rows), load(statsf)
    order = list(WORKLOADS) + sorted({s['w'] for s in samples} - set(WORKLOADS))
    workloads = [w for w in order if any(s['w'] == w for s in samples)]
    threads = sorted({s['t'] for s in samples})
    rounds = max((s['round'] for s in samples), default=-1) + 1

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
           f'- {rounds} rounds; CIs are 95% bootstrap intervals of the '
           f'difference of means against `{BASE}`.', '']
    for w in workloads:
        hashes = {s['hash'] for s in samples if s['w'] == w}
        out += [f'## {w}', '', f'Distinct output hashes: {len(hashes)}'
                + ('' if len(hashes) == 1 else ' (OUTPUT DIFFERS)'), '']
        for metric, label, scale, unit in (('drain', 'markLive drain', 1, 'ms'), ('ml', 'markLive', 1, 'ms'),
                                           ('wall', 'wall', 1, 's'),
                                           ('cpu', 'CPU (user+sys)', 1, 's'), ('rss', 'peak RSS', 1, 'MiB')):
            if not all(metric in s for s in samples if s['w'] == w):
                continue
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
