#!/usr/bin/env python3
"""Collect flash images separately from debug data; retain matching ABI inputs."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent


def export(source, output):
    source, output = source.resolve(), output.resolve()
    target = source / 'bin/targets/mediatek/filogic'
    if not target.is_dir():
        raise RuntimeError('No Filogic build output; refusing an empty delivery.')
    output.mkdir(parents=True, exist_ok=True)
    images = output / 'firmware'
    shutil.copytree(target, images, dirs_exist_ok=True)
    shutil.copytree(ROOT / 'windows', output / 'windows', dirs_exist_ok=True)
    shutil.copyfile(ROOT / 'README.md', output / 'README.md')
    shutil.copyfile(ROOT / 'decode.py', output / 'decode.py')
    metadata = output / 'build-metadata'
    metadata.mkdir(exist_ok=True)
    shutil.copyfile(source / '.config', metadata / 'openwrt.config')
    shutil.copyfile(source / 'package/hnat418-debug/files/build-manifest.txt', metadata / 'build-manifest.txt')
    kernels = list((source / 'build_dir').glob('target-*/linux-mediatek_filogic/linux-6.6.*'))
    if len(kernels) != 1:
        raise RuntimeError('Expected one matching expanded kernel build tree.')
    kernel = kernels[0]
    config = (kernel / '.config').read_text()
    for symbol in ('CONFIG_HNAT418_DEBUG=y', 'CONFIG_NET_MEDIATEK_SOC=y', 'CONFIG_DEBUG_FS=y'):
        if symbol not in config:
            raise RuntimeError('Required final kernel option missing: ' + symbol)
    for name in ('.config', 'Module.symvers', 'System.map', 'vmlinux'):
        if not (kernel / name).is_file():
            raise RuntimeError('Missing matching kernel build artifact: ' + name)
        shutil.copyfile(kernel / name, metadata / ('kernel.config' if name == '.config' else name))
    # Headers alone are insufficient to build arbitrary replacement modules.
    # Preserve the complete configured kernel and target toolchain as an
    # optional developer archive, NOT as an installable/flashable image.
    sdk_parts = [str(kernel.relative_to(source))]
    sdk_parts += [str(p.relative_to(source)) for p in (source / 'staging_dir').glob('toolchain-*')]
    subprocess.run(['tar', '-C', str(source), '-czf', str(output / 'matching-kernel-toolchain.tar.gz'),
                    *sdk_parts], check=True)
    checksums = []
    for path in sorted(output.rglob('*')):
        if path.is_file() and path.name != 'SHA256SUMS':
            h = hashlib.sha256()
            with path.open('rb') as f:
                for block in iter(lambda: f.read(1024 * 1024), b''):
                    h.update(block)
            checksums.append(h.hexdigest() + '  ' + str(path.relative_to(output)))
    (output / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    export(args.source, args.output)
