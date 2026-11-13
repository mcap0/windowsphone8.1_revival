#!/usr/bin/env python3
"""ecbench.py bench [MODE...] [--rounds N] | test [ROUNDS] | wycheproof | regress -- the engine's LibreSSL public key
code off the phone (PLAN.md section 77), as two Linux ARMv7 builds with the phone's flags (Thumb-2, -mcpu=krait, -O2,
no assembly, as deps/build.sh has it for Windows on ARM) in engine/out/libbench/libressl-{released,ours}: the release
tarball as published, and engine/deps/src/libressl with our patches (deps/patches/libressl, applied by deps/fetch.sh).

  bench       engine/port/tests/ecbench.c under engine/tools/armcount, 0 and N rounds (default 10): the instructions one
              operation costs in each build. Modes: p256-verify p256-pubkey p256-keygen p256-ecdh p256-sign
              x25519-keygen x25519-ecdh rsa2048-verify (default all).
  test        engine/port/tests/p256field.c (our P-256 field multiplication against fiat-crypto's, word for word) and
              p256test.c (P-256 through LibreSSL's API, ours against the generic code in one program), under qemu-arm.
  wycheproof  Project Wycheproof's P-256 ECDSA and ECDH vectors (C2SP/wycheproof at a pinned commit, checked by
              SHA-256, fetched into engine/out/libbench/wycheproof) through p256wycheproof.c, in both builds: every
              valid vector must pass and every invalid one fail in each, and the two must decide the "acceptable"
              ones alike.
  regress     LibreSSL's own regress suite (tests/, as ctest runs it) on our build under qemu-arm.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ENGINE, 'deps')
OUT = os.path.join(ENGINE, 'out', 'libbench')
SYSROOT = os.path.join(ENGINE, 'out', 'jsc-linux', 'sysroot')
TESTS = os.path.join(ENGINE, 'port', 'tests')
TOOLCHAIN = os.path.join(ENGINE, 'cmake', 'linux-armv7.cmake')
FLAGS = '--target=armv7a-linux-gnueabihf -mthumb -mcpu=krait -mfpu=neon-vfpv4 -mfloat-abi=hard -O2'
MODES = ['p256-verify', 'p256-pubkey', 'p256-keygen', 'p256-ecdh', 'p256-sign', 'x25519-keygen', 'x25519-ecdh',
         'rsa2048-verify']

WYCHEPROOF_COMMIT = '65ca494727a52944c204441c860b039e6786b99b'
WYCHEPROOF = {
    'ecdsa_secp256r1_sha256_test.json': '182db4f3e230f6f9fa9f800d2a614dede30284b8e8438bbfe1171905402e9332',
    'ecdsa_secp256r1_sha512_test.json': '8e940baad7c6172c58c8f354b10366d307be43a3cb82a5880cb46686c3e8f858',
    'ecdsa_secp256r1_sha3_256_test.json': '8e88fe7c0c33d103392e0cdadb928b9299393e3262d324d185c79a4682dfe289',
    'ecdsa_secp256r1_sha3_512_test.json': '1bf94ad0ada25f64523fa55bfd5bd9a3b1bb9fd4c9154553e6aa8f6f8c735ec6',
    'ecdsa_secp256r1_sha256_p1363_test.json': 'c60de693930e386c3a5472d08081623ef8504decc54b38ac01ec6b2a2575c986',
    'ecdsa_secp256r1_sha512_p1363_test.json': '95c41daca2101540641cc13ccab128d67ad665eaf8349896434e1e6a0c5c68fc',
    'ecdh_secp256r1_test.json': 'cdb8bd5d1206fddb6618c69ffa18f303b4752caba321d348d4aacae3f20cbec4',
    'ecdh_secp256r1_ecpoint_test.json': '648f16d077caf2400d02331ca51f44744c72c799830c8d0595d0b18b6dd9f886',
}


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def libressl_version():
    return open(os.path.join(DEPS, 'src', 'libressl', '.version')).read().strip()


def released_source():
    """The release tarball unpacked as published, without our patches."""
    version = libressl_version()
    dest = os.path.join(OUT, 'src', f'libressl-{version}')
    if not os.path.isdir(dest):
        archive = os.path.join(DEPS, 'src', 'archives', f'libressl-{version}.tar.gz')
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(os.path.dirname(dest), filter='data')
    return dest


def build(variant, tests=False):
    """Static libcrypto for Linux ARMv7 of one variant; returns (source, build directory)."""
    source = released_source() if variant == 'released' else os.path.join(DEPS, 'src', 'libressl')
    if variant == 'ours' and not os.path.exists(os.path.join(source, 'crypto', 'ec', 'ecp_nistp256.c')):
        sys.exit('engine/deps/src/libressl has not got our patches: run engine/deps/fetch.sh')
    directory = os.path.join(OUT, f'libressl-{variant}' + ('-tests' if tests else ''))
    if not os.path.exists(os.path.join(directory, 'build.ninja')):
        run(['cmake', '-G', 'Ninja', '-S', source, '-B', directory, f'-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}',
             '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_C_FLAGS={FLAGS}', '-DCMAKE_C_FLAGS_RELEASE=-DNDEBUG',
             '-DBUILD_SHARED_LIBS=OFF', f'-DLIBRESSL_APPS={"ON" if tests else "OFF"}',
             f'-DLIBRESSL_TESTS={"ON" if tests else "OFF"}', '-DENABLE_ASM=OFF', '-DENABLE_NC=OFF',
             f'-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-arm;-L;{SYSROOT}'], stdout=subprocess.DEVNULL)
    run(['ninja', '-C', directory, '-j', os.environ.get('ECBENCH_JOBS', '3')] + ([] if tests else ['crypto']),
        stdout=subprocess.DEVNULL)
    return source, directory


def program(name, variant, internal=False):
    """engine/port/tests/<name>.c linked with one variant's libcrypto; internal: with LibreSSL's own headers."""
    source, directory = build(variant)
    binary = os.path.join(OUT, f'libressl-{variant}', name)
    command = ['clang'] + FLAGS.split() + [f'--sysroot={SYSROOT}', '-fuse-ld=lld', '-Wall',
                                           '-I', os.path.join(directory, 'include'), '-I', os.path.join(source, 'include')]
    if internal:
        command += ['-DLIBRESSL_INTERNAL', '-DLIBRESSL_CRYPTO_INTERNAL', '-D__BEGIN_HIDDEN_DECLS=',
                    '-D__END_HIDDEN_DECLS=']
        for sub in ('crypto', 'crypto/ec', 'crypto/bn', 'crypto/arch/arm'):
            command += ['-I', os.path.join(source, sub)]
    command += [os.path.join(TESTS, name + '.c'), os.path.join(directory, 'crypto', 'libcrypto.a'), '-lpthread',
                '-o', binary]
    run(command)
    return binary


def instructions(command):
    result = subprocess.run([os.path.join(ENGINE, 'tools', 'armcount')] + command, capture_output=True, text=True)
    found = re.search(r'total insns: (\d+)', result.stdout + result.stderr)
    if result.returncode != 0 or not found:
        sys.exit(f'{" ".join(command)} failed:\n{result.stdout}{result.stderr}')
    return int(found.group(1))


def bench(args):
    rounds = 10
    if '--rounds' in args:
        i = args.index('--rounds')
        rounds = int(args[i + 1])
        args = args[:i] + args[i + 2:]
    modes = args or MODES
    binaries = {variant: program('ecbench', variant) for variant in ('released', 'ours')}
    print(f'{"mode":16} {"released":>12} {"ours":>12}   instructions per operation ({rounds} rounds)')
    for mode in modes:
        cost = {}
        for variant, binary in binaries.items():
            cost[variant] = (instructions([binary, mode, str(rounds)]) - instructions([binary, mode, '0'])) / rounds
        print(f'{mode:16} {cost["released"]:12,.0f} {cost["ours"]:12,.0f}   {cost["released"] / cost["ours"]:.2f}x')


def test(args):
    rounds = args[0] if args else '1000'
    source, _ = build('ours')
    field = os.path.join(OUT, 'libressl-ours', 'p256field')
    run(['clang'] + FLAGS.split() + [f'--sysroot={SYSROOT}', '-fuse-ld=lld', '-Wall', '-I',
                                     os.path.join(source, 'crypto', 'ec'), os.path.join(TESTS, 'p256field.c'), '-o', field])
    failed = subprocess.run(['qemu-arm', '-L', SYSROOT, field]).returncode != 0
    failed |= subprocess.run(['qemu-arm', '-L', SYSROOT, program('p256test', 'ours', internal=True), rounds]).returncode != 0
    sys.exit(1 if failed else 0)


def wycheproof_vectors():
    directory = os.path.join(OUT, 'wycheproof', WYCHEPROOF_COMMIT)
    os.makedirs(directory, exist_ok=True)
    for name, digest in WYCHEPROOF.items():
        path = os.path.join(directory, name)
        if not os.path.exists(path):
            url = f'https://raw.githubusercontent.com/C2SP/wycheproof/{WYCHEPROOF_COMMIT}/testvectors_v1/{name}'
            with urllib.request.urlopen(url) as response:
                data = response.read()
            open(path + '.part', 'wb').write(data)
            os.replace(path + '.part', path)
        if hashlib.sha256(open(path, 'rb').read()).hexdigest() != digest:
            sys.exit(f'{path}: SHA-256 differs from the pinned one')
    lines = []
    for name in WYCHEPROOF:
        data = json.load(open(os.path.join(directory, name)))
        for group in data['testGroups']:
            for t in group['tests']:
                h = lambda v: v if v else '-'
                if name.startswith('ecdsa'):
                    form = 'p1363' if 'p1363' in name else 'der'
                    lines.append(f'ecdsa {group["sha"]} {form} {name}:{t["tcId"]} {t["result"]} '
                                 f'{h(group["publicKeyDer"])} {h(t["msg"])} {h(t["sig"])}')
                else:
                    form = 'ecpoint' if 'ecpoint' in name else 'asn'
                    lines.append(f'ecdh {form} {name}:{t["tcId"]} {t["result"]} {h(t["public"])} {h(t["private"])} '
                                 f'{h(t["shared"])}')
    listing = os.path.join(directory, 'p256.txt')
    open(listing, 'w').write('\n'.join(lines) + '\n')
    return listing, len(lines)


def wycheproof(args):
    listing, count = wycheproof_vectors()
    outcomes = {}
    failed = False
    for variant in ('released', 'ours'):
        result = subprocess.run(['qemu-arm', '-L', SYSROOT, program('p256wycheproof', variant), listing],
                                capture_output=True, text=True)
        out = result.stdout.splitlines()
        for line in out:
            if line.startswith('FAIL') or line.startswith('p256wycheproof'):
                print(f'{variant}: {line}')
        failed |= result.returncode != 0
        outcomes[variant] = {line.split()[2]: line.split()[3] for line in out if line.startswith('acceptable')}
    differ = [k for k in outcomes['released'] if outcomes['released'][k] != outcomes['ours'].get(k)]
    print(f'{count} vectors; acceptable ones decided alike in both builds: {len(outcomes["released"]) - len(differ)} '
          f'of {len(outcomes["released"])}')
    for k in differ:
        print(f'  differs: {k} released {outcomes["released"][k]} ours {outcomes["ours"].get(k)}')
    sys.exit(1 if failed or differ else 0)


def regress(args):
    _, directory = build('ours', tests=True)
    sys.exit(subprocess.run(['ctest', '-j', os.environ.get('ECBENCH_JOBS', '2'), '--timeout', '1800',
                             '--output-on-failure'], cwd=directory).returncode)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('bench', 'test', 'wycheproof', 'regress'):
        sys.exit(__doc__)
    {'bench': bench, 'test': test, 'wycheproof': wycheproof, 'regress': regress}[sys.argv[1]](sys.argv[2:])


if __name__ == '__main__':
    main()
