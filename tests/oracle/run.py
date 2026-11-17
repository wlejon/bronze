#!/usr/bin/env python3
"""The oracle: every case under cases/ through the compiler, against its .expected.

run.py [filter ...] [--bronze PATH] [--modes run,aot,profile,switch,adversarial] [--jobs N] [--work DIR]

A case is cases/<name>.js with cases/<name>.expected (the program's stdout).
A filter selects the cases whose name contains it; none selects every case.
Modes (the default is every one but adversarial):
  run      bronze run <case>                       (in-process compile)
  aot      bronze build, then the executable       (generic IL)
  profile  a --harvest-out build is run to write its harvest, then a
           --feedback-in build on that harvest     (the optimiser)
  switch   only a case holding `const TRAINING = Math.random() < 0.5;` that
           writes a line `TRAINING` to stderr when it trained: its
           --harvest-out build runs until a run trained (its training arm
           alone), and the --feedback-in build on that harvest until a run
           did not, so that run takes arms the harvest never saw
           (speculation's exits, retirement, the optimistic copies of
           opt/speculate optimistic.h). A harvest is of one source text,
           which is why the arm is drawn at run time. The fed run has
           BRONZE_DEOPT_STATS=1; its report is kept in the work dir as
           deopt.txt when the case fails. The other modes of such a case
           also take a run that did not train.
  adversarial  the profile mode's harvest (one harvest run serves both) made
           WRONG three ways (PROFILES), a --feedback-in build on each and its
           run, which must print the pinned bytes and exit as the harvest run
           did: "a wrong profile costs speed, never an answer"
           (docs/type-feedback.md). Each perturbed profile is well-formed and
           of this build (its digest kept), so the reader takes it; a build
           that fails on one fails the case. The perturbations (PROFILES,
           below) are seeded by the case's name, so a failure reproduces: the
           work dir keeps adv<k>.tfb and deopt<k>.txt of a failing case. The
           summary counts the perturbed builds that speculated something
           (--opt-stats) and the guard exits their runs took.
Prints one line per failing (case, mode) with the first differing line, then
the pass counts. Exit status 1 when anything failed, 2 when a filter matched
no case.
"""
import concurrent.futures as cf
import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import zlib

here = pathlib.Path(__file__).resolve().parent
args = sys.argv[1:]


def option(name, default):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return value
    return default


bronze = os.path.abspath(option('--bronze', str(here.parent.parent / 'build/src/cli/bronze')))  # cases run in their own folders
modes = option('--modes', 'run,aot,profile,switch,adversarial').split(',')
jobs = int(option('--jobs', '12'))
work = pathlib.Path(option('--work', '/tmp/bronze-oracle'))
every = sorted(p.stem for p in (here / 'cases').glob('*.js') if p.with_suffix('.expected').exists())
unmatched = [f for f in args if not any(f in n for n in every)]
if unmatched:
    print('no case under ' + str(here / 'cases') + ' has a name containing: ' + ', '.join(unmatched))
    sys.exit(2)
names = [n for n in every if not args or any(f in n for f in args)]


def sh(cmd, cwd, timeout=120, env=None):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, errors='replace', env=env)
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


MARKER = 'const TRAINING = Math.random() < 0.5;'


def switching(src):
    return MARKER in src.read_text(errors='replace')


def real(cmd, src, env=None):
    """A run of the case; of a switching case, one that did not train."""
    for _ in range(40 if switching(src) else 1):
        rc, out, err = sh(cmd, src.parent, env=env)
        if 'TRAINING' not in err.splitlines():
            break
    return rc, out, err


