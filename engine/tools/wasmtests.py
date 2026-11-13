#!/usr/bin/env python3
"""wasmtests.py [group...] [--mode M]... [--match TEXT] [--jobs N] [--timeout S] -- JSC's own WebAssembly tests
(JSTests/wasm of our WebKit revision, fetched into engine/out/jsc-linux/tests) on the Linux ARMv7 jsc under qemu
(engine/tools/jsclinux).

Groups: spec (the spec test suite, through spec-harness.js), function (function-tests, modules), js-api, stress,
references, regress, multi-memory, tail-call, extended-const, gc, gc-spec, function-references,
function-references-spec, threads-spec, wast, fuzz, branch-hints, self-test, v8, v8-regress, ipint, modules, noJIT,
low-executable-memory. Default: spec function js-api. "all" = every group.
Modes (run-jsc-stress-tests' wasm modes that mean something without OMG and SIMD): default, no-consts (BBQ keeps no
constant in its value stack), eager, eager-jettison, no-cjit, collect (collect continuously). Default: default.
The whole of JSTests/wasm comes from the engine's git: git -C engine/webkit archive HEAD JSTests/wasm | tar -x -C
engine/out/jsc-linux/tests.
Prints one line per failure (the first error line of the output) and the totals; the full output of each failure is
kept in engine/out/jsc-linux/tests/results/<group>[.<mode>]/<name>.txt (the directory is emptied first). Failures
listed in engine/tools/wasm32/known.txt (features that are off on 32 bits) are counted apart as "known".
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.environ.get('JSC_LINUX_BASE') or os.path.join(ENGINE, 'out', 'jsc-linux')  # as engine/tools/jsclinux
WASM = os.path.join(BASE, 'tests', 'JSTests', 'wasm')
JSC = os.path.join(ENGINE, 'tools', 'jsclinux')
KNOWN = os.path.join(ENGINE, 'tools', 'wasm32', 'known.txt')

# group -> (directory, harness script or None, arguments before the test, module: 'auto' | 'always')
GROUPS = {
    'spec': ('spec-tests', '../spec-harness.js', [], 'auto'),
    'multi-memory': ('multi-memory-spec-tests', '../multi-memory-spec-harness.js', [], 'auto'),
    'tail-call': ('tail-call-spec-tests', '../tail-call-spec-harness.js', [], 'auto'),
    'extended-const': ('extended-const-spec-tests', '../spec-harness.js', [], 'auto'),
    'function': ('function-tests', None, [], 'auto'),
    'js-api': ('js-api', None, [], 'auto'),
    'stress': ('stress', None, [], 'auto'),
    'references': ('references', None, [], 'auto'),
    'regress': ('regress', None, [], 'auto'),
    'gc': ('gc', None, [], 'auto'),
    'gc-spec': ('gc-spec-tests', '../gc-spec-harness.js', [], 'auto'),
    'function-references': ('function-references', None, [], 'auto'),
    'function-references-spec': ('function-references-spec-tests', '../funcref-spec-harness.js', [], 'auto'),
    'threads-spec': ('threads-spec-tests', '../threads-spec-harness.js', [], 'auto'),
    'wast': ('wast-tests', None, [], 'auto'),
    'fuzz': ('fuzz', None, [], 'auto'),
    'branch-hints': ('branch-hints', None, [], 'auto'),
    'self-test': ('self-test', None, [], 'auto'),
    # runV8WebAssemblySuite(:no_module, "mjsunit.js"): the resources are copied next to the tests.
    'v8': ('v8', None, ['mjsunit.js'], 'never'),
    'v8-regress': ('v8/regress', None, ['mjsunit.js'], 'never'),
    # Not in upstream's wasm.yaml (IPInt is off here too: BBQ runs them); as modules, as runWebAssemblySuite runs a
    # suite (top-level await after load()).
    'ipint': ('ipint-tests', None, [], 'always'),
    'modules': ('modules', None, [], 'always'),
    'noJIT': ('noJIT', None, [], 'auto'),
    'low-executable-memory': ('lowExecutableMemory', None, ['--useConcurrentGC=0', '--useConcurrentJIT=0',
                              '--jitMemoryReservationSize=80000', '--useBaselineJIT=0', '--useDFGJIT=0'], 'always'),
}

EAGER = ['--thresholdForJITAfterWarmUp=10', '--thresholdForJITSoon=10', '--thresholdForOptimizeAfterWarmUp=20',
         '--thresholdForOptimizeAfterLongWarmUp=20', '--thresholdForOptimizeSoon=20',
         '--thresholdForOMGOptimizeAfterWarmUp=20', '--thresholdForOMGOptimizeSoon=20',
         '--maximumEvalCacheableSourceLength=150000', '--useEagerCodeBlockJettisonTiming=true',
         '--repatchBufferingCountdown=0']
MODES = {
    'default': [],
    'no-consts': ['--disableBBQConsts=true'],
    'eager': ['--useRandomizingExecutableIslandAllocation=true'] + EAGER,
    'eager-jettison': ['--forceCodeBlockToJettisonDueToOldAge=true', '--useRandomizingExecutableIslandAllocation=true',
                       '--verifyGC=true'],
    'no-cjit': ['--useConcurrentJIT=false', '--thresholdForJITAfterWarmUp=100', '--scribbleFreeCells=true'],
    'collect': ['--collectContinuously=true', '--verifyGC=true'],
}

BASE_OPTIONS = ['--useDollarVM=1', '--validateOptions=true']
# Options of tiers and features this CPU does not have: the test runs without them (and fails if it needs them:
# those are in known.txt).
DROPPED = re.compile(r'--(useOMGJIT|useWasmLLInt|useWasmIPInt|wasmFunctionIndexRangeToCompile|useWasmSIMD|useFTLJIT|'
                     r'forceAllFunctionsToUseSIMD|useWasmRelaxedSIMD|useWasmMemory64|useJSPI|useWasmTailCalls|'
                     r'useWasmWideArithmetic|useWasmFastMemory|useWebAssemblyThreading)=')


# run-jsc-stress-tests' variables as they are on this CPU (ARMv7 Linux, release, JIT on, no FTL, no SIMD).
PLATFORM = {'architecture': 'arm', 'hostOS': 'linux', 'platform': 'linux', 'buildType': 'release', 'cloop': False,
            'jitTests': True, 'isFTLPlatform': False, 'isOMGPlatform': False, 'isSIMDPlatform': False,
            'isWasmPlatform': True, 'memoryLimited': True, 'addressBits': 32, 'useCompressedHeap': False,
            'model': ''}


def skip_condition(condition):
    """A "skip if" condition (Ruby) on PLATFORM; one that cannot be read does not skip."""
    # ["a", "b"].include?($x) and its forms without parentheses; isFTLEnabled is $jitTests && $isFTLPlatform.
    condition = re.sub(r'(\[[^\]]*\])\.include\?\s*(?:\(\s*(\$\w+)\s*\)|(\$\w+))',
                       lambda m: f'({m.group(2) or m.group(3)} in {m.group(1)})', condition)
    condition = re.sub(r'(?<![$\w])isFTLEnabled\b', repr(PLATFORM['jitTests'] and PLATFORM['isFTLPlatform']), condition)
    expression = re.sub(r'\$([A-Za-z]+)', lambda m: repr(PLATFORM.get(m.group(1))), condition)
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression)
    try:
        return bool(eval(expression, {'__builtins__': {}}, {}))
    except Exception:
        print(f'wasmtests: cannot read "skip if {condition}"', file=sys.stderr)
        return False


def directives(path):
    """The //@ lines of run-jsc-stress-tests: skip, requireOptions, run modes with options."""
    skip = False
    options = []
    try:
        head = open(path, encoding='utf-8', errors='replace').read(4096)
    except OSError:
        return True, []
    for line in head.split('\n'):
        if not line.startswith('//@'):
            continue
        body = line[3:].strip()
        if body.startswith('skip') and 'if' not in body:
            skip = True
        # What upstream skips on this platform (ARMv7, 32-bit addresses, a small machine: the phone has 1 or 2 GB).
        # memoryHog! is a skip when $memoryLimited, which run-jsc-stress-tests sets on ARMv7.
        m = re.match(r'skip if (.*)', body)
        if (m and skip_condition(m.group(1))) or body.startswith('memoryHog!'):
            skip = True
        # The options of a run command or requireOptions, with or without a value (--watchdog-exception-ok).
        for m in re.finditer(r'"(--[A-Za-z][A-Za-z0-9-]*(=[^"]*)?)"', body):
            options.append(m.group(1))
    return skip, options


