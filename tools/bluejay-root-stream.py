#!/usr/bin/env python3
"""Stream a validated 16GiB HFSX image as bounded RAW and zero FILL chunks.

The encoder covers every image block, so old ext4 data cannot survive in a
hole. It never emits DONT_CARE. It reads the immutable image twice: once to
count chunks and verify its hash, then to send the bounded chunk stream.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import sys
import time

BLOCK = 4096
STEP = 8 << 20
SIZE = 16 << 30
RAW, FILL = 0xcac1, 0xcac2

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
parser.add_argument('--manifest', required=True, type=Path)
args = parser.parse_args()
manifest = json.loads(args.manifest.read_text())
assert args.image.stat().st_size == SIZE
assert manifest['size'] == SIZE and manifest['hfs_verified'] is True
assert manifest['build'] == '20H392'
assert len(manifest['sha256']) == 64

def pieces(stream):
    while data := stream.read(STEP):
        assert len(data) % BLOCK == 0
        start, zero = 0, not any(data[:BLOCK])
        for offset in range(BLOCK, len(data), BLOCK):
            empty = not any(data[offset:offset + BLOCK])
            if empty != zero:
                yield zero, data[start:offset]
                start, zero = offset, empty
        yield zero, data[start:]

stat = args.image.stat()
identity = (stat.st_size, stat.st_mtime_ns, stat.st_ino, stat.st_dev)
count, digest = 0, hashlib.sha256()
with args.image.open('rb') as image:
    for zero, data in pieces(image):
        count += 1
        digest.update(data)
assert digest.hexdigest() == manifest['sha256'], 'root image changed after verification'
print(f'ROOT_STREAM_VERIFIED {count} chunks sha256={digest.hexdigest()}', file=sys.stderr, flush=True)
stat = args.image.stat()
assert identity == (stat.st_size, stat.st_mtime_ns, stat.st_ino, stat.st_dev)

output = sys.stdout.buffer
output.write(struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, BLOCK,
                         SIZE // BLOCK, count, 0))
done, report = 0, time.monotonic()
with args.image.open('rb') as image:
    for zero, data in pieces(image):
        if zero:
            output.write(struct.pack('<HHIII', FILL, 0, len(data) // BLOCK, 16, 0))
        else:
            output.write(struct.pack('<HHII', RAW, 0, len(data) // BLOCK, 12 + len(data)))
            output.write(data)
        done += len(data)
        now = time.monotonic()
        if now - report >= 10:
            print(f'ROOT_STREAM_PROGRESS {done}/{SIZE}', file=sys.stderr, flush=True)
            report = now
output.flush()
assert done == SIZE
stat = args.image.stat()
assert identity == (stat.st_size, stat.st_mtime_ns, stat.st_ino, stat.st_dev), 'image modified while streaming'
print('ROOT_STREAM_COMPLETE', file=sys.stderr, flush=True)