def switch_case(name, src, d, exe, tfb, build, check):
    ok, why = build(['--harvest-out', str(tfb)])
    if not ok:
        return [(name, 'switch', 'harvest build: ' + why)]
    for _ in range(40):
        tfb.unlink(missing_ok=True)
        rc, out, err = sh([str(exe)], src.parent)
        if 'TRAINING' in err.splitlines() and tfb.exists():
            break
    else:
        return [(name, 'switch', 'no training run')]
    ok, why = build(['--feedback-in', str(tfb)])
    if not ok:
        return [(name, 'switch', 'build: ' + why)]
    rc, out, err = real([str(exe)], src, dict(os.environ, BRONZE_DEOPT_STATS='1'))
    if 'TRAINING' in err.splitlines():
        return [(name, 'switch', 'no run off the training arm')]
    (d / 'deopt.txt').write_text(err)
    check('switch', rc, out, err)
    return []


# ---- adversarial profiles ---------------------------------------------------
# A harvest (docs/type-feedback.md "Harvest"; src/opt/spec/profile.cpp reads
# it) is rows by IC index and slow-site index. Harvest holds it as fields,
# names still escaped as the file has them, and writes it back in the order
# the reader wants.

class Harvest:
    def __init__(self, text):
        self.rows = []  # header, `harvest`, and any row kind not modelled
        self.slowsites = None
        self.slow, self.last, self.callee, self.samples = {}, {}, {}, {}
        self.ics = {}  # index -> {'method', 'flags', 'ways': [[form, receiver, extra...]], 'other'}
        self.held = {'doubles': {}, 'objects': {}}
        self.other = {'mixed': [], 'nonobject': []}
        self.functions = []  # the module's IL function names (p_hot), set by the caller
        for line in text.splitlines():
            f = line.split('\t')
            k = f[0]
            if k == 'slowsites':
                self.slowsites = f[1:]
            elif k in ('slow', 'last', 'callee', 'sample'):
                getattr(self, {'sample': 'samples'}.get(k, k))[f[1] if k == 'sample' else int(f[1])] = \
                    int(f[2]) if k in ('slow', 'sample') else f[2]
            elif k == 'ic':
                self.ics[int(f[1])] = {'method': f[2] == 'method', 'flags': [] if f[3] == '-' else f[3].split(','),
                                       'ways': [], 'other': 0}
            elif k == 'way':
                self.ics[int(f[1])]['ways'].append(f[2:])
            elif k == 'other':
                self.ics[int(f[1])]['other'] = int(f[2])
            elif k in self.held:
                self.held[k][f[1]] = [int(x) for x in f[2].split(',')]
            elif k in self.other:
                self.other[k].append(f[1])
            else:
                self.rows.append(line)

    def armed(self):
        """The slow-site indices the harvested build counted."""
        if not self.slowsites:
            return []
        n, bits = int(self.slowsites[0]), self.slowsites[1]
        return [i for i in range(n) if (int(bits[i // 64 * 16:i // 64 * 16 + 16], 16) >> (i % 64)) & 1]

    def text(self):
        out = list(self.rows)
        if self.slowsites:
            out.append('slowsites\t' + '\t'.join(self.slowsites))
            n = int(self.slowsites[0])
            out += [f'slow\t{i}\t{c}' for i, c in sorted(self.slow.items()) if c > 0 and i < n]
            out += [f'last\t{i}\t{v}' for i, v in sorted(self.last.items()) if i < n]
            out += [f'callee\t{i}\t{v}' for i, v in sorted(self.callee.items()) if i < n]
        for i, s in sorted(self.ics.items()):
            out.append(f'ic\t{i}\t{"method" if s["method"] else "property"}\t{",".join(s["flags"]) or "-"}')
            out += ['\t'.join(['way', str(i)] + w) for w in s['ways']]
            if s['other']:
                out.append(f'other\t{i}\t{s["other"]}')
        out += [f'sample\t{h}\t{c}' for h, c in sorted(self.samples.items())]
        for held, other in (('doubles', 'mixed'), ('objects', 'nonobject')):
            out += [f'{held}\t{r}\t{",".join(map(str, p))}' for r, p in sorted(self.held[held].items()) if p]
            out += [f'{other}\t{k}' for k in sorted(set(self.other[other]))]
        return '\n'.join(out) + '\n'


def keys_of(name):
    """A receiver name's head and key list (`Ctor#N{!a,b}`), or None for one
    with no list to change (a dictionary, a kind name like `array`)."""
    m = re.fullmatch(r'([^{}]*)\{([^{}]*)\}', name)
    if not m or m.group(2) == 'dict':
        return None
    return m.group(1), m.group(2).split(',') if m.group(2) else []


def reshape(name, r):
    """The receiver name with its keys moved, so every slot it implies is
    another key's, or its read-only marks, last key or capacity changed."""
    parts = keys_of(name)
    if parts is None:
        return name
    head, keys = parts
    keys = r.choice([
        lambda: keys[1:] + keys[:1],
        lambda: keys[::-1],
        lambda: [k.lstrip('!') for k in keys] if any(k.startswith('!') for k in keys) else ['!' + k for k in keys],
        lambda: keys[:-1],
        lambda: keys + ['adv'],
    ])()
    if r.random() < 0.2:
        head = head.split('#')[0] + r.choice(['', '#2', '#8'])
    return head + '{' + ','.join(keys) + '}'


def ways(h, method):
    return [w for s in h.ics.values() if s['method'] == method for w in s['ways']]


def callee_pool(h):
    """Every function name the harvest gives a callee by, and Math functions
    of one and two arguments: the names a wrong callee is drawn from."""
    names = {w[2] for w in ways(h, True)} | set(h.callee.values())
    names |= {x for w in ways(h, False) if w[0] == 'accessor' for x in w[2:]}
    names -= {'*other*'}
    names = {n for n in names if not n.startswith('native:')}
    return sorted(names | {'intrinsic:Math.sqrt', 'intrinsic:Math.max', 'intrinsic:Math.floor'})


# Each perturbation makes one kind of evidence wrong on a draw of the sites.
def p_receivers(h, r):
    """Receivers' key lists rotated or changed (every slot guess another
    key's), or borrowed from another site's ways."""
    pool = [w[1] for w in ways(h, False) + ways(h, True)]
    for s in h.ics.values():
        for w in s['ways']:
            if r.random() < 0.5:
                w[1] = r.choice(pool) if r.random() < 0.3 else reshape(w[1], r)


def p_forms(h, r):
    """A way's form exchanged: own / proto / absent / accessor / store /
    array, direct / slot / native."""
    pool = callee_pool(h)
    prop = {
        'own': ['proto', 'absent', 'accessor', 'store'], 'store': ['own', 'proto'], 'proto': ['own', 'absent', 'accessor'],
        'absent': ['own', 'proto', 'accessor'], 'accessor': ['own', 'proto', 'absent'], 'array': ['own', 'absent'],
    }
    for s in h.ics.values():
        for i, w in enumerate(s['ways']):
            if r.random() >= 0.4:
                continue
            if s['method']:
                s['ways'][i] = [r.choice(['direct', 'slot']), w[1], r.choice(pool + ['*other*'])]
                continue
            form = r.choice(prop[w[0]])
            extra = {'proto': [str(r.randint(1, 3)), str(r.randint(0, 3))],
                     'accessor': [r.choice(pool + ['*other*']), r.choice(pool + ['*other*'])]}.get(form, [])
            s['ways'][i] = [form, w[1]] + extra


def p_places(h, r):
    """Proto ways' holder depth and slot moved."""
    for w in ways(h, False):
        if w[0] == 'proto' and len(w) == 4 and r.random() < 0.7:
            w[2] = str(max(1, int(w[2]) + r.choice([-1, 1, 2])))
            w[3] = str(r.choice([0, int(w[3]) + 1, max(0, int(w[3]) - 1)]))


def p_reprs(h, r):
    """Slot representations flipped: a receiver's double positions claimed
    as object ones and the reverse, receivers with none claimed all-double,
    and the keys found holding other values forgotten."""
    d, o = h.held['doubles'], h.held['objects']
    for name in sorted(set(d) | set(o)):
        if r.random() < 0.6:
            d[name], o[name] = o.get(name, []), d.get(name, [])
    for w in ways(h, False):
        parts = keys_of(w[1])
        if parts and parts[1] and w[1] not in d and r.random() < 0.3:
            d[w[1]] = list(range(len(parts[1])))
    for k in h.other:
        h.other[k] = []


def p_unseen(h, r):
    """Named sites and calls that ran made to look as if they never had: the
    last receiver or callee and its turns (il/feedback_site.h kLastShapeArm,
    kShapeTurnArm) and a call's count (kCallCountArm) dropped, so the blocks
    holding them read as never entered (opt/speculate never_entered.cpp)."""
    for i in list(h.last) + list(h.callee):
        if r.random() < 0.5:
            h.last.pop(i, None)
            h.callee.pop(i, None)
            h.slow.pop(i + 1, None)
            h.slow.pop(i - 5, None)


def p_monomorphic(h, r):
    """Every site one way, no `full`, `mega` or `other`: past every
    threshold on one guess."""
    for s in h.ics.values():
        if s['ways']:
            s['ways'] = [r.choice(s['ways'])]
        s['flags'], s['other'] = [], 0


def p_callees(h, r):
    """Callees exchanged between method ways and call sites (other arities,
    Math functions), each call made to look like it entered one callee once
    and never ran its helper."""
    pool = callee_pool(h)
    for w in ways(h, True):
        if r.random() < 0.6:
            w[2] = r.choice(pool)
    for i in h.callee:
        h.callee[i] = r.choice(pool)
        h.slow[i + 1] = 1  # kShapeTurnArm: one turn
        h.slow.pop(i - 6, None)  # the call's own index: the helper never ran


def p_arms(h, r):
    """Slow-site counts toggled: a fallback that ran made silent (its guess
    a leave), an arm that never ran given its neighbours' count (a double
    where ints flowed, another element kind, an Array where views came)."""
    for i in h.armed():
        if r.random() < 0.3:
            if i in h.slow:
                del h.slow[i]
            else:
                h.slow[i] = max([h.slow.get(j, 1) for j in range(i - 7, i + 8)])


def p_borrowed(h, r):
    """Rows of one site given to another: whole property and method sites
    exchanged, and a run of slow-site counts moved one instruction on."""
    for method in (False, True):
        idx = [i for i, s in h.ics.items() if s['method'] == method]
        for i in idx:
            if r.random() < 0.3:
                j = r.choice(idx)
                h.ics[i], h.ics[j] = h.ics[j], h.ics[i]
    if h.slow:
        lo = r.choice(sorted(h.slow))
        moved = {i + 8 if lo <= i < lo + 400 else i: c for i, c in h.slow.items()}
        h.slow = moved


def p_lifetimes(h, r):
    """Allocation sites' lifetimes reversed (il/feedback_site.h
    kAllocCountArm, kAllocTenuredArm; docs/type-feedback.md "Pretenuring"):
    a site whose objects died claimed to have had every one promoted, past
    the threshold, so its garbage is allocated old, and one whose objects
    lived claimed to have had none promoted. Every pair of armed neighbours
    stands in for one."""
    armed = set(h.armed())
    for i in sorted(armed):
        if i + 1 not in armed or r.random() >= 0.6:
            continue
        made = h.slow.get(i, 0)
        if made and h.slow.get(i + 1, 0) * 5 >= made * 4:
            h.slow.pop(i + 1, None)
        else:
            h.slow[i] = max(made, 256)
            h.slow[i + 1] = h.slow[i]


def p_hot(h, r):
    """Every function sampled hot (a harvest names a function by the FNV-1a
    hash of its IL name, il/feedback_site.h functionNameHash), so every site
    with evidence is speculated, not only those of a run long enough to be
    sampled."""
    for name in h.functions:
        x = 2166136261
        for c in name.encode():
            x = ((x ^ c) * 16777619) & 0xffffffff
        h.samples[f'{x:08x}'] = max(h.samples.get(f'{x:08x}', 0), 64)


PROFILES = [
    [p_receivers, p_forms, p_places, p_reprs, p_unseen, p_hot],
    [p_monomorphic, p_callees, p_lifetimes, p_hot],
    [p_arms, p_borrowed, p_hot],
]


def adversarial(name, src, d, exe, tfb, want, want_rc):
    """The case on each perturbed profile: (failures, builds that speculated
    something, guard exits)."""
    fails, speculated, exits = [], 0, 0
    # The module's functions by IL name, as the harvested build named them
    # (`bronze il` runs the passes a build without a profile runs).
    rc, il, err = sh([bronze, 'il', str(src)], d, 300)
    if rc != 0:
        return [(name, 'adversarial', 'il: ' + (err.strip().splitlines() or [''])[-1][:160])], 0, 0
    functions = re.findall(r'^func (\S+)\(', il, re.M)
    for k, perturbations in enumerate(PROFILES):
        h = Harvest(tfb.read_text(errors='replace'))
        h.functions = functions
        r = random.Random(zlib.crc32(f'{name}/{k}'.encode()))
        for p in perturbations:
            p(h, r)
        bad = d / f'adv{k}.tfb'
        bad.write_text(h.text())
        rc, out, err = sh([bronze, 'build', str(src), '-o', str(exe), '--feedback-in', str(bad), '--opt-stats'], d, 300)
        if rc != 0:
            tail = (err or out).strip().splitlines()[-1][:160] if (err or out).strip() else ''
            fails.append((name, 'adversarial', f'profile {k}: build rc={rc}: {tail}'))
            continue
        m = re.search(r'^speculate: (\d+)/', out + err, re.M)
        speculated += bool(m and int(m.group(1)))
        rc, out, err = real([str(exe)], src, dict(os.environ, BRONZE_DEOPT_STATS='1'))
        (d / f'deopt{k}.txt').write_text(err)
        exits += sum(int(x) for x in re.findall(r'^deopt: (\d+) exits', err, re.M))
        why = differs(out, want)
        if why is None and rc != want_rc:
            why = f'exit status {rc}, the harvest run\'s {want_rc}'
        if why is not None:
            said = [x for x in err.splitlines() if x.strip() and not x.startswith(('deopt:', ' '))]
            tail = said[-1][:120] if said else ''
            fails.append((name, 'adversarial', f'profile {k}: rc={rc} {why} {tail}'))
    return fails, speculated, exits


adv_counts = []  # per case (builds that speculated, guard exits)


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
        check('run', *real([bronze, 'run', str(src)], src))
    if 'aot' in modes:
        ok, why = build([])
        if ok:
            check('aot', *real([str(exe)], src))
        else:
            fails.append((name, 'aot', 'build: ' + why))
    fed = [m for m in ('profile', 'adversarial') if m in modes]
    if fed:
        # One harvest serves the profile and adversarial modes.
        ok, why = build(['--harvest-out', str(tfb)])
        harvest_rc = sh([str(exe)], src.parent)[0] if ok else None
        for mode in fed:
            if not ok:
                fails.append((name, mode, 'harvest build: ' + why))
            elif not tfb.exists():
                fails.append((name, mode, 'no harvest written'))
            elif mode == 'adversarial':
                bad, speculated, exits = adversarial(name, src, d, exe, tfb, want, harvest_rc)
                fails.extend(bad)
                adv_counts.append((speculated, exits))
            else:
                built, why = build(['--feedback-in', str(tfb)])
                if built:
                    check('profile', *real([str(exe)], src))
                else:
                    fails.append((name, 'profile', 'build: ' + why))
    if 'switch' in modes and switching(src):
        fails.extend(switch_case(name, src, d, exe, tfb, build, check))
    if not any(f[1] in ('switch', 'adversarial') for f in fails):
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
    n = len(names)
    if mode == 'switch':
        n = sum(1 for x in names if switching(here / 'cases' / (x + '.js')))
    print(f'{mode}: {n - bad}/{n} pass')
if adv_counts:
    print(f'adversarial: {sum(s for s, _ in adv_counts)}/{len(adv_counts) * len(PROFILES)} perturbed builds '
          f'speculated, {sum(e for _, e in adv_counts)} guard exits')
sys.exit(1 if failed else 0)
