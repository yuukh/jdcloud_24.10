from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from analyze_session import dsack_paths, interfaces
from test_session_analysis import row


def feedback(ns=300, **changes):
    value = row('tcp_ack', ns, src='192.0.2.2', dst='192.0.2.1', sport=32000,
                dport=5201, payload=0, ack=1200, sacks=[[1000, 1100]], dsack=1)
    value.update(changes)
    return value


class DsackPathTests(unittest.TestCase):
    def coverage(self, rows, stage='ip_out'):
        result = dsack_paths(rows)
        return result['patterns'][0]['coverage'].get(stage)

    def test_gso_span(self):
        self.assertEqual(self.coverage([row(seq=900, payload=1000), feedback()]),
                         {'minimum_observations': 1, 'maximum_observations': 1})

    def test_adjacent_segments_are_not_duplicates(self):
        self.assertEqual(self.coverage([row(payload=40), row(seq=1040, payload=60), feedback()]),
                         {'minimum_observations': 1, 'maximum_observations': 1})

    def test_partial_capture_has_gap(self):
        self.assertEqual(self.coverage([row(payload=40), row(seq=1050, payload=50), feedback()]),
                         {'minimum_observations': 0, 'maximum_observations': 1})

    def test_overlapping_retransmission(self):
        self.assertEqual(self.coverage([row(), row(ns=200, seq=1050, payload=50), feedback()]),
                         {'minimum_observations': 1, 'maximum_observations': 2})

    def test_full_retransmission(self):
        self.assertEqual(self.coverage([row(), row(ns=200), feedback()]),
                         {'minimum_observations': 2, 'maximum_observations': 2})

    def test_reverse_flow_scope(self):
        for change in ({'src': '192.0.2.3'}, {'dst': '192.0.2.3'}, {'sport': 5202}, {'dport': 32001}):
            with self.subTest(change=change):
                self.assertIsNone(self.coverage([row(**change), feedback()]))

    def test_after_ack_or_old_history_excluded(self):
        for stamp in (301, -500_000_000):
            self.assertIsNone(self.coverage([row(ns=stamp), feedback()]))

    def test_sequence_wrap_and_bucket_boundary(self):
        for start in (0xfffffff0, 65520):
            ack = feedback(sacks=[[start, (start + 100) & 0xffffffff]], ack=(start + 100) & 0xffffffff)
            self.assertEqual(self.coverage([row(seq=start, payload=100), ack]),
                             {'minimum_observations': 1, 'maximum_observations': 1})

    def test_separate_stages_and_interface_names(self):
        data = [row(), row('device_xmit', ifindex=42), feedback()]
        result = dsack_paths(data, names={42: 'rax0'})
        self.assertEqual(set(result['patterns'][0]['coverage']), {'ip_out', 'device_xmit:rax0'})
        self.assertEqual(interfaces('42: rax0@if1: <UP>\n9: br-lan: <UP>\n'), {42: 'rax0', 9: 'br-lan'})

    def test_invalid_and_excessive_ranges_bounded(self):
        result = dsack_paths([row(payload=2**31), feedback(sacks=[[100, 100]]),
                              feedback(sacks=[[200, 100]]), feedback(sacks=[[0, 2**31]])])
        self.assertEqual(result['skipped_excessive_data_spans'], 1)
        self.assertEqual(result['invalid_or_excessive_dsack_spans'], 3)

    def test_overwrite_limit_is_reported(self):
        result = dsack_paths([row(), feedback()], {'cpu_counts': [{'overwritten': 50}]})
        self.assertEqual(result['overwritten_records'], 50)
        self.assertTrue(result['limits'])


if __name__ == '__main__':
    unittest.main()
