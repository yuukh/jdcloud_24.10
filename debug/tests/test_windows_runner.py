"""PowerShell orchestration tests with fake ssh/scp/iperf, not Windows hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).parents[1]
PWSH = Path('/cache/hnat418-debug/powershell/pwsh')
MOCK = r'''#!/usr/bin/env python3
import os, pathlib, sys, time
name = pathlib.Path(sys.argv[0]).name
if name == 'iperf3.exe':
    if '--version' in sys.argv:
        print('fixture iperf3')
    else:
        print('{"end":{"sum_received":{"bits_per_second":1000000000}}}')
elif name == 'ssh.exe':
    time.sleep(3)
    if os.environ.get('H418_WINDOWS_FAIL'):
        print('HNAT418 ARCHIVE /overlay/hnat418-debug/latest.tar.gz')
        sys.exit(1)
    print('HNAT418 DONE /overlay/hnat418-debug/latest.tar.gz')
elif name == 'scp.exe':
    pathlib.Path(sys.argv[-1]).write_bytes(b'fixture archive, not router data')
'''


@unittest.skipUnless(PWSH.exists(), 'Optional PowerShell QA tool is not installed')
class WindowsRunnerTests(unittest.TestCase):
    def run_runner(self, fail=False):
        cache = Path('/cache/hnat418-debug/windows-runner-tests')
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as temp:
            root = Path(temp)
            shutil.copyfile(ROOT / 'windows/Run-tests.ps1', root / 'Run-tests.ps1')
            for name in ('iperf3.exe', 'ssh.exe', 'scp.exe'):
                path = root / name
                path.write_text(MOCK)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'])
            if fail:
                env['H418_WINDOWS_FAIL'] = '1'
            result = subprocess.run([str(PWSH), '-NoLogo', '-NoProfile', '-File', str(root / 'Run-tests.ps1'),
                                     '-Router', '192.168.3.1'], env=env,
                                    capture_output=True, text=True, timeout=30)
            files = [p.name for p in root.glob('results-*/*')]
            return result, files

    def test_success_downloads_archive(self):
        result, files = self.run_runner()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('router-evidence.tar.gz', files)
        self.assertEqual(sum(name.startswith('client-52') for name in files), 4)

    def test_failure_downloads_partial_archive(self):
        result, files = self.run_runner(True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('router-partial-evidence.tar.gz', files)


if __name__ == '__main__':
    unittest.main()
