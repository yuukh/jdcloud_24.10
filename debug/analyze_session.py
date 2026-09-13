#!/usr/bin/env python3
"""Audit an HNAT418 session without extracting the supplied archive.

Correlations are observations, not proof of wireless delivery or of causation.
In particular, a missing record in an overwritten ring is not a missing packet.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import tarfile

from decode import parse, summarize

LABELS = ('hardware-a', 'bypass', 'hardware-quiet', 'hardware-b')


def identity(row):
    return tuple(row[name] for name in
                 ('src', 'dst', 'sport', 'dport', 'seq', 'payload', 'ip_id', 'cookie'))


def flow(row):
    return tuple(row[name] for name in ('src', 'dst', 'sport', 'dport'))


def inside(seq, length, point):
    """TCP sequence containment, including a 32-bit wrap, for bounded lengths."""
    return 0 < length < 2**31 and ((point - seq) & 0xffffffff) < length


def provenance(rows):
    emitted = {}
    originals = defaultdict(list)
    decisions = Counter()
    stale, holes = [], []
    for row in rows:
        stage = row['stage_name']
        key = identity(row)
        if stage == 'ip_out':
            emitted[key] = row
            if row['payload']:
                originals[flow(row)].append({'row': row, 'stale': False})
        elif stage == 'bridge_decision':
            origin = emitted.get(key)
            # Match the same full tuple, sequence span, IP ID, cookie and time.
            # Never identify a locally generated packet from a cookie alone.
            if (origin is None or origin['iif'] or row['iif'] or row['no_fdb'] or
                    not 0 <= row['ns'] - origin['ns'] <= 500_000_000):
                continue
            decisions[(row['a'], row['b'])] += 1
            if row['a'] or row['b']:
                stale.append({name: row[name] for name in
                              ('ns', 'sport', 'dport', 'seq', 'payload', 'ip_id',
                               'iif', 'ttl', 'a', 'b', 'to_ppe', 'no_fdb')})
                stale[-1].update(ip_out_ns=origin['ns'],
                                 metadata=[f'{row[n]:08x}' for n in ('meta0', 'meta1', 'meta2')])
                for entry in reversed(originals[flow(row)]):
                    if entry['row'] is origin:
                        entry['stale'] = True
                        break
        elif stage == 'tcp_ack' and row['sacks']:
            # An ordinary SACK above the cumulative ACK reports a hole. The
            # first block of a DSACK is not an ordinary SACK block.
            sacks = row['sacks'][1:] if row['dsack'] else row['sacks']
            if not any(0 < ((left - row['ack']) & 0xffffffff) < 2**31
                       for left, _right in sacks):
                continue
            downflow = (row['dst'], row['src'], row['dport'], row['sport'])
            matches = [entry for entry in originals[downflow]
                       if inside(entry['row']['seq'], entry['row']['payload'], row['ack'])]
            # Select the first observed transmission, not a later retransmit.
            if matches and matches[0]['stale']:
                holes.append({'ack_ns': row['ns'], 'ack': row['ack'],
                              'original_seq': matches[0]['row']['seq'],
                              'original_ns': matches[0]['row']['ns'],
                              'sacks': sacks})
    return {
        'matched_local_bridge_decisions': sum(decisions.values()),
        'local_decision_counts': [{'from_extge': a, 'has_ingress_info': b, 'count': n}
                                  for (a, b), n in sorted(decisions.items())],
        'local_packets_classified_as_ingress': len(stale),
        'local_data_packets_classified_as_ingress': sum(bool(r['payload']) for r in stale),
        'local_ingress_classification_examples': stale[:8],
        'sack_holes_in_first_observed_stale_transmission': len(holes),
        'distinct_stale_originals_with_sack_holes': len({(r['original_ns'], r['original_seq']) for r in holes}),
        'sack_hole_examples': holes[:8],
        'limits': 'A SACK hole correlation does not by itself identify why a packet was late.',
    }


def tcp_counters(text):
    result = {}
    for prefix in ('Tcp:', 'TcpExt:'):
        lines = [line.split()[1:] for line in text.splitlines() if line.startswith(prefix)]
        if len(lines) >= 2:
            result.update(zip(lines[0], map(int, lines[1])))
    return result


def analyze(archive_path, clients=None):
    with tarfile.open(archive_path, 'r:*') as archive:
        members = {}
        total = 0
        for member in archive:
            total += member.size
            if total > 128 * 1024 * 1024 or len(members) >= 128:
                raise ValueError('Evidence archive exceeds diagnostic bounds')
            if member.isfile():
                if member.name in members or member.size > 40 * 1024 * 1024:
                    raise ValueError('Duplicate or excessive evidence member')
                members[member.name] = member
        roots = [name[:-len('/session.txt')] for name in members if name.endswith('/session.txt')]
        if len(roots) != 1:
            raise ValueError('Expected exactly one diagnostic session')
        root = roots[0]

        def read(name):
            member = members.get(root + '/' + name)
            if member is None:
                return b''
            with archive.extractfile(member) as stream:
                return stream.read()

        result = {'session': root, 'archive_sha256': hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                  'session_text': read('session.txt').decode(), 'rounds': {}}
        for label, port in zip(LABELS, range(5201, 5205)):
            prefix = label + '/'
            server = json.loads(read(prefix + 'iperf-server.json'))
            before = read(prefix + 'before.txt').decode()
            after = read(prefix + 'after.txt').decode()
            timeline = read(prefix + 'timeline.txt').decode()
            old, new = tcp_counters(before), tcp_counters(after)
            counter_scope = 'before/after snapshots; system-wide, not per socket'
            if not old or not new:
                # Older firmware only saved these counters in the timeline.
                samples = [sample for sample in timeline.split('=== sample=')
                           if tcp_counters(sample)]
                old = tcp_counters(samples[0]) if samples else {}
                new = tcp_counters(samples[-1]) if samples else {}
                counter_scope = 'first/last timeline sample; system-wide, excludes unsampled tail'
            round_result = {
                'port': port, 'server_end': server.get('end'),
                'result': read(prefix + 'result.txt').decode(),
                'tcp_counter_scope': counter_scope if old and new else 'unavailable',
                'tcp_counter_deltas': {key: new[key] - old[key] for key in old if key in new
                                       and new[key] != old[key] and re.search(
                                           'Retrans|DSACK|Reorder|SACK|Loss|Spurious|InSegs|OutSegs', key, re.I)},
                'kernel_warnings': [line for line in after.splitlines() if re.search(
                    r'BUG:|WARNING:|Call trace:|NETDEV WATCHDOG|oom-kill|Oops:', line)],
            }
            sockets = [line.strip() for line in timeline.splitlines()
                       if 'cubic ' in line and 'bytes_sent:' in line]
            round_result['last_socket_samples'] = sockets[-2:]
            if clients is not None:
                client = json.loads((clients / f'client-{port}.json').read_text(encoding='utf-8-sig'))
                round_result['client_end'] = client.get('end')
            data = read(prefix + 'records.bin')
            if data:
                metadata, rows = parse(data)
                if metadata['port'] != port:
                    raise ValueError('Recorder port does not match its directory')
                basic = summarize(metadata, rows)
                round_result['trace'] = {key: basic[key] for key in (
                    'started_ns', 'frozen_ns', 'retained_interval_ns', 'cpu_counts',
                    'dsack_acks', 'stage_counts', 'bridge_decisions', 'ppe_cpu_reasons')}
                round_result['trace']['repeated_header_key_counts'] = {
                    key: len(value) for key, value in basic['repeated_header_keys'].items()}
                round_result['provenance'] = provenance(rows)
                round_result['foe_lookup_reasons'] = dict(Counter(
                    row['a'] for row in rows if row['stage_name'] == 'foe_lookup'))
            result['rounds'][label] = round_result
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--clients', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.archive, args.clients), indent=2))
