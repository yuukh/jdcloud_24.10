#!/usr/bin/env python3
"""Identify all inputs, including uncommitted edits; no credential files read."""
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parent.parent
paths = [root / name for name in ('build-debug.sh', 'diy-part1.sh', 'diy-part2.sh',
                                   'feeds.conf.default', 'immortalwrt.config')]
paths += [p for directory in ('debug', 'patches') for p in (root / directory).rglob('*')
          if p.is_file() and '__pycache__' not in p.parts]
h = hashlib.sha256()
for p in sorted(paths):
    h.update(str(p.relative_to(root)).encode() + b'\0' + p.read_bytes())
print(h.hexdigest())
