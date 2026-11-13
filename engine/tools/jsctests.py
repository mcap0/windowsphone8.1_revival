#!/usr/bin/env python3
"""jsctests.py [group...] [--mode M]... [--match TEXT] [--jobs N] [--timeout S] -- JSC's own JavaScript tests (JSTests of
our WebKit revision, fetched into engine/out/jsc-linux/tests) on the Linux ARMv7 jsc under qemu (engine/tools/jsclinux):
the phone's 32-bit LLInt, Baseline and DFG off the phone (PLAN §76.2). The WebAssembly tests are wasmtests.py's.

Groups: stress (JSTests/stress), es6 (per es6.yaml), modules, complex (per complex.yaml). Default: stress es6 modules.
Modes, as run-jsc-stress-tests names them (the ones without FTL): default, no-llint, no-cjit-validate-phases, dfg-eager,
dfg-eager-no-cjit-validate, eager-jettison-no-cjit, no-cjit-collect-continuously. Default: default. A test runs in a
mode unless its //@ lines run it only in modes this CPU does not have (FTL), skip it here (32-bit addresses, a small
machine: memoryHog!), or expect a crash.
The suites come from the engine's git: git -C engine/webkit archive HEAD JSTests/stress JSTests/es6 JSTests/modules
JSTests/complex JSTests/es6.yaml JSTests/complex.yaml | tar -x -C engine/out/jsc-linux/tests.
--cache: every test twice on one bytecode cache directory (jsc --diskCachePath), as run-jsc-stress-tests'
runBytecodeCache does on macOS: the first run writes what it compiles, the second reads it back; both must pass, and the
totals say how many second runs decoded a file (the encoder and decoder of the phone's bytecode cache, PLAN.md \u00a777).
Prints one line per failure and the totals; the full output of each failure is kept in
engine/out/jsc-linux/tests/results/js-<group>[.<mode>]/<name>.txt (emptied first). Known failures (with the reason) in
engine/tools/wasm32/known-js.txt, as group/name: reason.
"""
import argparse
import concurrent.futures
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.environ.get('JSC_LINUX_BASE') or os.path.join(ENGINE, 'out', 'jsc-linux')  # as engine/tools/jsclinux
JSTESTS = os.path.join(BASE, 'tests', 'JSTests')
JSC = os.path.join(ENGINE, 'tools', 'jsclinux')
KNOWN = os.path.join(ENGINE, 'tools', 'wasm32', 'known-js.txt')

# The platform variables and the "skip if" reader are wasmtests.py's.
_spec = importlib.util.spec_from_file_location('wasmtests', os.path.join(ENGINE, 'tools', 'wasmtests.py'))
wasmtests = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wasmtests)

BASE_OPTIONS = ['--validateOptions=true', '--useFTLJIT=false', '--useFunctionDotArguments=true',
                '--validateExceptionChecks=true', '--useDollarVM=true', '--maxPerThreadStackUsage=1572864']
EAGER = ['--thresholdForJITAfterWarmUp=10', '--thresholdForJITSoon=10', '--thresholdForOptimizeAfterWarmUp=20',
         '--thresholdForOptimizeAfterLongWarmUp=20', '--thresholdForOptimizeSoon=20',
         '--maximumEvalCacheableSourceLength=150000', '--useEagerCodeBlockJettisonTiming=true',
         '--repatchBufferingCountdown=0']
