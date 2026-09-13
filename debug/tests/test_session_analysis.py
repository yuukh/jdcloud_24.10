from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from analyze_session import inside, provenance, tcp_counters


def row(stage='ip_out', ns=100, **changes):
    value = dict(stage_name=stage, ns=ns, src='192.0.2.1', dst='192.0.2.2', sport=5201,
                 dport=32000, seq=1000, payload=100, ip_id=7, cookie=99, iif=0,
                 no_fdb=0, ttl=64, a=0, b=0, to_ppe=0, meta0=1, meta1=2, meta2=3)
    value.update(changes)
    return value


class SessionAnalysisTests(unittest.TestCase):
    def test_local_ingress_classification(self):
        result = provenance([row(), row('bridge_decision', 200, b=1)])
        self.assertEqual(result['matched_local_bridge_decisions'], 1)
        self.assertEqual(result['local_packets_classified_as_ingress'], 1)

    def test_return_packet_is_not_local(self):
        result = provenance([row(), row('bridge_decision', 200, b=1, iif=4, no_fdb=1)])
        self.assertEqual(result['local_packets_classified_as_ingress'], 0)

    def test_cookie_alone_does_not_match(self):
        for changes in ({'seq': 999}, {'ip_id': 8}, {'dport': 33000}, {'ns': 900000000}):
            with self.subTest(changes=changes):
                result = provenance([row(), row('bridge_decision', b=1, **changes)])
                self.assertEqual(result['local_packets_classified_as_ingress'], 0)

    def test_sack_hole_correlated_but_dsack_is_not_a_hole(self):
        ack = row('tcp_ack', 300, src='192.0.2.2', dst='192.0.2.1', sport=32000,
                  dport=5201, ack=1000, sacks=[[1100, 1200]], dsack=0)
        inputs = [row(), row('bridge_decision', 200, b=1), ack]
        self.assertEqual(provenance(inputs)['sack_holes_in_first_observed_stale_transmission'], 1)
        ack.update(dsack=1, sacks=[[900, 1000]])
        self.assertEqual(provenance(inputs)['sack_holes_in_first_observed_stale_transmission'], 0)

    def test_sequence_wrap(self):
        self.assertTrue(inside(0xfffffff0, 32, 0))
        self.assertFalse(inside(0xfffffff0, 32, 16))
        self.assertFalse(inside(0, 0, 0))

    def test_counter_pairs(self):
        self.assertEqual(tcp_counters('Tcp: InSegs RetransSegs\nTcp: 20 3\n'),
                         {'InSegs': 20, 'RetransSegs': 3})
        self.assertEqual(tcp_counters('snapshot without counters'), {})


if __name__ == '__main__':
    unittest.main()