def prepare(group):
    """v8: upstream copies the harness files next to each test; symlinks here."""
    directory = GROUPS[group][0]
    if not directory.startswith('v8'):
        return
    resources = os.path.join(WASM, 'v8', 'resources')
    target = os.path.join(WASM, directory)
    for name in os.listdir(resources):
        link = os.path.join(target, name)
        if not os.path.lexists(link):
            os.symlink(os.path.join(resources, name), link)


# Tests that pass under qemu only with more time than --timeout (4 times it): 200 instances, each compiled from text
# by wabt in JavaScript (350 s on 2026-11-12, beside a build).
SLOW_HERE = {'stress/grow-funcref-table-with-default.js'}


def run_one(group, mode, name, timeout):
    directory, harness, before, module = GROUPS[group]
    if f'{group}/{name}' in SLOW_HERE:
        timeout *= 4
    cwd = os.path.join(WASM, directory)
    path = os.path.join(cwd, name)
    skip, options = directives(path)
    if skip:
        return name, 'skip', ''
    options = [o for o in options if not DROPPED.match(o)]
    command = [JSC] + BASE_OPTIONS + MODES[mode] + options + before
    if harness:
        command += [harness, name]
    else:
        source = open(path, encoding='utf-8', errors='replace').read()
        is_module = module == 'always' or (module == 'auto' and re.search(r'^\s*(import|export)\s', source, re.M))
        command += (['-m', name] if is_module else [name])
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout, text=True, errors='replace',
                                env=dict(os.environ, TZ='US/Pacific'))  # as run-jsc-stress-tests
        output = result.stdout + result.stderr
        status = 'pass' if result.returncode == 0 else f'fail({result.returncode})'
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) else (error.stdout or '')
        status = 'timeout'
    return name, status, output