NO_CJIT = ['--useConcurrentJIT=false', '--thresholdForJITAfterWarmUp=100', '--scribbleFreeCells=true']
COLLECT = ['--collectContinuously=true', '--useGenerationalGC=false', '--verifyGC=true']
MODES = {
    'default': [],
    'no-llint': ['--useLLInt=false'],
    'no-cjit-validate-phases': ['--validateBytecode=true', '--validateGraphAtEachPhase=true',
                                '--useSourceProviderCache=false', '--useRandomizingExecutableIslandAllocation=true',
                                '--useLLIntICs=false'] + NO_CJIT,
    'dfg-eager': EAGER + ['--forceOSRExitToLLInt=true'],
    'dfg-eager-no-cjit-validate': ['--validateGraph=true'] + NO_CJIT + EAGER,
    'eager-jettison-no-cjit': ['--useRandomizingExecutableIslandAllocation=true',
                               '--forceCodeBlockToJettisonDueToOldAge=true', '--verifyGC=true'] + NO_CJIT,
    'no-cjit-collect-continuously': NO_CJIT + COLLECT,
}
# The option lists of run-jsc-stress-tests that //@ lines splat into a run command (FTL: none here).
SPLATS = {'NO_CJIT_OPTIONS': NO_CJIT, 'EAGER_OPTIONS': EAGER, 'FORCE_LLINT_EXIT_OPTIONS': ['--forceOSRExitToLLInt=true'],
          'FTL_OPTIONS': []}
# run-jsc-stress-tests' configurations of the run commands that carry their own options (its table of runs), for a
# test whose only run command is one of them; the modes above stand for the default ones.
RUN_KINDS = {
    'runShadowChicken': ['--useDFGJIT=false', '--alwaysUseShadowChicken=true'],
    'runNoJIT': ['--useJIT=false'],
    'runNoLLInt': ['--useLLInt=false'],
    'runNoCJIT': NO_CJIT,
    'runNoCJITValidate': ['--validateBytecode=true', '--validateGraph=true'] + NO_CJIT,
    'runNoCJITValidatePhases': MODES['no-cjit-validate-phases'],
    'runNoCJITNoAccessInlining': ['--useAccessInlining=false'] + NO_CJIT,
    'runNoCJITNoASO': ['--useArchitectureSpecificOptimizations=false'] + NO_CJIT,
    'runEagerJettisonNoCJIT': MODES['eager-jettison-no-cjit'],
    'runMiniMode': ['--forceMiniVMMode=true'],
    'runNoisyTestNoCJIT': NO_CJIT,
}
# Tests that pass under qemu only with more time than --timeout (4 times it): 20M JIT calls; 2000 new global objects.
# Measured on 2026-11-12 with two builds beside them: 322 s and 502 s.
SLOW_HERE = {'stress/memcpy-typed-loops.js', 'stress/toctou-having-a-bad-time-new-array.js'}
# Run commands that only run on an FTL platform (run-jsc-stress-tests returns at once elsewhere).
FTL_ONLY = re.compile(r'^(runFTL\w*|runNoFTL\w*|runOMG\w*|runFTLNoCJIT\w*)$')
GROUPS = ('stress', 'es6', 'modules', 'complex')


def ruby_if(body):
    """'if COND then A else B end' -> A or B by the platform; anything else as it is."""
    m = re.match(r'if (.*?) then (.*?)(?: else (.*?))? end\s*$', body)
    if not m:
        return body
    return m.group(2) if wasmtests.skip_condition(m.group(1)) else (m.group(3) or 'skip')


def directive(body):
    """A //@ line as it applies on this platform: Ruby's 'if ... then ... end', a comment and a trailing 'X if COND' /
    'X unless COND' resolved (strings masked first, so an 'if' or '#' inside quotes stays). None: not here."""
    body = ruby_if(body)
    masked = re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: '"' + 'x' * (len(m.group(0)) - 2) + '"', body)
    if '#' in masked:
        body, masked = body[:masked.index('#')].rstrip(), masked[:masked.index('#')].rstrip()
    m = re.search(r'\s(if|unless)\s+(.*)$', masked)
    if m:
        condition = wasmtests.skip_condition(body[m.start(2):])
        if condition != (m.group(1) == 'if'):
            return None
        body = body[:m.start()].rstrip()
    return body


