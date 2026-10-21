#!/usr/bin/env python3
"""Verify the Pixel v4 header, legacy LZ4 kernel and ARM64 Image size."""
import hashlib, struct, subprocess, sys
from pathlib import Path
boot = Path(sys.argv[1]).read_bytes()
raw = Path(sys.argv[2]).read_bytes()
assert boot[:8] == b'ANDROID!'
kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<4I', boot, 8)
assert struct.unpack_from('<I', boot, 40)[0] == 4
assert header_size == 1584
kernel = boot[4096:4096 + kernel_size]
assert kernel[:4] == bytes.fromhex('02214c18'), 'kernel is not legacy LZ4'
decoded = subprocess.run(['lz4', '-d', '-c'], input=kernel, capture_output=True, check=True).stdout
assert decoded == raw
assert raw[56:60] == b'ARM\x64'
assert struct.unpack_from('<Q', raw, 8)[0] == 0x80000
assert struct.unpack_from('<Q', raw, 16)[0] == len(raw)
rd_offset = 4096 + ((kernel_size + 4095) & ~4095)
assert boot[rd_offset:rd_offset + ramdisk_size][:2] == b'\x1f\x8b'
print(f'verified v4 + legacy LZ4: Image={len(raw)} boot={len(boot)} sha256={hashlib.sha256(boot).hexdigest()}')
