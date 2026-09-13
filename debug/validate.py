#!/usr/bin/env python3
"""Compile diagnostic kernel objects in /cache; never edit the upstream repo."""
import argparse
import difflib
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
CACHE = Path('/cache/hnat418-debug')
UPSTREAM = Path('/cache/hnat-418/linux-6.6.133-upstream')


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def patch_counts(path):
    lines = path.read_text().splitlines()
    errors = []
    for i, line in enumerate(lines):
        match = re.match(r'@@ -(\d+),(\d+) \+(\d+),(\d+) @@(.*)', line)
        if not match:
            continue
        old = new = 0
        for following in lines[i + 1:]:
            if following.startswith(('@@ ', '--- ')):
                break
            if following.startswith(' '):
                old += 1
                new += 1
            elif following.startswith('-'):
                old += 1
            elif following.startswith('+'):
                new += 1
            else:
                break
        if (old, new) != (int(match[2]), int(match[4])):
            errors.append((line, f'@@ -{match[1]},{old} +{match[3]},{new} @@{match[5]}'))
    return errors


def prepare():
    for p in (ROOT / 'kernel').glob('*.patch'):
        errors = patch_counts(p)
        if errors:
            raise RuntimeError(f'{p.name}: incorrect hunk sizes: {errors}')
    if not (UPSTREAM / 'Makefile').is_file():
        raise RuntimeError('Prepare the pinned OpenWrt kernel first; see debug/README.md')
    CACHE.mkdir(exist_ok=True)
    inputs = sorted((ROOT / 'kernel').glob('*')) + sorted((ROOT / 'overlay').rglob('*'))
    tag = hashlib.sha256(b''.join(str(p.relative_to(ROOT)).encode() + p.read_bytes()
                                 for p in inputs if p.is_file())).hexdigest()[:16]
    target = CACHE / ('kernel-' + tag)
    if (target / '.debug-prepared').exists():
        return target
    if target.exists():
        raise RuntimeError(f'Partial tree {target}; inspect it before retrying')
    # Validate the complete series against only the affected files first.
    with tempfile.TemporaryDirectory(dir=CACHE, prefix='check-') as temp:
        temp = Path(temp)
        with (ROOT / 'kernel/base.patch').open() as f:
            run('patch', '--batch', '--fuzz=0', '-p1', '-d', temp, stdin=f,
                stdout=subprocess.DEVNULL)
        hw = temp / 'target/linux/mediatek'
        base = hw / 'patches-6.6/999999-hnat418-cpu-wifi-ownership-and-metadata.patch'
        shadow = temp / 'shadow'
        shadow.mkdir()
        for patch in (base, ROOT / 'kernel/hooks.patch'):
            for name in re.findall(r'^--- a/(.+)$', patch.read_text(), re.M):
                dest = shadow / name
                if not dest.exists():
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    src = UPSTREAM / name
                    if not src.exists():
                        src = hw / 'files-6.6' / name
                    shutil.copyfile(src, dest)
            with patch.open() as f:
                run('patch', '--batch', '--fuzz=0', '-p1', '-d', shadow, stdin=f)
        run('cp', '--reflink=auto', '-a', UPSTREAM, target)
        for path in shadow.rglob('*'):
            if path.is_file() and not path.name.endswith(('.orig', '.rej')):
                dest = target / path.relative_to(shadow)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, dest)
        shutil.copytree(ROOT / 'overlay', target, dirs_exist_ok=True)
        # Emit a reviewable final delta, separate from production inputs.
        with (CACHE / (target.name + '.diff')).open('w') as output:
            for name in sorted(set(re.findall(r'^--- a/(.+)$',
                    base.read_text() + (ROOT / 'kernel/hooks.patch').read_text(), re.M))):
                before = (UPSTREAM / name).read_text() if (UPSTREAM / name).exists() else ''
                after = (target / name).read_text()
                output.writelines(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                       fromfile='a/' + name, tofile='b/' + name))
    (target / '.debug-prepared').write_text(tag + '\n')
    return target


def objects(tree, jobs):
    build = CACHE / (tree.name + '-objects')
    build.mkdir(exist_ok=True)
    command = ['make', '-C', tree, f'O={build}', 'ARCH=arm64', 'CC=ccache gcc']
    options = ['ARCH_MEDIATEK', 'NET_VENDOR_MEDIATEK', 'IPV6', 'NET_DSA',
               'NET_DSA_MT7530', 'VLAN_8021Q', 'NET_MEDIATEK_SOC', 'MEDIATEK_NETSYS_V2',
               'NF_CONNTRACK', 'NF_CONNTRACK_MARK', 'NF_CONNTRACK_EVENTS',
               'IP_NF_IPTABLES', 'IP_NF_NAT', 'NF_NAT', 'BRIDGE', 'BRIDGE_NETFILTER',
               'NETFILTER_FAMILY_BRIDGE', 'NET_MEDIATEK_HNAT', 'DEBUG_FS', 'HNAT418_DEBUG']
    with (CACHE / (tree.name + '-objects.log')).open('w') as log:
        output = dict(stdout=log, stderr=subprocess.STDOUT)
        run(*command, 'defconfig', **output)
        run(tree / 'scripts/config', '--file', build / '.config',
            *[v for name in options for v in ('-e', name)],
            '-d', 'HSR', '-d', 'MEDIATEK_NETSYS_V3', '-d', 'WERROR', **output)
        run(*command, 'olddefconfig', **output)
        config = (build / '.config').read_text()
        for option in ('HNAT418_DEBUG', 'NET_MEDIATEK_HNAT', 'MEDIATEK_NETSYS_V2'):
            if f'CONFIG_{option}=y\n' not in config:
                raise RuntimeError(f'Required configuration missing: {option}')
        run(*command, f'-j{jobs}', 'net/core/hnat418.o', 'net/core/dev.o',
            'net/core/skbuff.o', 'net/core/gro.o', 'net/bridge/br_input.o',
            'net/ipv4/ip_output.o', 'net/ipv4/tcp_ipv4.o',
            'drivers/net/ethernet/mediatek/mtk_eth_soc.o',
            'drivers/net/ethernet/mediatek/mtk_hnat/', **output)
    print('OBJECTS_OK', build, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'objects', 'kunit'))
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    os.environ['CCACHE_DIR'] = str(CACHE / 'ccache')
    tree = prepare()
    print('KERNEL', tree, flush=True)
    if args.action == 'objects':
        objects(tree, args.jobs)
    if args.action == 'kunit':
        run('python3', tree / 'tools/testing/kunit/kunit.py', 'run', '--arch=arm64',
            f'--jobs={args.jobs}', f'--build_dir={CACHE / (tree.name + "-kunit")}',
            f'--kunitconfig={ROOT / "tests/kunitconfig"}', '--timeout=180',
            '--make_options=CC=ccache gcc', 'hnat418-debug*', cwd=tree)


if __name__ == '__main__':
    main()