def plan(path):
    """(skip reason or None, options, timeout factor, module) from the //@ lines. requireOptions come last, in their
    order, as run-jsc-stress-tests puts them ("-e", "let iterations=40000" included); "-m" makes the file a module."""
    try:
        head = open(path, encoding='utf-8', errors='replace').read(8192)
    except OSError:
        return 'unreadable', [], 1, False
    options, required, runs, slow, module = [], [], [], False, False
    for line in head.split('\n'):
        if not line.startswith('//@'):
            continue
        body = directive(line[3:].strip())
        if body is None:
            continue
        if re.match(r'skip\b', body):
            return 'skip', [], 1, False
        if body.startswith('memoryHog!'):
            return 'memoryHog', [], 1, False
        if body.startswith('mustCrash!') or body.startswith('crashOK!'):
            return 'expects a crash', [], 1, False
        if body.startswith('slow!'):
            slow = True
            continue
        if body.startswith('$') or body.startswith('exclusive!') or body.startswith('noNoLLInt'):
            continue
        name = re.match(r'(\w+!?)', body)
        if not name:
            continue
        name = name.group(1)
        # The quoted arguments and run-jsc-stress-tests' option lists (*NO_CJIT_OPTIONS), in their order.
        strings = []
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|\*([A-Z_]+)', body):
            strings += [m.group(1)] if m.group(2) is None else SPLATS.get(m.group(2), [])
        if '-m' in strings:
            module = True
            strings = [s for s in strings if s != '-m']
        if name == 'requireOptions':
            required += strings
            continue
        options += [s for s in strings if re.match(r'--[A-Za-z][A-Za-z0-9-]*(=.*)?$', s)]
        runs.append(name)
    if runs and all(FTL_ONLY.match(r) for r in runs):
        return 'FTL only', [], 1, False
    if len(set(runs)) == 1 and runs[0] in RUN_KINDS:
        options = RUN_KINDS[runs[0]] + options
    return None, options + required, (4 if slow else 1), module


def es6_modes():
    modes = {}
    path = os.path.join(JSTESTS, 'es6.yaml')
    if os.path.exists(path):
        current = None
        for line in open(path):
            m = re.match(r'- path: es6/(.*)', line.strip())
            if m:
                current = m.group(1)
            m = re.match(r'cmd: runES6 :(\w+)', line.strip())
            if m and current:
                modes[current] = m.group(1)
    return modes


def complex_tests():
    tests = {}
    path = os.path.join(JSTESTS, 'complex.yaml')
    if os.path.exists(path):
        current = None
        for line in open(path):
            m = re.match(r'- path: complex/(.*)', line.strip())
            if m:
                current = m.group(1)
            m = re.match(r'cmd: runComplexTest \[(.*?)\], \[(.*?)\], \w+, (.*)', line.strip())
            if m and current:
                before = re.findall(r'"([^"]+)"', m.group(1))
                after = re.findall(r'"([^"]+)"', m.group(2))
                options = re.findall(r'"(--[^"]+)"', m.group(3))
                tests[current] = (before, after, options)
    return tests


def run_one(group, mode, name, timeout, es6, complexes, cache=False):
    directory = os.path.join(JSTESTS, group)
    path = os.path.join(directory, name)
    reason, options, factor, module = plan(path)
    before, after = [], []
    if group == 'es6':
        es6_mode = es6.get(name, 'normal')
        if es6_mode != 'normal':
            return name, 'skip', f'es6.yaml: {es6_mode}'
    if group == 'complex':
        if name not in complexes:
            return name, 'skip', 'not a test'
        before, after, extra = complexes[name]
        options += extra
    if reason:
        return name, 'skip', reason
    if f'{group}/{name}' in SLOW_HERE:
        factor = max(factor, 4)
    command = [JSC] + BASE_OPTIONS + MODES[mode] + options + before
    # A module as run-jsc-stress-tests runs one: "-m" in its //@ lines, the modules group, .mjs (jsc's own rule). Not by
    # its text: tests of module syntax hold "import" and "export" in strings and run as scripts.
    is_module = module or group == 'modules' or name.endswith('.mjs')
    command += (['-m', name] if is_module else [name]) + after
    if cache:
        return run_cached(name, command, directory, timeout * factor)
    return (name,) + run_command(command, directory, timeout * factor)


