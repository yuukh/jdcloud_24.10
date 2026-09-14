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


def interfaces(text):
    """Use snapshot interface indexes; never assume eth0/rax0 numbering."""
    return {int(index): name.split('@')[0] for index, name in
            re.findall(r'^([0-9]+): ([^ :]+):', text, re.M)}


def metadata_trust(rows, layout):
    """Audit an explicitly selected packed HNAT layout; never auto-guess it."""
    if layout not in ('legacy', 'rx-v2'):
        raise ValueError('Specify the layout of the running HNAT module')
    alg_bit = 23 if layout == 'legacy' else 31
    counts, origins, examples = Counter(), {}, []
    for row in rows:
        if row['iif'] or row['no_fdb']:
            continue
        tag = ((row['meta1'] >> 4) if layout == 'legacy' else (row['meta2'] >> 16)) & 0xffff
        key = identity(row)
        if row['stage_name'] == 'ip_out':
            origins[key] = row
            if tag == 0x6789:
                counts['local_ip_out_with_old_valid_tag'] += 1
        elif row['stage_name'] == 'bridge_decision':
            original = origins.get(key)
            if (original is None or tag != 0x6789 or
                    not 0 <= row['ns'] - original['ns'] <= 500_000_000):
                continue
            if (row['meta0'] ^ original['meta0'] == 1 << alg_bit and
                    row['meta0'] & (1 << alg_bit) and
                    row['meta1'] == original['meta1'] and row['meta2'] == original['meta2']):
                counts['local_ip_to_bridge_only_alg_set'] += 1
                if len(examples) < 4:
                    examples.append({'ip_out_ns': original['ns'], 'bridge_ns': row['ns'],
                                     'seq': row['seq'], 'payload': row['payload'],
                                     'before_meta0': f'{original["meta0"]:08x}',
                                     'after_meta0': f'{row["meta0"]:08x}'})
    return {'layout': layout, **counts, 'examples': examples,
            'limits': 'Requires the correct compiled HNAT descriptor layout. '
                      'A metadata mutation is not proof of the cause of duplicate reception.'}


def span_coverage(intervals, length):
    """Minimum/maximum observed coverage, not the number of on-air copies."""
    edges = Counter({0: 0, length: 0})
    for left, right in intervals:
        edges[left] += 1
        edges[right] -= 1
    positions = sorted(edges)
    count, minimum, maximum = 0, len(intervals), 0
    for position, following in zip(positions, positions[1:]):
        count += edges[position]
        if following > position:
            minimum, maximum = min(minimum, count), max(maximum, count)
    return minimum, maximum


def dsack_paths(rows, metadata=None, names=None, lookback_ns=500_000_000):
    """Correlate reverse-flow DSACK spans against retained TX stage ranges.

    A 64-KiB sequence bucket index handles GSO, segmentation and wrap without
    a quadratic full-trace scan. Arbitrarily large untrusted spans are bounded.
    Multiple adjacent software segments count as one coverage, not duplicates.
    """
    metadata, names = metadata or {}, names or {}
    if not 0 < lookback_ns < 2**63:
        raise ValueError('Positive bounded lookback required')
    stages = {'ip_out', 'cpu_inject', 'qdma_map_complete', 'device_xmit', 'ppe_rx'}
    maximum_span = 1 << 20
    index = defaultdict(list)
    indexed, skipped = {}, 0

    def buckets(seq, length):
        return {bucket & 0xffff for bucket in range(seq >> 16, ((seq + length - 1) >> 16) + 1)}

    for number, row in enumerate(rows):
        if row['stage_name'] not in stages or not row['payload']:
            continue
        if not 0 < row['payload'] <= maximum_span:
            skipped += 1
            continue
        indexed[number] = row
        for bucket in buckets(row['seq'], row['payload']):
            index[(flow(row), bucket)].append(number)

    counts, spans, patterns, examples = Counter(), Counter(), Counter(), {}
    for ack in rows:
        if ack['stage_name'] != 'tcp_ack' or not ack['dsack'] or not ack['sacks']:
            continue
        left, right = ack['sacks'][0]
        length = (right - left) & 0xffffffff
        if not 0 < length <= maximum_span:
            counts['invalid_or_excessive_dsack_spans'] += 1
            continue
        counts['retained_dsack_acks'] += 1
        spans[length] += 1
        downflow = (ack['dst'], ack['src'], ack['dport'], ack['sport'])
        candidates = {number for bucket in buckets(left, length)
                      for number in index.get((downflow, bucket), ())}
        covered, sample = defaultdict(list), []
        for number in sorted(candidates):
            row = indexed[number]
            if not 0 <= ack['ns'] - row['ns'] <= lookback_ns:
                continue
            offset = ((row['seq'] - left + 2**31) & 0xffffffff) - 2**31
            start, end = max(0, offset), min(length, offset + row['payload'])
            if start >= end:
                continue
            label = row['stage_name']
            if label == 'device_xmit':
                device = names.get(row.get('ifindex', 0), 'ifindex-' + str(row.get('ifindex', 0)))
                label += ':' + device
            covered[label].append((start, end))
            if len(sample) < 12:
                sample.append({key: row[key] for key in
                               ('ns', 'stage_name', 'seq', 'payload', 'ip_id')})
        pattern = tuple((stage, *span_coverage(intervals, length))
                        for stage, intervals in sorted(covered.items()))
        patterns[pattern] += 1
        if pattern not in examples and len(examples) < 16:
            examples[pattern] = {'ack_ns': ack['ns'], 'ack': ack['ack'],
                                 'span': [left, right], 'observations': sample}
    overwritten = sum(cpu['overwritten'] for cpu in metadata.get('cpu_counts', []))
    return {
        **counts, 'lookback_ns': lookback_ns, 'overwritten_records': overwritten,
        'skipped_excessive_data_spans': skipped,
        'dsack_span_lengths': dict(sorted(spans.items())),
        'patterns': [{'acks': count, 'coverage': {
            stage: {'minimum_observations': low, 'maximum_observations': high}
            for stage, low, high in pattern}, 'example': examples.get(pattern)}
            for pattern, count in patterns.most_common()],
        'limits': [
            'Only retained records in the stated pre-ACK time window are compared.',
            'Each stage is separate; a GSO span is not a wire packet count.',
            'Minimum zero means incomplete observed span, not a network drop.',
            'One observed submission does not prove one hardware transmission.',
            'DSACK is receiver feedback, not an independent capture of received data.',
            'Overwritten history cannot establish absence of earlier transmissions.',
        ],
    }


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


