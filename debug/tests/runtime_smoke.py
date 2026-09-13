#!/usr/bin/env python3
"""Boot the diagnostic QEMU kernel with a generated /cache-only initramfs."""
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
CACHE = Path('/cache/hnat418-debug/runtime-smoke')


def main():
    kernel = Path(sys.argv[1]).resolve()
    CACHE.mkdir(parents=True, exist_ok=True)
    root = CACHE / 'root'
    for name in ('bin', 'dev', 'proc', 'sys', 'tmp'):
        (root / name).mkdir(parents=True, exist_ok=True)
    shutil.copyfile('/bin/busybox', root / 'bin/busybox')
    (root / 'bin/busybox').chmod(0o755)
    subprocess.run(['gcc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'runtime_smoke.c'), '-o', str(root / 'smoke')], check=True)
    (root / 'init').write_text('''#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t debugfs debugfs /sys/kernel/debug
/bin/busybox ip link set lo up
/smoke
echo SMOKE_EXIT=$?
/bin/busybox poweroff -f
''')
    (root / 'init').chmod(0o755)
    archive = CACHE / 'initramfs.cpio.gz'
    with archive.open('wb') as output:
        subprocess.run('find . -print0 | cpio --null -o --format=newc | gzip -1',
                       shell=True, check=True, cwd=root, stdout=output)
    result = subprocess.run(['qemu-system-aarch64', '-nodefaults', '-m', '1024',
                             '-machine', 'virt', '-cpu', 'max,pauth-impdef=on',
                             '-kernel', str(kernel), '-initrd', str(archive),
                             '-append', 'console=ttyAMA0 rdinit=/init kunit.enable=0 panic=1',
                             '-no-reboot', '-nographic', '-serial', 'stdio'],
                            capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (CACHE / 'qemu.log').write_text(log)
    if 'HNAT418_RUNTIME_SMOKE_PASS' not in log or 'SMOKE_EXIT=0' not in log:
        raise RuntimeError('Runtime smoke failed: ' + log[-4000:])
    if re.search(r'BUG:|WARNING:|KASAN:|UBSAN:|Kernel panic|runtime error:', log):
        raise RuntimeError('Kernel diagnostic in runtime smoke log')
    print('\n'.join(line for line in log.splitlines() if 'SMOKE_' in line))


if __name__ == '__main__':
    main()
