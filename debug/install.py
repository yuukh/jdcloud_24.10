#!/usr/bin/env python3
"""Install the pinned, self-contained debug overlay into an OpenWrt build tree."""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
UPSTREAM_REV = 'ec9ef10efc65da1e6d1de4e2c043c0e13d08eed8'


def command(*args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def configure(path, settings):
    lines = path.read_text().splitlines() if path.exists() else []
    names = set(settings)
    lines = [line for line in lines if not any(
        line.startswith(name + '=') or line == '# ' + name + ' is not set'
        for name in names)]
    lines.extend(name + '=' + value for name, value in settings.items())
    path.write_text('\n'.join(lines) + '\n')


def install(source):
    source = source.resolve()
    if command('git', '-C', source, 'rev-parse', 'HEAD') != UPSTREAM_REV:
        raise RuntimeError('Wrong upstream revision; use build-debug.sh or the dedicated workflow.')
    if not (source / '.config').is_file():
        raise RuntimeError('Copy immortalwrt.config to .config before installing the debug package.')
    if (source / 'package/hnat418-debug').exists():
        raise RuntimeError('Debug overlay already installed; use a fresh build worktree.')
    # No broad bypass/fence/alternate Wi-Fi patch is combined with this candidate.
    nf = source / 'target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat/hnat_nf_hook.c'
    if 'cpu_to_ge_bypass' in nf.read_text():
        raise RuntimeError('Conflicting main bypass patch was already applied.')
    ip = str(ipaddress.IPv4Address(os.environ.get('HNAT418_LAN_IP', '192.168.3.1')))
    generator = source / 'package/base-files/files/bin/config_generate'
    text = generator.read_text()
    default = 'ipad=${ipaddr:-"192.168.6.1"}'
    if text.count(default) != 1:
        raise RuntimeError('Unexpected pinned-source LAN generator; refusing a blind replacement.')
    wifi_patch = ROOT / 'mt_wifi/999-hnat418-local-tuple-provenance.patch'
    subprocess.run(['git', '-C', str(source), 'apply', '--check',
                    '--directory=package/mtk/drivers/mt_wifi/src', str(wifi_patch)], check=True)
    subprocess.run(['git', '-C', str(source), 'apply', '--check', str(ROOT / 'kernel/base.patch')], check=True)
    subprocess.run(['git', '-C', str(source), 'apply', str(ROOT / 'kernel/base.patch')], check=True)
    files = source / 'target/linux/mediatek/files-6.6'
    shutil.copytree(ROOT / 'overlay', files, dirs_exist_ok=True)
    shutil.copyfile(ROOT / 'kernel/hooks.patch', source /
                    'target/linux/mediatek/patches-6.6/9999999-hnat418-bounded-debug.patch')
    shutil.copyfile(wifi_patch, source / 'package/mtk/drivers/mt_wifi/patches' / wifi_patch.name)
    configure(source / 'target/linux/mediatek/filogic/config-6.6', {
        'CONFIG_HNAT418_DEBUG': 'y', 'CONFIG_IKCONFIG': 'y', 'CONFIG_IKCONFIG_PROC': 'y',
    })
    configure(source / '.config', {
        'CONFIG_PACKAGE_hnat418-debug': 'y', 'CONFIG_KERNEL_DEBUG_FS': 'y',
    })
    generator.write_text(text.replace(default, 'ipad=${ipaddr:-"' + ip + '"}'))
    package = source / 'package/hnat418-debug'
    shutil.copytree(ROOT / 'package', package)
    manifest = ['hnat418-debug-format=1', 'upstream=' + UPSTREAM_REV,
                'firmware-repo=' + command('git', '-C', ROOT.parent, 'rev-parse', 'HEAD'),
                'tcp-ack-counters=total_retrans,reord_seen,dsack_dups',
                'capture-modes=0:quiet,1:dsack,2:recovery',
                'source-note=IPv4 scoped diagnostic build, not a final HNAT fix']
    manifest.append('fresh-install-lan=' + ip)
    for path in sorted(ROOT.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            manifest.append(hashlib.sha256(path.read_bytes()).hexdigest() + '  debug/' +
                            str(path.relative_to(ROOT)))
    for path in sorted((source / 'feeds').glob('*')):
        if (path / '.git').exists():
            manifest.append('feed ' + path.name + ' ' + command('git', '-C', path, 'rev-parse', 'HEAD'))
    (package / 'files/build-manifest.txt').write_text('\n'.join(manifest) + '\n')
    print('Installed HNAT 418 bounded diagnostics into', source)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    install(parser.parse_args().source)
