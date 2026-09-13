from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from analyze_session import metadata_trust
from test_session_analysis import row


class MetadataTrustTests(unittest.TestCase):
    def packets(self, layout):
        meta = dict(meta0=0, meta1=0x67890 if layout == 'legacy' else 0,
                    meta2=0x67890000 if layout == 'rx-v2' else 0)
        original = row(**meta)
        decision = row('bridge_decision', ns=200, **meta)
        decision['meta0'] = 1 << (23 if layout == 'legacy' else 31)
        return original, decision

    def test_actual_bit_transition_both_layouts(self):
        for layout in ('legacy', 'rx-v2'):
            result = metadata_trust(self.packets(layout), layout)
            self.assertEqual(result['local_ip_out_with_old_valid_tag'], 1)
            self.assertEqual(result['local_ip_to_bridge_only_alg_set'], 1)

    def test_flow_time_and_other_metadata_changes_do_not_match(self):
        for change in ({'sport': 5202}, {'iif': 4}, {'no_fdb': 1},
                       {'meta2': 1}, {'ns': 600_000_000}, {'meta0': 1}):
            original, decision = self.packets('legacy')
            decision.update(change)
            result = metadata_trust([original, decision], 'legacy')
            self.assertEqual(result.get('local_ip_to_bridge_only_alg_set', 0), 0)

    def test_descriptor_layout_must_be_explicit(self):
        with self.assertRaises(ValueError):
            metadata_trust([], 'auto')
        result = metadata_trust(self.packets('legacy'), 'rx-v2')
        self.assertEqual(result.get('local_ip_out_with_old_valid_tag', 0), 0)


if __name__ == '__main__':
    unittest.main()