def run_command(command, directory, timeout):
    try:
        # run-jsc-stress-tests runs every test in US/Pacific (the Date tests expect its offsets).
        result = subprocess.run(command, cwd=directory, capture_output=True, timeout=timeout, text=True,
                                errors='replace', env=dict(os.environ, TZ='US/Pacific'))
        output = result.stdout + result.stderr
        status = 'pass' if result.returncode == 0 else f'fail({result.returncode})'
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) else (error.stdout or '')
        status = 'timeout'
    return status, output


def run_cached(name, command, directory, timeout):
    """The test twice on a fresh cache directory: written by the first run, read by the second."""
    root = os.path.join(BASE, 'tests', 'cache-tmp')
    os.makedirs(root, exist_ok=True)
    cache = tempfile.mkdtemp(dir=root)
    try:
        options = [f'--diskCachePath={cache}']
        status, output = run_command(command[:1] + options + command[1:], directory, timeout)
        if status != 'pass':
            return name, status, '[first run, writing the cache]\n' + output
        status, output = run_command(command[:1] + options + ['--reportBytecodeCacheDecodeTimes=true'] + command[1:],
                                     directory, timeout)
        if status != 'pass':
            return name, status, '[second run, reading the cache]\n' + output
        return name, 'pass-decoded' if 'BytecodeCache: decoded' in output else 'pass', output
    finally:
        shutil.rmtree(cache, ignore_errors=True)


def known_failures():
    known = {}
    if os.path.exists(KNOWN):
        for line in open(KNOWN):
            line = line.split('#', 1)[0].strip()
            if ':' in line:
                key, reason = line.split(':', 1)
                known[key.strip()] = reason.strip()
    return known


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('groups', nargs='*', default=['stress', 'es6', 'modules'])
    parser.add_argument('--mode', action='append', choices=sorted(MODES))
    parser.add_argument('--match', default='')
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--cache', action='store_true')
    args = parser.parse_args()
    groups = list(GROUPS) if args.groups == ['all'] else args.groups
    modes = args.mode or ['default']
    known = known_failures()
    es6, complexes = es6_modes(), complex_tests()

    totals = {}
    for mode in modes:
        for group in groups:
            directory = os.path.join(JSTESTS, group)
            names = sorted(n for n in os.listdir(directory)
                           if (n.endswith('.js') or n.endswith('.mjs')) and args.match in n
                           and os.path.isfile(os.path.join(directory, n)))
            label = f'js-{group}' if mode == 'default' else f'js-{group}.{mode}'
            if args.cache:
                label += '.cache'
            results_dir = os.path.join(BASE, 'tests', 'results', label)
            os.makedirs(results_dir, exist_ok=True)
            for old in os.listdir(results_dir):
                if not args.match or args.match in old:
                    os.remove(os.path.join(results_dir, old))
            counts = {'pass': 0, 'fail': 0, 'skip': 0, 'timeout': 0, 'known': 0, 'decoded': 0}
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                for name, status, output in pool.map(lambda n: run_one(group, mode, n, args.timeout, es6, complexes,
                                                                       args.cache), names):
                    if status == 'pass-decoded':
                        counts['decoded'] += 1
                        status = 'pass'
                    kind = status.split('(')[0]
                    key = f'{group}/{name}'
                    if kind in ('fail', 'timeout') and key in known:
                        kind = 'known'
                    counts[kind] = counts.get(kind, 0) + 1
                    if kind in ('fail', 'timeout', 'known'):
                        open(os.path.join(results_dir, name + '.txt'), 'w').write(output)
                    if kind in ('fail', 'timeout'):
                        print(f'{label}/{name}: {status}: {wasmtests.first_error(output)}', flush=True)
            totals[label] = counts
            print(f'== {label}: {counts["pass"]} passed, {counts["fail"]} failed, {counts["timeout"]} timed out, '
                  f'{counts["known"]} known, {counts["skip"]} skipped'
                  + (f', {counts["decoded"]} decoded from the cache' if args.cache else ''), flush=True)
    if any(c['fail'] or c['timeout'] for c in totals.values()):
        sys.exit(1)


if __name__ == '__main__':
    main()
