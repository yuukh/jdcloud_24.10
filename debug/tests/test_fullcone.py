"""Run the complete pinned masquerade function before/after our patch.

Dependencies are small fixtures; this is not a live conntrack concurrency or
hardware test.  The old port-wrap case is stopped by a bounded lookup counter.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from validate import ROOT, UPSTREAM, patch_counts


class FullconeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cache = Path('/cache/hnat418-debug/host-tests')
        cache.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='fullcone-', dir=cache)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        relative = Path('net/netfilter/nf_nat_masquerade.c')
        original = (UPSTREAM / relative).read_text()
        target = cls.directory / relative
        target.parent.mkdir(parents=True)
        target.write_text(original)
        patch = ROOT / 'kernel/fullcone.patch'
        if patch_counts(patch):
            raise AssertionError(patch_counts(patch))
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-d', cls.directory,
                        '-i', patch], check=True, capture_output=True)
        for label, text in [('before', original), ('after', target.read_text())]:
            start = text.index('unsigned int\nnf_nat_masquerade_ipv4(')
            function = text[start:text.index('\nEXPORT_SYMBOL_GPL(nf_nat_masquerade_ipv4)', start)]
            source = cls.directory / (label + '.c')
            source.write_text((ROOT / 'tests/fullcone_stubs.h').read_text() + '\n' + function +
                              '\n' + (ROOT / 'tests/fullcone_cases.c').read_text())
            command = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if label == 'before':
                command.append('-DBEFORE_FIX')
            subprocess.run(command + [str(source), '-o', str(cls.directory / label)], check=True)

    def run_case(self, case, before=False, exit_code=0):
        result = subprocess.run([str(self.directory / ('before' if before else 'after')), case],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, exit_code, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '')
        return result.stdout

    def test_confirmed_connections_are_not_modified(self):
        for case in ('confirmed-no-extension', 'confirmed-empty-helper', 'confirmed-other-helper'):
            with self.subTest(case=case):
                self.assertIn('OLD_CONFIRMED_CONNTRACK_MUTATED', self.run_case(case, before=True))
                self.assertIn('CONFIRMED_CONNTRACK_UNCHANGED', self.run_case(case))

    def test_saturated_last_port_does_not_wrap_under_lock(self):
        self.assertIn('OLD_PORT_SEARCH_WRAPPED_WITH_LOCK_HELD',
                      self.run_case('last-port-busy', before=True, exit_code=77))
        self.run_case('last-port-busy')

    def test_exhausted_and_invalid_ranges_fail_bounded(self):
        for case in ('all-odd-ports-busy', 'finite-range-busy', 'invalid-range'):
            with self.subTest(case=case):
                self.run_case(case)

    def test_new_flows_reuse_expectations_and_preserve_other_helpers(self):
        for case in ('new-udp', 'existing-helper-space', 'other-helper', 'existing-expectation',
                     'last-port-free', 'mode-disabled', 'tcp'):
            with self.subTest(case=case):
                self.run_case(case)

    def test_setup_and_allocation_failures(self):
        for case in ('zero-source', 'no-address', 'setup-failure', 'helper-allocation-failure'):
            with self.subTest(case=case):
                self.run_case(case)


if __name__ == '__main__':
    unittest.main()
