"""Exercise the actual patched C helpers with ASan/UBSan and a small skb fixture.

This does not emulate DMA/PPE/WED. Kernel integration is covered separately by
patch application checks and KUnit; a firmware rebuild is not a hardware test.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).parents[1]


class DatapathTests(unittest.TestCase):
    def test_real_predicates_and_rx_parser(self):
        patch = (ROOT / 'kernel/base.patch').read_text()
        first = patch.split('@@ -0,0 ', 1)[1].split('\n', 1)[1].split('--- /dev/null', 1)[0]
        header = '\n'.join(line[1:] for line in first.splitlines() if line.startswith('+'))
        predicates = header[header.index('static bool hnat_cpu_has_ingress_port'):header.index('/* Called at the bridge')]
        recorder = (ROOT / 'overlay/net/core/hnat418.c').read_text()
        packet = re.search(r'struct h418_packet \{.*?\n\};', recorder, re.S).group()
        parser = recorder[recorder.index('static bool h418_decode_at'):recorder.index('static bool h418_match')]
        # Verify that production eligibility uses port provenance even when a
        # genuine return packet has lost its metadata tag.
        self.assertIn('!hnat_cpu_has_ingress_port(skb)) {', patch)
        self.assertIn('stage == H418_PPE_RX ? h418_decode_rx(skb, &p)', recorder)
        source = '\n'.join(((ROOT / 'tests/datapath_stubs.h').read_text(), packet,
                            predicates, parser, (ROOT / 'tests/datapath_cases.c').read_text()))
        cache = Path(os.environ.get('HNAT418_TEST_TMPDIR', '/cache/hnat418-debug/host-tests'))
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hnat418-c-test-', dir=cache) as name:
            directory = Path(name)
            (directory / 'test.c').write_text(source)
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
            result = subprocess.run([str(directory / 'test')], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stderr, '')
            self.assertIn('HNAT418_DATAPATH_HELPERS_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
