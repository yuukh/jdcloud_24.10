import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('decode', Path(__file__).parents[1] / 'decode.py')
decode = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decode)


def sample(count=1, slots=2):
    header = decode.HEADER.pack(b'H418RING', 1, 144, slots, 1, 10, 100,
                                b'\xc0\xa8\x03\x01', b'\xc0\xa8\x03\x89', 5201, 1, 0, 2)
    cpu = decode.CPU.pack(0, 0, count, max(0, count - slots), 0)
    records = []
    for position in range(slots):
        values = [0] * 44
        values[18] = b'\xc0\xa8\x03\x01'
        values[19] = b'\xc0\xa8\x03\x89'
        serials = [n for n in range(max(1, count - slots + 1), count + 1) if (n - 1) % slots == position]
        if serials:
            values[0], values[1] = 20 + serials[0], serials[0]
            values[3], values[5], values[28], values[29], values[36] = 101, 1460, 5201, 13737, 1
        records.append(decode.RECORD.pack(*values))
    return header + cpu + b''.join(records)


class DecoderTests(unittest.TestCase):
    def test_binary_layout(self):
        self.assertEqual(decode.HEADER.size, 64)
        self.assertEqual(decode.CPU.size, 32)
        self.assertEqual(decode.RECORD.size, 144)

    def test_parse(self):
        metadata, rows = decode.parse(sample())
        self.assertEqual(metadata['server'], '192.168.3.1')
        self.assertEqual(rows[0]['dport'], 13737)
        self.assertEqual(rows[0]['stage_name'], 'ip_out')

    def test_wrap_is_explicit(self):
        metadata, rows = decode.parse(sample(5))
        self.assertEqual([r['serial'] for r in rows], [4, 5])
        self.assertEqual(metadata['cpu_counts'][0]['overwritten'], 3)

    def test_truncated(self):
        with self.assertRaises(ValueError):
            decode.parse(sample()[:-1])

    def test_wrong_version(self):
        data = bytearray(sample())
        data[8] = 2
        with self.assertRaises(ValueError):
            decode.parse(data)

    def test_serial_hole_rejected(self):
        data = bytearray(sample())
        data[104] = 8
        with self.assertRaises(ValueError):
            decode.parse(data)

    def test_no_hardware_claim_without_records(self):
        metadata, rows = decode.parse(sample())
        result = decode.summarize(metadata, rows)
        self.assertIn('warning', result)
        self.assertTrue(result['limits'])

    def test_repeated_headers_are_not_wire_assertion(self):
        metadata, rows = decode.parse(sample(2))
        result = decode.summarize(metadata, rows)
        self.assertEqual(len(result['repeated_header_keys']['ip_out']), 1)
        self.assertNotIn('root_cause', result)


if __name__ == '__main__':
    unittest.main()
