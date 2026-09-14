"""Exercise the actual ash controller with fake external devices in /cache.

No real firewall, netdev, kernel debugfs or persistent router path is touched.
The original script is copied, with its fixed paths relocated into a fixture.
The process management, shell control flow and archive generation are real.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).parents[1]
CACHE = Path('/cache/hnat418-debug/controller-tests')

MOCK = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ['H418_TEST_ROOT'])
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with (root/'calls.log').open('a') as out:
    out.write(json.dumps([name, args])+'\n')
if name == 'id':
    # The production controller requires root; this fixture never does.
    print('0')
elif name == 'ip':
    if 'address' in args:
        print('1: br-lan inet 192.168.3.1/24 scope global br-lan')
    elif 'route' in args:
        print('192.168.3.137 dev br-lan src 192.168.3.1')
elif name == 'nft':
    state = root/'firewall-created'
    if args[:2] == ['list','table']:
        sys.exit(0 if state.exists() else 1)
    if args[:2] == ['delete','table']:
        state.unlink(missing_ok=True)
    else:
        state.write_text(sys.stdin.read())
elif name == 'iperf3':
    time.sleep(0.3)
    if os.environ.get('H418_TEST_FAIL'):
        print('test injected server failure', file=sys.stderr)
        sys.exit(7)
    print('{"end":{"sum_sent":{"bits_per_second":1000000000}}}')
elif name == 'df':
    print('Filesystem 1024-blocks Used Available Capacity Mounted on')
    print('fake 2000000 1 '+ ('1' if os.environ.get('H418_TEST_DISK') else '1999999')+' 1% /overlay')
elif name == 'ss' and os.environ.get('H418_TEST_SAMPLER_FAIL'):
    sys.exit(9)
elif name == 'snapshot':
    print('Test snapshot; no real router reads')
'''


class ControllerTests(unittest.TestCase):
    def setUp(self):
        CACHE.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=CACHE)
        self.root = Path(self.temp.name)
        for d in ('overlay', 'debug', 'bin', 'mtkhnat'):
            (self.root / d).mkdir()
        (self.root / 'debug/status').write_text('active=0\n')
        (self.root / 'debug/control').write_text('')
        (self.root / 'debug/records').write_bytes(b'FAKE RECORD FOR SHELL TEST ONLY')
        for name in ('id', 'ip', 'nft', 'iperf3', 'df', 'ss', 'tc', 'ethtool', 'snapshot'):
            path = self.root / 'bin' / name
            path.write_text(MOCK)
            path.chmod(0o755)
        text = (ROOT / 'package/files/hnat418-run').read_text()
        text = text.replace('/overlay/hnat418-debug', str(self.root / 'overlay/hnat418-debug'))
        text = text.replace('/sys/kernel/debug/hnat418', str(self.root / 'debug'))
        text = text.replace('/var/run/hnat418.lock', str(self.root / 'lock'))
        text = text.replace('[ -d /overlay ]', '[ -d ' + str(self.root / 'overlay') + ' ]')
        text = text.replace('/usr/sbin/hnat418-snapshot', str(self.root / 'bin/snapshot'))
        text = text.replace('/usr/libexec/ip-full', str(self.root / 'bin/ip'))
        text = text.replace('/usr/libexec/timeout-coreutils', '/usr/bin/timeout')
        text = text.replace('/sys/module/mtkhnat', str(self.root / 'mtkhnat'))
        # Standalone BusyBox ash deliberately ignores PATH for built-in applets.
        # Shell functions redirect just the fixture commands, not production code.
        wrappers = '\n'.join(name + '() { "' + str(self.root / 'bin' / name) + '" "$@"; }'
                             for name in ('id', 'df', 'nft', 'iperf3', 'ss', 'tc', 'ethtool'))
        text = wrappers + '\n' + text
        self.script = self.root / 'controller'
        self.script.write_text(text)
        self.env = dict(os.environ, H418_TEST_ROOT=str(self.root),
                        SSH_CONNECTION='192.168.3.137 40000 192.168.3.1 22',
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])

    def tearDown(self):
        self.temp.cleanup()

    def run_controller(self, **env):
        return subprocess.run(['busybox', 'ash', str(self.script)],
                              env=dict(self.env, **env), capture_output=True,
                              text=True, timeout=15)

    def test_four_rounds_archive_and_cleanup(self):
        result = self.run_controller()
        self.assertIn('hardware-a port=5201 hardware=1 capture=2', result.stdout)
        self.assertIn('bypass port=5202 hardware=0 capture=1', result.stdout)
        self.assertIn('hardware-quiet port=5203 hardware=1 capture=0', result.stdout)
        self.assertIn('hardware-b port=5204 hardware=1 capture=2', result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count('HNAT418 ROUND_END'), 4)
        self.assertFalse((self.root / 'lock').exists())
        self.assertFalse((self.root / 'firewall-created').exists())
        self.assertEqual((self.root / 'debug/control').read_text(), 'stop\n')
        archive = self.root / 'overlay/hnat418-debug/latest.tar.gz'
        with tarfile.open(archive) as tar:
            names = tar.getnames()
        self.assertEqual(sum(name.endswith('records.bin') for name in names), 3)
        self.assertEqual(sum(name.endswith('iperf-server.json') for name in names), 4)

    def test_server_failure_gets_partial_archive(self):
        result = self.run_controller(H418_TEST_FAIL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('HNAT418 ARCHIVE', result.stdout)
        self.assertFalse((self.root / 'firewall-created').exists())
        self.assertFalse((self.root / 'lock').exists())
        self.assertEqual((self.root / 'debug/control').read_text(), 'stop\n')

    def test_low_disk_refuses_before_network_changes(self):
        result = self.run_controller(H418_TEST_DISK='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('128 MiB', result.stderr)
        self.assertFalse((self.root / 'firewall-created').exists())
        self.assertEqual((self.root / 'debug/control').read_text(), '')

    def test_invalid_peer_refuses(self):
        result = self.run_controller(SSH_CONNECTION='192.168.3.999 40 192.168.3.1 22')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'firewall-created').exists())

    def test_existing_lock_not_removed(self):
        (self.root / 'lock').mkdir()
        result = self.run_controller()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.root / 'lock').exists())
        self.assertFalse((self.root / 'firewall-created').exists())

    def test_active_kernel_session_not_overwritten(self):
        (self.root / 'debug/status').write_text('active=1\n')
        result = self.run_controller()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / 'debug/control').read_text(), '')

    def test_sampler_failure_not_reported_as_pass(self):
        result = self.run_controller(H418_TEST_SAMPLER_FAIL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('sampler failed', result.stderr)
        self.assertFalse((self.root / 'firewall-created').exists())


if __name__ == '__main__':
    unittest.main()