def snapshot_section(text, name):
    match = re.search(r'^=== ' + re.escape(name) + r' ===\s*\n(.*?)(?=^=== |\Z)',
                      text, re.M | re.S)
    return match[1] if match else ''


def snapshot_uptime(text):
    match = re.search(r'^\s*(\d+(?:\.\d+)?)\s+\d+(?:\.\d+)?\s*$',
                      snapshot_section(text, 'time'), re.M)
    return float(match[1]) if match else None


def kernel_warning_audit(before, after):
    """Distinguish old boot warnings from new timestamped observations.

    A warning outside the supplied ring window cannot be counted.  A warning
    during a round is system-wide evidence, not proof that iperf caused it.
    """
    pattern = r'BUG:|WARNING:|Call trace:|NETDEV WATCHDOG|oom-kill|Oops:|KASAN:|UBSAN:|Kernel panic'

    def warnings(text):
        log = snapshot_section(text, 'kernel-log-tail')
        return list(dict.fromkeys(line for line in log.splitlines() if re.search(pattern, line)))

    old, new = warnings(before), warnings(after)
    start, end = snapshot_uptime(before), snapshot_uptime(after)
    reset = start is not None and end is not None and end < start
    pre_existing, during, unknown = list(old), [], []
    for line in new:
        if line in old and not reset:
            continue
        match = re.match(r'^\[\s*(\d+(?:\.\d+)?)\]', line)
        stamp = float(match[1]) if match else None
        if reset or start is None or stamp is None:
            unknown.append(line)
        elif stamp < start:
            if line not in pre_existing:
                pre_existing.append(line)
        elif end is not None and stamp <= end:
            during.append(line)
        else:
            unknown.append(line)
    return {
        'before_uptime': start, 'after_uptime': end, 'clock_reset_detected': reset,
        'pre_existing': pre_existing, 'new_during_round': during,
        'unclassified': unknown,
        'limits': 'System-wide bounded kernel log windows, not a complete boot history. '
                  'Repeated pre-existing lines are not new warnings in every round. '
                  'A warning during a round does not establish causation by that flow.',
    }


def retransmission_intervals(server):
    """Retain each iperf interval with retransmits, not only its average rate."""
    result = []
    for interval in server.get('intervals', []):
        total = interval.get('sum', {})
        if total.get('retransmits', 0) > 0:
            result.append({key: total.get(key) for key in
                           ('start', 'end', 'seconds', 'bits_per_second', 'retransmits')})
    return result


def analyze(archive_path, clients=None, metadata_layout=None):
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
                'kernel_warning_audit': kernel_warning_audit(before, after),
                'retransmission_intervals': retransmission_intervals(server),
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
                    'dsack_acks', 'freeze_reason', 'stage_counts', 'bridge_decisions', 'ppe_cpu_reasons')}
                round_result['trace']['repeated_header_key_counts'] = {
                    key: len(value) for key, value in basic['repeated_header_keys'].items()}
                round_result['trace']['freeze_after_arm_seconds'] = (
                    metadata['frozen_ns'] - metadata['started_ns']) / 1_000_000_000
                if 'tcp-ack-counters=total_retrans,reord_seen,dsack_dups' in before:
                    acks = [row for row in rows if row['stage_name'] == 'tcp_ack']
                    round_result['trace']['retained_ack_counter_maxima'] = {
                        name: max((row[field] for row in acks), default=None)
                        for name, field in (('total_retrans', 'a'), ('reord_seen', 'b'), ('dsack_dups', 'c'))}
                round_result['provenance'] = provenance(rows)
                if metadata_layout is not None:
                    round_result['metadata_trust'] = metadata_trust(rows, metadata_layout)
                round_result['dsack_path_coverage'] = dsack_paths(rows, metadata, interfaces(before))
                round_result['foe_lookup_reasons'] = dict(Counter(
                    row['a'] for row in rows if row['stage_name'] == 'foe_lookup'))
            result['rounds'][label] = round_result
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--clients', type=Path)
    parser.add_argument('--hnat-metadata-layout', choices=('legacy', 'rx-v2'),
                        help='Only set after verifying the running module descriptor layout')
    args = parser.parse_args()
    print(json.dumps(analyze(args.archive, args.clients, args.hnat_metadata_layout), indent=2))