def first_error(output):
    for line in output.split('\n'):
        if re.search(r'Error|Exception|ASSERT|SIGSEGV|Segmentation|Illegal|Aborted|crash|failed|qemu:', line):
            return line.strip()[:200]
    lines = [l for l in output.split('\n') if l.strip()]
    return (lines[-1] if lines else '(no output)')[:200]


# Errors that only say a feature is off on 32 bits (Options.cpp, PLAN §76.3): the parser refuses its opcodes or types
# (v128 is the type the "can't get ... type" messages stop at), or the API is not there (JSPI).
KNOWN_ERRORS = [
    (re.compile(r'wasm-simd is not enabled|can\'t get (\d+th argument|\d+th Type\'s return value|Global\'s value|Function local\'s|\d+th field|array\'s element) ?[Tt]ype'), 'SIMD (v128) is off on 32 bits'),
    (re.compile(r'Memory64 is not enabled'), 'memory64 is off on 32 bits'),
    (re.compile(r'wide arithmetic is not enabled'), 'wide arithmetic is off on 32 bits'),
    (re.compile(r'WebAssembly\.Suspending|WebAssembly\.promising'), 'JSPI is off on 32 bits'),
]


def known_error(output):
    for pattern, reason in KNOWN_ERRORS:
        if pattern.search(output):
            return reason
    return None


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
    parser.add_argument('groups', nargs='*', default=['spec', 'function', 'js-api'])
    parser.add_argument('--mode', action='append', choices=sorted(MODES))
    parser.add_argument('--match', default='')
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    groups = list(GROUPS) if args.groups == ['all'] else args.groups
    modes = args.mode or ['default']
    known = known_failures()

    totals = {}
    for mode in modes:
        for group in groups:
            directory = GROUPS[group][0]
            prepare(group)
            names = sorted(n for n in os.listdir(os.path.join(WASM, directory))
                           if n.endswith('.js') and args.match in n and not os.path.islink(os.path.join(WASM, directory, n)))
            label = group if mode == 'default' else f'{group}.{mode}'
            results_dir = os.path.join(BASE, 'tests', 'results', label)
            os.makedirs(results_dir, exist_ok=True)
            for old in os.listdir(results_dir):
                if not args.match or args.match in old:
                    os.remove(os.path.join(results_dir, old))
            counts = {'pass': 0, 'fail': 0, 'skip': 0, 'timeout': 0, 'known': 0}
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                for name, status, output in pool.map(lambda n: run_one(group, mode, n, args.timeout), names):
                    kind = status.split('(')[0]
                    key = f'{group}/{name}'
                    if kind in ('fail', 'timeout') and (key in known or (kind == 'fail' and known_error(output))):
                        kind = 'known'
                    elif kind == 'pass' and key in known:
                        print(f'{label}/{name}: passes, but known.txt says: {known[key]}', flush=True)
                    counts[kind] = counts.get(kind, 0) + 1
                    if kind in ('fail', 'timeout', 'known'):
                        open(os.path.join(results_dir, name + '.txt'), 'w').write(output)
                    if kind in ('fail', 'timeout'):
                        print(f'{label}/{name}: {status}: {first_error(output)}', flush=True)
            totals[label] = counts
            print(f'== {label}: {counts["pass"]} passed, {counts["fail"]} failed, {counts["timeout"]} timed out, '
                  f'{counts["known"]} known (off on 32 bits), {counts["skip"]} skipped', flush=True)
    if any(c['fail'] or c['timeout'] for c in totals.values()):
        sys.exit(1)


if __name__ == '__main__':
    main()
