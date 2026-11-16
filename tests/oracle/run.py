#!/usr/bin/env python3
"""The oracle: every case under cases/ through the compiler, against its .expected.

run.py [name ...] [--bronze PATH] [--modes run,aot,profile] [--jobs N] [--work DIR]

A case is cases/<name>.js with cases/<name>.expected (the program's stdout).
Modes:
  run      bronze run <case>                       (in-process compile)
  aot      bronze build, then the executable       (generic IL)
  profile  a --harvest-out build is run to write its harvest, then a
           --feedback-in build on that harvest     (the optimiser)
Prints one line per failing (case, mode) with the first differing line, then
the pass counts. Exit status 1 when anything failed.
"""
import concurrent.futures as cf
import pathlib
import shutil
import subprocess
import sys

here = pathlib.Path(__file__).resolve().parent
args = sys.argv[1:]


def option(name, default):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return value
    return default


bronze = option('--bronze', str(here.parent.parent / 'build/src/cli/bronze'))
modes = option('--modes', 'run,aot,profile').split(',')
jobs = int(option('--jobs', '12'))
work = pathlib.Path(option('--work', '/tmp/bronze-oracle'))
names = args or sorted(p.stem for p in (here / 'cases').glob('*.js') if p.with_suffix('.expected').exists())


def sh(cmd, cwd, timeout=120):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, errors='replace')
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, '', 'timeout'


def differs(out, want):
    a, b = out.splitlines(), want.splitlines()
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else '<end>'
        y = b[i] if i < len(b) else '<end>'
        if x != y:
            return f'line {i + 1}: got {x[:70]!r} want {y[:70]!r}'
    return None


def case(name):
    src = here / 'cases' / (name + '.js')
    want = src.with_suffix('.expected').read_text(errors='replace')
    d = work / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    exe = d / 'case.exe'
    tfb = d / 'case.tfb'
    fails = []

    def check(mode, rc, out, err):
        why = differs(out, want)
        if why is not None:
            tail = err.strip().splitlines()[-1][:120] if err.strip() else ''
            fails.append((name, mode, f'rc={rc} {why} {tail}'))

    def build(extra):
        rc, out, err = sh([bronze, 'build', str(src), '-o', str(exe)] + extra, d, 300)
        return rc == 0, (err or out).strip().splitlines()[-1][:160] if rc != 0 and (err or out).strip() else ''

    if 'run' in modes:
        check('run', *sh([bronze, 'run', str(src)], src.parent))
    if 'aot' in modes:
        ok, why = build([])
        if ok:
            check('aot', *sh([str(exe)], src.parent))
        else:
            fails.append((name, 'aot', 'build: ' + why))
    if 'profile' in modes:
        ok, why = build(['--harvest-out', str(tfb)])
        if ok:
            sh([str(exe)], src.parent)
            if tfb.exists():
                ok, why = build(['--feedback-in', str(tfb)])
                if ok:
                    check('profile', *sh([str(exe)], src.parent))
                else:
                    fails.append((name, 'profile', 'build: ' + why))
            else:
                fails.append((name, 'profile', 'no harvest written'))
        else:
            fails.append((name, 'profile', 'harvest build: ' + why))
    shutil.rmtree(d, ignore_errors=True)
    return fails


failed = []
with cf.ThreadPoolExecutor(jobs) as pool:
    for fails in pool.map(case, names):
        failed.extend(fails)
for name, mode, why in sorted(failed):
    print(f'FAIL {mode:8s} {name}: {why}')
for mode in modes:
    bad = sum(1 for f in failed if f[1] == mode)
    print(f'{mode}: {len(names) - bad}/{len(names)} pass')
sys.exit(1 if failed else 0)
