"""PowerShell orchestration tests with fake ssh/scp/iperf, not Windows hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).parents[1]
PWSH = Path(shutil.which('pwsh') or '/cache/hnat418-debug/powershell/pwsh')
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
    if os.environ.get('H418_SCP_FAIL'):
        pathlib.Path(sys.argv[-1]).write_bytes(b'partial transfer')
        sys.exit(1)
    pathlib.Path(sys.argv[-1]).write_bytes(b'fixture archive, not router data')
'''


@unittest.skipUnless(PWSH.exists(), 'Optional PowerShell QA tool is not installed')
class WindowsRunnerTests(unittest.TestCase):
    def run_runner(self, fail=False, mode=''):
        cache = Path('/cache/hnat418-debug/windows-runner-tests')
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as temp:
            root = Path(temp)
            source = (ROOT / 'windows/Run-tests.ps1').read_text()
            if mode in ('job_error', 'job_failure'):
                statement = "Write-Error 'simulated retry diagnostic'" if mode == 'job_error' else "throw 'simulated client failure'"
                source = source.replace("'All four client tests completed.'", statement)
            (root / 'Run-tests.ps1').write_text(source)
            for name in ('iperf3.exe', 'ssh.exe', 'scp.exe'):
                path = root / name
                path.write_text(MOCK)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'])
            if fail:
                env['H418_WINDOWS_FAIL'] = '1'
            if mode == 'download_failure':
                env['H418_SCP_FAIL'] = '1'
            extra = ['-DownloadOnly'] if mode == 'download_only' else []
            if mode == 'download_only':
                (root / 'iperf3.exe').unlink()
                (root / 'ssh.exe').unlink()
            result = subprocess.run([str(PWSH), '-NoLogo', '-NoProfile', '-File', str(root / 'Run-tests.ps1'),
                                     '-Router', '192.168.3.1', *extra], env=env,
                                    capture_output=True, text=True, timeout=30)
            files = [p.name for p in root.glob('results-*/*')]
            return result, files

    def test_success_downloads_archive(self):
        result, files = self.run_runner()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('router-evidence.tar.gz', files)
        self.assertEqual(sum(name.startswith('client-52') for name in files), 4)
        self.assertIn('client-network-before.txt', files)
        self.assertIn('client-network-after.txt', files)

    def test_failure_downloads_partial_archive(self):
        result, files = self.run_runner(True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('router-partial-evidence.tar.gz', files)

    def test_job_error_does_not_prevent_download(self):
        result, files = self.run_runner(mode='job_error')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('router-evidence.tar.gz', files)
        self.assertIn('client-job-errors.txt', files)

    def test_job_failure_keeps_evidence_but_reports_failure(self):
        result, files = self.run_runner(mode='job_failure')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('router-evidence.tar.gz', files)
        self.assertIn('runner-error.txt', files)

    def test_download_only_requires_no_iperf_or_remote_test(self):
        result, files = self.run_runner(mode='download_only')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('router-evidence.tar.gz', files)
        self.assertNotIn('router-control.txt', files)
        self.assertNotIn('client-network-before.txt', files)
        self.assertNotIn('client-network-after.txt', files)
        self.assertFalse(any(name.startswith('client-52') for name in files))

    def test_failed_copy_is_not_a_completed_archive(self):
        result, files = self.run_runner(mode='download_failure')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('router-evidence.tar.gz', files)
        self.assertIn('runner-error.txt', files)


if __name__ == '__main__':
    unittest.main()
