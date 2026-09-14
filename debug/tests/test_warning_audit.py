from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from analyze_session import kernel_warning_audit, retransmission_intervals


def snapshot(uptime, log):
    return (f'\n=== time ===\nMon Sep 14 02:06:54 UTC 2026\n{uptime:.2f} 0.00\n'
            f'\n=== kernel-log-tail ===\n{log}\n\n=== recent-system-log ===\n'
            'Unrelated application WARNING: not a kernel event\n')


class WarningAuditTests(unittest.TestCase):
    def test_same_boot_warning_is_not_four_new_faults(self):
        warning = '[   67.273765] WARNING: CPU: 1 at nf_ct_ext_add'
        for start in (119, 182, 244, 305):
            result = kernel_warning_audit(snapshot(start, warning), snapshot(start + 60, warning))
            self.assertEqual(result['pre_existing'], [warning])
            self.assertEqual(result['new_during_round'], [])
            self.assertEqual(result['unclassified'], [])

    def test_timestamped_new_warning(self):
        warning = '[  130.000000] WARNING: new warning'
        result = kernel_warning_audit(snapshot(119, ''), snapshot(179, warning))
        self.assertEqual(result['new_during_round'], [warning])

    def test_newly_visible_old_warning_is_not_new(self):
        warning = '[  67.000000] WARNING: old warning'
        result = kernel_warning_audit(snapshot(119, ''), snapshot(179, warning))
        self.assertEqual(result['pre_existing'], [warning])
        self.assertEqual(result['new_during_round'], [])

    def test_reboot_and_missing_clock_do_not_invent_event_time(self):
        warning = '[  12.000000] WARNING: unknown boot'
        result = kernel_warning_audit(snapshot(119, ''), snapshot(20, warning))
        self.assertTrue(result['clock_reset_detected'])
        self.assertEqual(result['unclassified'], [warning])
        result = kernel_warning_audit('', snapshot(20, warning))
        self.assertEqual(result['unclassified'], [warning])

    def test_missing_kernel_section_does_not_parse_other_logs(self):
        self.assertEqual(kernel_warning_audit('', 'WARNING: application')['new_during_round'], [])

    def test_retransmissions_are_not_hidden_by_average(self):
        bad = dict(start=59, end=60, seconds=1, bits_per_second=861e6, retransmits=765)
        result = retransmission_intervals({'intervals': [
            {'sum': dict(start=0, end=1, retransmits=0)}, {'sum': bad}]})
        self.assertEqual(result, [bad])
        self.assertEqual(retransmission_intervals({}), [])


if __name__ == '__main__':
    unittest.main()
