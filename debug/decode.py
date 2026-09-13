#!/usr/bin/env python3
"""Validate bounded HNAT418 binary metadata; never extract untrusted archives."""
import argparse
from collections import Counter, defaultdict
import ipaddress
import json
from pathlib import Path
import struct
import tarfile

HEADER = struct.Struct('<8sIIIIQQ4s4sIIII')
CPU = struct.Struct('<IIQQQ')
RECORD = struct.Struct('<QQ16I4s4s8I8H8B')
STAGES = ('unused', 'ip_out', 'bridge_decision', 'cpu_prepare', 'cpu_inject',
          'device_xmit', 'qdma_map_complete', 'dma_skb_release', 'ppe_rx',
          'foe_lookup', 'wifi_bind', 'tcp_ack', 'prepare_drop', 'qdma_busy', 'qdma_error')


def record(values, cpu):
    r = dict(zip(('ns', 'serial', 'cookie', 'seq', 'ack', 'payload', 'skb_len', 'mark', 'cb44',
                  'meta0', 'meta1', 'meta2', 'a', 'b', 'c', 'ifindex', 'iif', 'gso_type'), values[:18]))
    r.update(cpu=cpu, src=str(ipaddress.IPv4Address(values[18])),
             dst=str(ipaddress.IPv4Address(values[19])),
             sacks=[list(values[i:i + 2]) for i in range(20, 20 + min(values[42], 4) * 2, 2)])
    r.update(zip(('sport', 'dport', 'ip_id', 'gso_size', 'gso_segs', 'queue', 'headroom', 'netoff',
                  'stage', 'flags', 'ttl', 'checksum', 'to_ppe', 'no_fdb', 'sack_count', 'dsack'), values[28:]))
    if not 1 <= r['stage'] < len(STAGES):
        raise ValueError('Unknown event stage')
    r['stage_name'] = STAGES[r['stage']]
    if r['netoff'] & 0x8000:
        r['netoff'] -= 0x10000
    return r


def parse(data):
    if len(data) < HEADER.size:
        raise ValueError('Truncated recorder header')
    magic, version, size, slots, cpus, started, frozen, server, peer, port, hw, dsacks, reason = HEADER.unpack_from(data)
    if magic != b'H418RING' or version != 1 or size != RECORD.size:
        raise ValueError('Unsupported recorder ABI')
    if not 1 <= cpus <= 16 or not 1 <= slots <= 16384:
        raise ValueError('Invalid bounded recorder dimensions')
    if len(data) != HEADER.size + cpus * (CPU.size + slots * size):
        raise ValueError('Truncated or extended recorder; do not interpret missing events')
    rows, counts = [], []
    offset = HEADER.size
    for expected_cpu in range(cpus):
        cpu, reserved, count, overwritten, reserved2 = CPU.unpack_from(data, offset)
        offset += CPU.size
        if cpu != expected_cpu or overwritten != max(0, count - slots) or reserved or reserved2:
            raise ValueError('Invalid CPU header')
        found = []
        for values in RECORD.iter_unpack(data[offset:offset + slots * size]):
            if not values[1]:
                continue
            found.append(values[1])
            rows.append(record(values, cpu))
        offset += slots * size
        if sorted(found) != list(range(max(1, count - slots + 1), count + 1)):
            raise ValueError('Recorder serial numbers have gaps or duplicates')
        counts.append({'cpu': cpu, 'recorded': count, 'retained': len(found), 'overwritten': overwritten})
    rows.sort(key=lambda r: (r['ns'], r['cpu'], r['serial']))
    return dict(version=version, started_ns=started, frozen_ns=frozen,
                server=str(ipaddress.IPv4Address(server)), peer=str(ipaddress.IPv4Address(peer)),
                port=port, hardware=hw, dsack_acks=dsacks, freeze_reason=reason, cpu_counts=counts), rows


