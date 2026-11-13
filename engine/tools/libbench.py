#!/usr/bin/env python3
"""libbench.py [inflate|png] [FILE...] -- the engine's zlib and libpng off the phone (PLAN.md section 77): zlib 1.3.2 or
zlib-ng (compat), libpng with NEON off or on, built for Linux ARMv7 with the phone's flags (Thumb-2, -mcpu=krait, -O2)
into engine/out/libbench/<variant>, then engine/port/tests/libbench.c run under engine/tools/armcount. Prints the guest
instructions of each variant and checks that all of them produce the same bytes.
Default files: the HTML, scripts and style sheets (inflate) or the PNGs (png) of share/banco.
"""
import glob
import os
import re
import subprocess
import sys

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(ENGINE)
SRC = os.path.join(ENGINE, 'deps', 'src')
OUT = os.path.join(ENGINE, 'out', 'libbench')
TOOLCHAIN = os.path.join(ENGINE, 'cmake', 'linux-armv7.cmake')
FLAGS = '--target=armv7a-linux-gnueabihf -mthumb -mcpu=krait -mfpu=neon-vfpv4 -mfloat-abi=hard -O2'
VARIANTS = {
    'zlib+png': ('zlib', 'off'),
    'zlib-ng+png': ('zlib-ng', 'off'),
    'zlib-ng+png-neon': ('zlib-ng', 'on'),
}


def cmake(name, source, build, prefix, *args):
    if os.path.exists(os.path.join(prefix, '.done-' + name)):
        return
    subprocess.run(['cmake', '-G', 'Ninja', '-S', source, '-B', build, f'-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}',
                    '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_C_FLAGS={FLAGS}', f'-DCMAKE_C_FLAGS_RELEASE=-DNDEBUG',
                    f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DCMAKE_PREFIX_PATH={prefix}', '-DBUILD_SHARED_LIBS=OFF',
                    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', *args], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', build], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--install', build], check=True, stdout=subprocess.DEVNULL)
    open(os.path.join(prefix, '.done-' + name), 'w').close()


def build(variant):
    zlib, neon = VARIANTS[variant]
    prefix = os.path.join(OUT, variant)
    os.makedirs(prefix, exist_ok=True)
    if zlib == 'zlib':
        cmake('zlib', os.path.join(SRC, 'zlib'), os.path.join(prefix, 'b-zlib'), prefix, '-DZLIB_BUILD_SHARED=OFF',
              '-DZLIB_BUILD_TESTING=OFF')
        library = 'libz.a' if os.path.exists(os.path.join(prefix, 'lib', 'libz.a')) else 'libzs.a'
    else:
        # As deps/build.sh builds it for the phone: zlib's API and names, NEON chosen at compile time.
        cmake('zlib-ng', os.path.join(SRC, 'zlib-ng'), os.path.join(prefix, 'b-zlib-ng'), prefix, '-DZLIB_COMPAT=ON',
              '-DWITH_RUNTIME_CPU_DETECTION=OFF', '-DWITH_ARMV8=OFF', '-DWITH_GTEST=OFF', '-DBUILD_TESTING=OFF',
              '-DZLIB_ENABLE_TESTS=OFF', '-DWITH_GZFILEOP=OFF')
        library = 'libz.a'
    zlibLibrary = os.path.join(prefix, 'lib', library)
    cmake('libpng', os.path.join(SRC, 'libpng'), os.path.join(prefix, 'b-libpng'), prefix, '-DPNG_SHARED=OFF',
          '-DPNG_STATIC=ON', '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF', f'-DPNG_ARM_NEON={neon}',
          f'-DZLIB_LIBRARY={zlibLibrary}', f'-DZLIB_INCLUDE_DIR={os.path.join(prefix, "include")}',
          '-DSKIP_INSTALL_FILES=ON')
    binary = os.path.join(prefix, 'libbench')
    subprocess.run(['clang', '--target=armv7a-linux-gnueabihf', f'--sysroot={os.path.join(ENGINE, "out", "jsc-linux", "sysroot")}',
                    '-fuse-ld=lld'] + FLAGS.split() + ['-I', os.path.join(prefix, 'include'),
                    os.path.join(ENGINE, 'port', 'tests', 'libbench.c'), os.path.join(prefix, 'lib', 'libpng16.a'),
                    zlibLibrary, '-lm', '-o', binary], check=True)
    return binary


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'png'
    files = sys.argv[2:]
    if not files:
        pattern = '*.png' if mode == 'png' else '*'
        files = sorted(f for f in glob.glob(os.path.join(ROOT, 'share', 'banco', '*', pattern))
                       if mode == 'png' or re.search(r'\.(js|css|html)$', f))
    results = {}
    for variant in VARIANTS:
        binary = build(variant)
        run = subprocess.run([os.path.join(ENGINE, 'tools', 'armcount'), binary, mode] + files, capture_output=True,
                             text=True)
        if run.returncode:
            sys.exit(f'{variant}: {run.stdout[-400:]}{run.stderr[-400:]}')
        count = int(re.search(r'total insns: (\d+)', run.stderr + run.stdout).group(1))
        summed = re.search(r'checksum (\w+)', run.stdout).group(1)
        results[variant] = (count, summed)
    base = next(iter(results.values()))[0]
    for variant, (count, summed) in results.items():
        print(f'{variant:18} {count / 1e6:10.1f} M instructions  {count / base * 100:5.1f}%  checksum {summed}')
    if len({s for _, s in results.values()}) != 1:
        sys.exit('the variants do not agree')


if __name__ == '__main__':
    main()
