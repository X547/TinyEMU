#!/usr/bin/env python3
#
# Device test suite runner: builds the images, runs them on temu for each
# target and machine, restarts after a test that kills the machine, and
# compares characterization snapshots with their golden copies.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#

import argparse
import concurrent.futures
import fnmatch
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import covreport

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PROTOCOL_VERSION = 1


class Target:
    def __init__(self, name, arch, machine, root, boot):
        self.name = name
        self.arch = arch          # build directory and cross file
        self.machine = machine    # temu's "machine"
        self.root = root          # the root bus
        self.boot = boot          # 'bios', 'kernel-sbi' or 'kernel'


def image_path(arch):
    """temu loads a RISC-V image flat and an x86 one as a PVH ELF."""
    ext = 'bin' if arch.startswith('riscv') else 'elf'
    return os.path.join(HERE, 'build-' + arch, 'devtests.' + ext)


TARGETS = {t.name: t for t in [
    Target('riscv64-m', 'riscv64', 'riscv64', 'fdt', 'bios'),
    Target('riscv64-s', 'riscv64', 'riscv64', 'fdt', 'kernel-sbi'),
    Target('riscv32-m', 'riscv32', 'riscv32', 'fdt', 'bios'),
    Target('riscv32-s', 'riscv32', 'riscv32', 'fdt', 'kernel-sbi'),
    Target('i686', 'i686', 'pc', 'acpi', 'kernel'),
    Target('x86_64', 'x86_64', 'pc', 'acpi', 'kernel'),
]}

VERDICTS = ('PASS', 'FAIL', 'SKIP', 'CRASH', 'TIMEOUT')


class TestResult:
    def __init__(self, name):
        self.name = name
        self.verdict = None
        self.us = 0
        self.reason = ''
        self.fails = []
        self.logs = []
        self.snaps = []
        self.crash = ''
        self.known = False
        self.golden = ''      # '', 'match', 'new', 'mismatch'
        self.golden_diff = []

    def to_json(self):
        return {k: v for k, v in self.__dict__.items()}


class Entry:
    """One target on one machine: one or more temu runs."""

    def __init__(self, target, machine):
        self.target = target
        self.machine = machine
        self.results = {}     # name -> TestResult, in the order run
        self.restarts = 0
        self.boot_error = ''
        self.hello = ''
        self.listed = []

    @property
    def key(self):
        return '%s/%s' % (self.target.name, self.machine)

    def counts(self):
        c = {v: 0 for v in VERDICTS}
        for r in self.results.values():
            c[r.verdict] = c.get(r.verdict, 0) + 1
        return c


# ----------------------------------------------------------------------------
# configuration

def machine_targets(path):
    with open(path) as f:
        m = re.search(r'targets:\s*([^\n*]+)', f.read())
    return m.group(1).split() if m else list(TARGETS)


def write_config(template, target, image, cmdline, fixtures, out_dir):
    with open(template) as f:
        text = f.read()
    rel = os.path.relpath(image, out_dir).replace(os.sep, '/')
    boot = {
        'bios': 'bios: "%s"' % rel,
        'kernel-sbi': 'kernel: "%s", sbi: "native"' % rel,
        'kernel': 'kernel: "%s"' % rel,
    }[target.boot]
    subst = {
        '@MACHINE@': target.machine,
        '@BOOT@': boot,
        '@CMDLINE@': cmdline,
        '@ROOT@': target.root,
        '@FIXTURE@': '{ type: "test-control" },' if fixtures else '',
    }
    for k, v in subst.items():
        text = text.replace(k, v)
    path = os.path.join(out_dir, 'machine.cfg')
    with open(path, 'w') as f:
        f.write(text)
    return path


# ----------------------------------------------------------------------------
# running temu

