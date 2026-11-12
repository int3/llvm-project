#!/usr/bin/env python3
"""Time lld-macho's markLive drain: donation without threshold, donation with
the 256-visit threshold, and work stealing.

usage: marklive-sweep.py [--work DIR]

Links the four PR workloads at 1/16/64/128/224 threads, 8 shuffled rounds
after one warmup, with glibc's top_pad raised. Writes DIR/samples.jsonl;
send that file back. Reuses DIR/bin and DIR/corpora if they exist.
"""
import argparse, gzip, hashlib, json, os, platform, random, shlex, shutil
import subprocess, sys, tarfile, time, urllib.request
from pathlib import Path

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


THRESH_URL = ('https://github.com/int3/llvm-project/releases/download/'
              'marklive-repro-v1/ld64.lld-thresh.gz')
THRESH_SHA256 = 'bff89ab9c447f6028ca5403067adb49ff14ad32af667f533b61927281ff3ee2e'
VARIANTS = ['levels', 'thresh', 'steal']
WORKLOADS = ['chromium-macos-sym1', 'firefox-macos', 'firefox-ios', 'rust-debug']
THREADS = [1, 16, 64, 128, 224]
ROUNDS, WARMUPS = 8, 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', type=Path, default=Path.home() / 'marklive-bench')
    work = ap.parse_args().work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    bins = work / 'bin'
    download_bundle(work)
    thresh = bins / 'ld64.lld-thresh'
    if not thresh.exists() or hashlib.sha256(thresh.read_bytes()).hexdigest() != THRESH_SHA256:
        print(f'+ download {THRESH_URL}', flush=True)
        with urllib.request.urlopen(THRESH_URL) as r:
            data = gzip.decompress(r.read())
        if hashlib.sha256(data).hexdigest() != THRESH_SHA256:
            sys.exit('ld64.lld-thresh: sha256 mismatch')
        thresh.write_bytes(data)
        thresh.chmod(0o755)
    os.environ['PATH'] = f'{bins}{os.pathsep}{os.environ.get("PATH", "")}'
    dirs = fetch(work, WORKLOADS)
    env = dict(os.environ, GLIBC_TUNABLES='glibc.malloc.top_pad=268435456')
    scratch = Path('/dev/shm') / f'marklive-sweep-{os.getpid()}'
    scratch.mkdir(parents=True)
    rows = work / 'samples.jsonl'
    rows.unlink(missing_ok=True)
    rng = random.Random(20261112)
    combos = [(v, t) for v in VARIANTS for t in THREADS]
    try:
        for w in WORKLOADS:
            for v, t in combos * WARMUPS:
                link(bins / f'ld64.lld-{v}', dirs[w], [], t, scratch, env)
            for i in range(ROUNDS):
                order = combos[:]
                rng.shuffle(order)
                for v, t in order:
                    m, _ = link(bins / f'ld64.lld-{v}', dirs[w], [], t, scratch, env)
                    with open(rows, 'a') as f:
                        f.write(json.dumps(dict(w=w, v=v, t=t, round=i, **m)) + '\n')
                print(f'{w} round {i + 1}/{ROUNDS} {time.strftime("%H:%M:%S")}', flush=True)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f'DONE: send {rows}')


main()
