#
# Coverage of temu's device code by the device test suite: a temu built with
# clang's source based coverage writes a profile per process, which is
# merged and reported for the files coverage-scope.txt names.
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

import fnmatch
import glob
import json
import os
import platform
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# A file whose line coverage falls by more than this many points against
# the baseline is a regression. Paths that depend on host timing make the
# same run vary by about a point.
TOLERANCE = 2.0


def host():
    """Baselines are per host: the compilers count regions differently."""
    return platform.system().lower()


def build_dir():
    return os.path.join(ROOT, 'build-coverage')


def build_temu():
    """Configure on first use, then bring up to date. Returns the binary,
    or None after showing why the build failed."""
    build = build_dir()
    # Continuous mode (%c), which keeps the counters of a process that is
    # killed, works only with the counters relocated at run time.
    f = ' '.join(['-fprofile-instr-generate', '-fcoverage-mapping',
                  '-mllvm', '-runtime-counter-relocation'])
    options = ['-Dtest_fixtures=true', '-Dc_args=' + f, '-Dcpp_args=' + f,
               '-Dc_link_args=-fprofile-instr-generate',
               '-Dcpp_link_args=-fprofile-instr-generate']
    if os.path.isdir(build):
        steps = [['meson', 'configure', build] + options]
    else:
        steps = [['meson', 'setup', build] + options]
    steps.append(['ninja', '-C', build])
    # meson's compiler checks run programs built with these flags, whose
    # profiles would otherwise land in the source tree
    env = dict(os.environ, CC='clang', CXX='clang++',
               LLVM_PROFILE_FILE=os.path.join(build, 'meson-%p.profraw'))
    for cmd in steps:
        p = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if p.returncode != 0:
            sys.stdout.write(p.stdout)
            print('coverage build failed: %s' % ' '.join(cmd))
            return None
    return os.path.join(build, 'temu')


def profile_dir(work):
    return os.path.join(work, 'coverage', 'profiles')


def prepare(work, keep):
    """The LLVM_PROFILE_FILE for temu: one file per process, written as it
    runs (%c), so a run killed after a timeout counts too."""
    d = profile_dir(work)
    if not keep:
        shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, 'temu-%p-%c.profraw')


def load_scope():
    rules = []
    with open(os.path.join(HERE, 'coverage-scope.txt')) as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                rules.append((not line.startswith('!'), line.lstrip('!')))
    return rules


def in_scope(rules, rel):
    inside = False
    for include, pattern in rules:
        if fnmatch.fnmatchcase(rel, pattern):
            inside = include
    return inside


def merge(work):
    raws = glob.glob(os.path.join(profile_dir(work), '*.profraw'))
    # a process killed before it wrote anything leaves an empty file
    raws = [r for r in raws if os.path.getsize(r) > 0]
    if not raws:
        print('coverage: no profiles under %s' % profile_dir(work))
        return None
    out = os.path.join(work, 'coverage', 'temu.profdata')
    lst = os.path.join(work, 'coverage', 'profiles.txt')
    with open(lst, 'w') as f:
        f.write('\n'.join(raws) + '\n')
    subprocess.run(['llvm-profdata', 'merge', '-sparse',
                    '-f', lst, '-o', out], check=True)
    return out


def summarize_files(temu, profdata):
    """{relative path: {'lines': (covered, count), 'regions': ...,
    'branches': ..., 'functions': ...}} for every file in scope."""
    p = subprocess.run(['llvm-cov', 'export', '-summary-only', temu,
                        '-instr-profile=' + profdata],
                       check=True, stdout=subprocess.PIPE, text=True)
    data = json.loads(p.stdout)
    rules = load_scope()
    files = {}
    for f in data['data'][0]['files']:
        rel = os.path.relpath(os.path.normpath(f['filename']), ROOT)
        rel = rel.replace(os.sep, '/')
        if not in_scope(rules, rel):
            continue
        s = f['summary']
        files[rel] = {k: (s[k]['covered'], s[k]['count'])
                      for k in ('lines', 'regions', 'branches', 'functions')}
    return files


def pct(pair):
    covered, count = pair
    return 100.0 * covered / count if count else 100.0


def baseline_path():
    return os.path.join(HERE, 'coverage', 'baseline-%s.txt' % host())


def read_baseline(path):
    base = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                if line.startswith('#') or not line.strip():
                    continue
                rel, value = line.split()
                base[rel] = float(value)
    return base


def write_baseline(path, files):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write('# line coverage per file, percent; run.py --coverage '
                '--update-coverage-baseline\n')
        for rel in sorted(files):
            f.write('%s %.1f\n' % (rel, pct(files[rel]['lines'])))


def report(work, temu, update_baseline, compare, verbose):
    """Merge, write the summary and the HTML, and compare with the baseline
    when 'compare' says the run was a whole one. Returns 0, or 1 on a
    regression."""
    profdata = merge(work)
    if profdata is None:
        return 1
    files = summarize_files(temu, profdata)

    total = {k: [0, 0] for k in ('lines', 'regions', 'branches', 'functions')}
    rows = []
    for rel in sorted(files):
        s = files[rel]
        for k in total:
            total[k][0] += s[k][0]
            total[k][1] += s[k][1]
        rows.append('%-48s %6.1f %6.1f %6.1f %6d' % (
            rel, pct(s['lines']), pct(s['regions']), pct(s['branches']),
            s['lines'][1] - s['lines'][0]))
    header = '%-48s %6s %6s %6s %6s' % ('file', 'lines', 'region', 'branch',
                                       'missed')
    footer = '%-48s %6.1f %6.1f %6.1f %6d' % (
        'TOTAL', pct(total['lines']), pct(total['regions']),
        pct(total['branches']), total['lines'][1] - total['lines'][0])
    summary = os.path.join(work, 'coverage', 'summary.txt')
    with open(summary, 'w') as f:
        f.write('\n'.join([header] + rows + [footer]) + '\n')

    html = os.path.join(work, 'coverage', 'html')
    shutil.rmtree(html, ignore_errors=True)
    subprocess.run(['llvm-cov', 'show', temu, '-instr-profile=' + profdata,
                    '-format=html', '-output-dir=' + html,
                    '-show-line-counts-or-regions', '-show-branches=count']
                   + [os.path.join(ROOT, rel) for rel in sorted(files)],
                   check=True, stdout=subprocess.DEVNULL)

    if verbose:
        print(header)
        print('\n'.join(rows))
    print(footer)
    print('coverage: %s, %s' % (os.path.relpath(summary),
                                os.path.relpath(os.path.join(html,
                                                             'index.html'))))

    path = baseline_path()
    status = 0
    if update_baseline:
        write_baseline(path, files)
        print('coverage: baseline written to %s' % os.path.relpath(path))
    elif not compare:
        print('coverage: not compared with the baseline, which is for '
              'whole runs')
    else:
        base = read_baseline(path)
        for rel in sorted(files):
            now = pct(files[rel]['lines'])
            if rel in base and now < base[rel] - TOLERANCE:
                print('coverage regression: %s %.1f%% (baseline %.1f%%)' % (
                    rel, now, base[rel]))
                status = 1
        if not base:
            print('coverage: no baseline for %s yet' % host())
    return status