class Run:
    """One temu process, read line by line."""

    def __init__(self, temu, args, cfg, cwd, log, env):
        self.proc = subprocess.Popen(
            [temu] + args + [os.path.basename(cfg)], cwd=cwd, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        self.lines = queue.Queue()
        self.log = log
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            self.log.write(line + '\n')
            self.lines.put(line)
        self.lines.put(None)

    def stop(self):
        """Close the console, which stops temu, then insist."""
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(3)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
        return self.proc.returncode


def parse_line(line):
    i = line.find('@@T ')
    if i < 0:
        return None, ''
    rest = line[i + 4:]
    word, _, args = rest.partition(' ')
    return word, args


def run_once(opts, entry, cfg_dir, cmdline, log):
    """One temu run. Returns (finished, current test or None, why)."""
    cfg = write_config(opts.machine_files[entry.machine], entry.target,
                       opts.images[entry.target.arch], cmdline,
                       opts.fixtures, cfg_dir)
    run = Run(opts.temu, opts.temu_args, cfg, cfg_dir, log, opts.env)
    current = None
    why = ''
    finished = False
    try:
        while True:
            try:
                line = run.lines.get(timeout=opts.timeout)
            except queue.Empty:
                why = 'timeout'
                break
            if line is None:
                break
            word, args = parse_line(line)
            if word is None:
                continue
            if word == 'HELLO':
                entry.hello = args
                if not args.startswith('%d ' % PROTOCOL_VERSION):
                    why = 'protocol %s' % args.split(' ')[0]
                    break
            elif word == 'TEST':
                entry.listed.append(args.strip())
            elif word == 'BEGIN':
                current = TestResult(args.strip())
                entry.results[current.name] = current
            elif word == 'FAIL' and current:
                current.fails.append(args)
            elif word == 'LOG' and current:
                current.logs.append(args)
            elif word == 'SNAP' and current:
                key, _, value = args.partition(' ')
                current.snaps.append((key, value))
            elif word == 'CRASH' and current:
                current.crash = args
            elif word == 'END' and current:
                parts = args.split(' ', 3)
                current.verdict = parts[1]
                current.us = int(parts[2]) if len(parts) > 2 else 0
                current.reason = parts[3] if len(parts) > 3 else ''
                current = None
            elif word == 'PANIC':
                why = 'panic: ' + args
            elif word == 'DONE':
                finished = True
    finally:
        code = run.stop()
    if not finished and not why:
        why = 'exit %s' % code
    return finished, current, why


def run_entry(opts, entry):
    work = os.path.join(opts.work, entry.target.name, entry.machine)
    os.makedirs(work, exist_ok=True)
    base = []
    if opts.meta:
        base.append('meta=1')
    if opts.tests:
        base.append('tests=' + ','.join(opts.tests))
    if opts.skip:
        base.append('skip=' + ','.join(opts.skip))

    def run_alone(name):
        """One test in a machine of its own; False when it did not boot."""
        finished, current, why = run_once(
            opts, entry, work,
            ' '.join([a for a in base if not a.startswith('tests=')]
                     + ['tests=' + name]), log)
        if current is not None:
            _killed(current, why)
        elif not finished and name not in entry.results:
            entry.boot_error = why
            return False
        return True

    with open(os.path.join(work, 'temu.log'), 'w') as log:
        if opts.list:
            finished, _, why = run_once(opts, entry, work,
                                        ' '.join(base + ['list=1']), log)
            if not finished:
                entry.boot_error = why
            return entry

        if opts.isolate:
            finished, _, why = run_once(opts, entry, work,
                                        ' '.join(base + ['list=1']), log)
            if not finished:
                entry.boot_error = why
                return entry
            names = entry.listed
            entry.listed = []
            for name in names:
                if not run_alone(name):
                    return entry
            return entry

        after = None
        while True:
            cmdline = ' '.join(base + (['after=' + after] if after else []))
            before = len(entry.results)
            finished, current, why = run_once(opts, entry, work, cmdline, log)
            if finished:
                break
            if current is not None:
                _killed(current, why)
                after = current.name
            elif len(entry.results) > before:
                # between two tests: go on after the last one that ended
                after = list(entry.results)[-1]
            else:
                entry.boot_error = why
                break
            entry.restarts += 1
            if entry.restarts > opts.max_restarts:
                entry.boot_error = 'more than %d restarts' % opts.max_restarts
                break

        # tests that need a machine no other test touched
        for r in [r for r in entry.results.values() if r.verdict == 'RERUN']:
            del entry.results[r.name]
            if not run_alone(r.name):
                break
    return entry


def _killed(result, why):
    result.verdict = 'TIMEOUT' if why == 'timeout' else 'CRASH'
    result.reason = why


# ----------------------------------------------------------------------------
# golden snapshots and known failures

def golden_paths(opts, machine, target, test):
    """Where a test's golden copy may be, the most particular first: for
    the machine and target, the target, the machine, or all of them."""
    g = opts.golden
    name = test + '.txt'
    return [os.path.join(g, machine, target, name),
            os.path.join(g, target, name),
            os.path.join(g, machine, name),
            os.path.join(g, name)]


def read_golden(path):
    snaps = []
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            if line.strip() and not line.startswith('#'):
                key, _, value = line.partition(' ')
                snaps.append((key, value))
    return snaps


def check_golden(opts, entry):
    for r in entry.results.values():
        if not r.snaps or r.verdict != 'PASS':
            continue
        paths = [p for p in golden_paths(opts, entry.machine,
                                         entry.target.name, r.name)
                 if os.path.exists(p)]
        if not paths:
            r.golden = 'new'
            continue
        path = paths[0]
        want = read_golden(path)
        got = [(k, v) for k, v in r.snaps]
        if want == got:
            r.golden = 'match'
            continue
        r.golden = 'mismatch'
        r.verdict = 'FAIL'
        r.reason = 'golden mismatch (%s)' % os.path.relpath(path, HERE)
        want_d, got_d = dict(want), dict(got)
        for k in sorted(set(want_d) | set(got_d)):
            if want_d.get(k) != got_d.get(k):
                r.golden_diff.append('%s: want %s, got %s' % (
                    k, want_d.get(k, '(none)'), got_d.get(k, '(none)')))


def update_golden(opts, entries):
    """The commonest snapshot of a test is the shared copy. A target, then
    a machine, whose runs all differ from it alike gets one of its own, and
    what is still left over one per machine and target, matching the order
    golden_paths() looks in."""
    by_test = {}
    for e in entries:
        for r in e.results.values():
            if r.snaps and r.verdict in ('PASS', 'FAIL') and not r.fails:
                by_test.setdefault(r.name, {})[(e.machine, e.target.name)] = \
                    tuple(r.snaps)
    g = opts.golden
    written = 0
    for test, runs in sorted(by_test.items()):
        name = test + '.txt'
        for m, t in runs:
            for p in golden_paths(opts, m, t, test)[:3]:
                if os.path.exists(p):
                    os.remove(p)
        counts = {}
        for snaps in runs.values():
            counts[snaps] = counts.get(snaps, 0) + 1
        shared = max(counts, key=lambda v: counts[v])

        def agreed(values):
            values = set(values)
            if len(values) == 1:
                v = values.pop()
                if v != shared:
                    return v
            return None

        by_target = {}
        for t in {t for _, t in runs}:
            v = agreed(s for (_, tt), s in runs.items() if tt == t)
            if v:
                by_target[t] = v
        by_machine = {}
        for m in {m for m, _ in runs}:
            rest = [s for (mm, t), s in runs.items()
                    if mm == m and t not in by_target]
            v = agreed(rest) if rest else None
            if v:
                by_machine[m] = v

        _write_golden(os.path.join(g, name), shared)
        written += 1
        for t, v in by_target.items():
            _write_golden(os.path.join(g, t, name), v)
            written += 1
        for m, v in by_machine.items():
            _write_golden(os.path.join(g, m, name), v)
            written += 1
        for (m, t), snaps in runs.items():
            effective = by_target.get(t) or by_machine.get(m) or shared
            if snaps != effective:
                _write_golden(os.path.join(g, m, t, name), snaps)
                written += 1
    print('golden: wrote %d files under %s' % (written, os.path.relpath(g)))


def _write_golden(path, snaps):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        for k, v in snaps:
            f.write('%s %s\n' % (k, v))


def load_known(path):
    known = []
    if not os.path.exists(path):
        return known
    with open(path) as f:
        for line in f:
            line = line.split('#', 1)[0].split()
            if len(line) == 3:
                known.append(tuple(line))
    return known


def apply_known(known, entry):
    for r in entry.results.values():
        for t, m, n in known:
            if (fnmatch.fnmatchcase(entry.target.name, t) and
                    fnmatch.fnmatchcase(entry.machine, m) and
                    fnmatch.fnmatchcase(r.name, n)):
                r.known = True


# ----------------------------------------------------------------------------
# building

def build_images(opts, arches):
    """False when a build fails; its output has been shown."""
    for arch in sorted(arches):
        build = os.path.join(HERE, 'build-' + arch)
        steps = []
        if not os.path.isdir(build):
            steps.append(['meson', 'setup', build, '--cross-file',
                          os.path.join(HERE, 'cross', arch + '.cross')])
        steps.append(['ninja', '-C', build])
        for cmd in steps:
            p = subprocess.run(cmd, cwd=HERE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
            if p.returncode != 0:
                sys.stdout.write(p.stdout)
                print('build failed: %s' % ' '.join(cmd))
                return False
    return True


def find_temu():
    if os.environ.get('TEMU'):
        return os.environ['TEMU']
    for d in ('build-fixtures', 'build-linux-fixtures',
              'build-windows-fixtures'):
        for exe in ('temu', 'temu.exe'):
            p = os.path.join(ROOT, d, exe)
            if os.path.exists(p):
                return p
    return None


# ----------------------------------------------------------------------------
# reporting

def summarize(opts, entries):
    status = 0
    for e in entries:
        c = e.counts()
        known_fail = sum(1 for r in e.results.values()
                         if r.known and r.verdict not in ('PASS', 'SKIP'))
        unexpected = [r for r in e.results.values()
                      if r.known and r.verdict == 'PASS']
        bad = [r for r in e.results.values()
               if not r.known and r.verdict not in ('PASS', 'SKIP')]
        print('%-22s pass=%-4d fail=%-3d skip=%-3d crash=%-3d timeout=%-3d '
              'known=%-3d restarts=%d' % (
                  e.key, c['PASS'], c['FAIL'], c['SKIP'], c['CRASH'],
                  c['TIMEOUT'], known_fail, e.restarts))
        if e.boot_error:
            print('    boot: %s (log: %s)' % (e.boot_error, os.path.relpath(
                os.path.join(opts.work, e.target.name, e.machine, 'temu.log'))))
            status = 1
        for r in bad:
            status = 1
            print('    %s %s %s' % (r.verdict, r.name, r.reason))
            for f in r.fails[:10]:
                print('        ' + f)
            if r.crash:
                print('        ' + r.crash)
            for d in r.golden_diff[:10]:
                print('        ' + d)
            if opts.verbose:
                for l in r.logs[-10:]:
                    print('        log: ' + l)
        for r in unexpected:
            print('    unexpected pass: %s (in known-failures.txt)' % r.name)
        new = [r.name for r in e.results.values() if r.golden == 'new']
        if new and opts.verbose:
            print('    no golden copy: ' + ', '.join(new))
    return status


def write_json(opts, entries):
    data = [{
        'target': e.target.name, 'machine': e.machine, 'hello': e.hello,
        'restarts': e.restarts, 'boot_error': e.boot_error,
        'results': [r.to_json() for r in e.results.values()],
    } for e in entries]
    with open(os.path.join(opts.work, 'results.json'), 'w') as f:
        json.dump(data, f, indent=1)


# ----------------------------------------------------------------------------
# self test

SELFTEST_EXPECT = {
    'framework.a_pass': ('PASS', 0),
    'framework.b_fail_expect': ('FAIL', 2),
    'framework.c_fail_assert': ('FAIL', 1),
    'framework.d_skip': ('SKIP', 0),
    'framework.e_crash': ('CRASH', 0),
    'framework.f_heap_release': ('PASS', 0),
    'framework.g_hang': ('TIMEOUT', 0),
    'framework.h_power_off': ('CRASH', 0),
    'framework.i_last': ('PASS', 0),
}


def selftest(opts, entries):
    """Check that each meta test ended the way it is written to, that the
    runner got past the two that kill the machine, and that a snapshot is
    compared with its golden copy."""
    status = 0
    for e in entries:
        problems = []
        for name, (verdict, fails) in SELFTEST_EXPECT.items():
            r = e.results.get(name)
            if r is None:
                problems.append('%s did not run' % name)
                continue
            if r.verdict != verdict:
                problems.append('%s: %s, expected %s' % (name, r.verdict,
                                                         verdict))
            if len(r.fails) != fails:
                problems.append('%s: %d failures reported, expected %d' % (
                    name, len(r.fails), fails))
        r = e.results.get('framework.e_crash')
        if r and not r.crash:
            problems.append('framework.e_crash: no CRASH line')
        r = e.results.get('framework.d_skip')
        if r and r.reason != 'on purpose':
            problems.append('framework.d_skip: reason %r' % r.reason)
        r = e.results.get('framework.i_last')
        if r and r.logs != ['reached']:
            problems.append('framework.i_last: logs %r' % r.logs)
        r = e.results.get('framework.a_pass')
        if r and r.golden != 'match':
            problems.append('framework.a_pass: golden %s' % r.golden)
        if e.restarts != 2:
            problems.append('%d restarts, expected 2' % e.restarts)
        print('%-22s selftest %s' % (e.key, 'ok' if not problems else
                                     'FAILED'))
        for p in problems:
            print('    ' + p)
        status |= 1 if problems else 0
    return status


# ----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Run the device test suite on temu.')
    ap.add_argument('targets', nargs='*',
                    help='targets to run (default: all): ' +
                    ' '.join(TARGETS))
    ap.add_argument('--temu',
                    help='temu built with -Dtest_fixtures=true '
                    '(default: $TEMU or a build-*fixtures dir)')
    ap.add_argument('--temu-args', default='',
                    help='extra temu arguments, e.g. "-no-accel"')
    ap.add_argument('-m', '--machine', action='append', default=[],
                    help='machines/<name>.cfg to run on (default: all)')
    ap.add_argument('-t', '--tests', action='append', default=[],
                    help='test name globs to run')
    ap.add_argument('-s', '--skip', action='append', default=[],
                    help='test name globs to leave out')
    ap.add_argument('--timeout', type=float, default=30,
                    help='seconds without a line before a run is killed')
    ap.add_argument('--max-restarts', type=int, default=50)
    ap.add_argument('--isolate', action='store_true',
                    help='one temu run per test')
    ap.add_argument('--list', action='store_true',
                    help='list the selected tests')
    ap.add_argument('--selftest', action='store_true',
                    help='check the framework and the runner')
    ap.add_argument('--update-golden', action='store_true',
                    help='record the snapshots of this run as golden')
    ap.add_argument('--no-fixtures', action='store_true',
                    help='temu without the test-control device')
    ap.add_argument('--no-build', action='store_true',
                    help='use the images as they are')
    ap.add_argument('--coverage', action='store_true',
                    help='run on a coverage build of temu (build-coverage, '
                    'made with clang if missing) and report')
    ap.add_argument('--coverage-keep', action='store_true',
                    help='add to the profiles earlier runs left')
    ap.add_argument('--update-coverage-baseline', action='store_true',
                    help='record this coverage as the baseline')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 1)
    ap.add_argument('--work', default=os.path.join(HERE, 'work'))
    ap.add_argument('-v', '--verbose', action='store_true')
    opts = ap.parse_args()

    opts.env = None
    if opts.coverage:
        if opts.temu is None:
            opts.temu = covreport.build_temu()
            if opts.temu is None:
                return 1
        opts.env = dict(os.environ, LLVM_PROFILE_FILE=covreport.prepare(
            opts.work, opts.coverage_keep))
    elif opts.temu is None:
        opts.temu = find_temu()
    if not opts.temu or not os.path.exists(opts.temu):
        ap.error('no temu with test fixtures found; pass --temu or set TEMU')
    opts.temu = os.path.abspath(opts.temu)
    opts.temu_args = opts.temu_args.split()
    opts.fixtures = not opts.no_fixtures
    opts.meta = False
    opts.golden = os.path.join(HERE, 'golden')
    targets = opts.targets or list(TARGETS)
    for t in targets:
        if t not in TARGETS:
            ap.error('unknown target %s' % t)

    if opts.selftest:
        opts.meta = True
        opts.tests = ['framework.*']
        opts.skip = []
        opts.timeout = min(opts.timeout, 5)
        opts.machine = ['basic']
        opts.isolate = False
        opts.golden = tempfile.mkdtemp(prefix='devtests-golden-')
        _write_golden(os.path.join(opts.golden, 'framework.a_pass.txt'),
                      [('value', '0x1234')])

    machines_dir = os.path.join(HERE, 'machines')
    opts.machine_files = {
        os.path.splitext(f)[0]: os.path.join(machines_dir, f)
        for f in sorted(os.listdir(machines_dir)) if f.endswith('.cfg')}
    for m in opts.machine:
        if m not in opts.machine_files:
            ap.error('no machines/%s.cfg' % m)
    machines = opts.machine or list(opts.machine_files)

    entries = []
    for t in targets:
        for m in machines:
            if t in machine_targets(opts.machine_files[m]):
                entries.append(Entry(TARGETS[t], m))
    if not entries:
        ap.error('no machine runs those targets')

    arches = {e.target.arch for e in entries}
    if not opts.no_build and not build_images(opts, arches):
        return 1
    opts.images = {a: image_path(a) for a in arches}
    for a, img in opts.images.items():
        if not os.path.exists(img):
            ap.error('no image %s; build it first' % img)

    os.makedirs(opts.work, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max(1, opts.jobs)) as pool:
        entries = list(pool.map(lambda e: run_entry(opts, e), entries))

    if opts.list:
        for e in entries:
            print('%s: %s' % (e.key, e.boot_error or ' '.join(e.listed)))
        return 1 if any(e.boot_error for e in entries) else 0

    known = load_known(os.path.join(HERE, 'known-failures.txt'))
    for e in entries:
        check_golden(opts, e)
        apply_known(known, e)
    write_json(opts, entries)

    if opts.selftest:
        status = selftest(opts, entries)
        # and a snapshot that differs from its golden copy fails the test
        _write_golden(os.path.join(opts.golden, 'framework.a_pass.txt'),
                      [('value', '0x9999')])
        opts.tests = ['framework.a_pass']
        again = [Entry(e.target, e.machine) for e in entries]
        with concurrent.futures.ThreadPoolExecutor(max(1, opts.jobs)) as pool:
            again = list(pool.map(lambda e: run_entry(opts, e), again))
        for e in again:
            check_golden(opts, e)
            r = e.results.get('framework.a_pass')
            if r is None or r.verdict != 'FAIL' or r.golden != 'mismatch':
                print('%-22s selftest FAILED: golden mismatch not caught' %
                      e.key)
                status = 1
        shutil.rmtree(opts.golden, ignore_errors=True)
        return status
    status = summarize(opts, entries)
    if opts.update_golden:
        update_golden(opts, entries)
    if opts.coverage:
        whole = not (opts.targets or opts.machine or opts.tests or opts.skip)
        status |= covreport.report(opts.work, opts.temu,
                                  opts.update_coverage_baseline, whole,
                                  opts.verbose)
    return status


if __name__ == '__main__':
    sys.exit(main())