def summarize(metadata, rows):
    counts = Counter(r['stage_name'] for r in rows)
    repeated = {}
    for stage in ('ip_out', 'cpu_inject', 'qdma_map_complete'):
        keys = defaultdict(list)
        for r in rows:
            if r['stage_name'] == stage and r['payload']:
                keys[(r['sport'], r['dport'], r['seq'], r['payload'], r['ip_id'])].append(r)
        repeated[stage] = [
            {'sport': key[0], 'dport': key[1], 'seq': key[2], 'payload': key[3], 'ip_id': key[4],
             'observations': len(items), 'ns': [r['ns'] for r in items[:8]],
             'cookies': [r['cookie'] for r in items[:8]]}
            for key, items in keys.items() if len(items) > 1]
    decisions = Counter((r['a'], r['b']) for r in rows if r['stage_name'] == 'bridge_decision')
    exceptions = Counter(r['a'] for r in rows if r['stage_name'] == 'ppe_rx')
    result = {
        **metadata,
        'retained_interval_ns': [rows[0]['ns'], rows[-1]['ns']] if rows else [],
        'stage_counts': dict(counts),
        'bridge_decisions': [{'from_extge': a, 'has_ingress_info': b, 'count': n}
                             for (a, b), n in decisions.items()],
        'ppe_cpu_reasons': dict(exceptions), 'repeated_header_keys': repeated,
        'sack_events': [r for r in rows if r['stage_name'] == 'tcp_ack'],
        'candidate_bypass_decisions': [r for r in rows if r['stage_name'] == 'bridge_decision'
                                     and (r['a'] or r['b'])][:200],
        'limits': [
            'Bounded retained window, not a lossless recording of the whole test.',
            'Per-CPU overwritten history is explicit; absent history proves nothing.',
            'IP_ID/sequence/header equality is not a payload equality or air-delivery proof.',
            'QDMA_MAP is completed descriptor construction before publication; DMA_RELEASE is not a Wi-Fi ACK.',
            'SACK is observed at TCP receive entry; compare actual socket/TCP acceptance counters.',
            'No direct visibility into autonomous WED firmware or Windows NIC duplicate delivery.',
        ],
    }
    if metadata['hardware'] and not counts['qdma_map_complete']:
        result['warning'] = 'No matching QDMA records retained. Do not claim hardware offload is proven.'
    return result


def process(input_path, output, full_events=False):
    output.mkdir(parents=True, exist_ok=True)
    reports = {}
    if input_path.suffix == '.bin':
        sources = [(input_path.name, input_path.read_bytes())]
    else:
        sources = []
        with tarfile.open(input_path, 'r:*') as archive:
            for member in archive:
                if not member.isfile() or not member.name.endswith('/records.bin'):
                    continue
                if member.size > 40 * 1024 * 1024 or len(sources) >= 4:
                    raise ValueError('Unreasonably large recorder archive')
                with archive.extractfile(member) as stream:
                    sources.append((member.name, stream.read()))
    if not sources:
        raise ValueError('No recorder snapshots found in this input')
    for name, data in sources:
        metadata, rows = parse(data)
        label = Path(name).parent.name if '/' in name else input_path.stem
        if label in reports:
            raise ValueError('Duplicate snapshot label')
        reports[label] = summarize(metadata, rows)
        if full_events:
            with (output / f'events-{metadata["port"]}.jsonl').open('w') as stream:
                for row in rows:
                    stream.write(json.dumps(row, separators=(',', ':')) + '\n')
    (output / 'summary.json').write_text(json.dumps(reports, indent=2) + '\n')
    for name, report in reports.items():
        print(name, 'port=', report['port'], 'DSACK=', report['dsack_acks'],
              'stages=', report['stage_counts'])
    return reports


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, default=Path('hnat418-report'))
    parser.add_argument('--full-events', action='store_true')
    args = parser.parse_args()
    process(args.input, args.output, args.full_events)
