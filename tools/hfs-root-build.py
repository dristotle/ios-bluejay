#!/usr/bin/env python3
"""Guest-side HFS+ construction from read-only APFS/9p inputs."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys

def run(*args):
    print('+', *args, flush=True)
    subprocess.run(args, check=True)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('device')
parser.add_argument('mountpoint')
parser.add_argument('--resume', action='store_true', help='keep the existing filesystem')
parser.add_argument('--prepare-only', action='store_true', help='repair/mount for a host rsync sender')
parser.add_argument('--finalize', action='store_true', help='skip copying; apply tweaks and verify')
parser.add_argument('--mounted', action='store_true', help='use an already mounted guest filesystem')
args = parser.parse_args()
device, mountpoint = args.device, args.mountpoint
root = Path(mountpoint)

def fsck_readwrite():
    repair = subprocess.run(['fsck.hfsplus', '-f', '-y', device],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print('\n'.join(line for line in repair.stdout.splitlines()
                    if 'Illegal name' not in line), flush=True)
    assert repair.returncode in (0, 1), ('fsck repair', repair.returncode)

if args.mounted:
    assert os.path.ismount(root), 'finalization requires a mounted filesystem'
elif args.resume or args.finalize:
    fsck_readwrite()
    run('mount', '-t', 'hfsplus', '-o', 'force', device, mountpoint)
else:
    run('mkfs.hfsplus', '-s', '-J', '-v', 'iOSBluejay', device)
    run('mount', '-t', 'hfsplus', '-o', 'force', device, mountpoint)

if args.prepare_only:
    print('HFS_ROOT_MOUNTED_FOR_RSYNC', flush=True)
    sys.exit(0)

if not args.finalize:
    for source, target in [('os', ''), ('sysos', 'private/preboot/Cryptexes/OS'),
                            ('appos', 'private/preboot/Cryptexes/App')]:
        dest = root / target
        dest.mkdir(parents=True, exist_ok=True)
        print(f'HFS_COPY_{source.upper()}', flush=True)
        run('rsync', '-aH', '--numeric-ids', '--info=stats2,progress2',
            f'/mnt/{source}/root/', str(dest) + '/')

# Expose cryptex files where ignition_level=1 expects them, sharing storage.
dsc = Path('System/Library/Caches/com.apple.dyld')
(root / dsc).mkdir(parents=True, exist_ok=True)
for cache in (root / 'private/preboot/Cryptexes/OS' / dsc).iterdir():
    dest = root / dsc / cache.name
    if os.path.lexists(dest):
        assert os.path.samefile(cache, dest), f'conflicting shared cache: {dest}'
    else:
        os.link(cache, dest)
for cryptex in ['OS', 'App']:
    source = root / 'private/preboot/Cryptexes' / cryptex
    for current, dirs, files in os.walk(source, followlinks=False):
        for name in files + [d for d in dirs if (Path(current) / d).is_symlink()]:
            src = Path(current) / name
            relative = src.relative_to(source)
            if relative.parts[:len(dsc.parts)] == dsc.parts:
                continue
            dest = root / relative
            if os.path.lexists(dest):
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            if src.is_symlink():
                dest.symlink_to(os.readlink(src))
                st = src.lstat()
                os.lchown(dest, st.st_uid, st.st_gid)
            else:
                os.link(src, dest)

def directory(path, owner, mode):
    path.mkdir(parents=True, exist_ok=True)
    os.chown(path, owner, owner)
    os.chmod(path, mode)

def save(path, value, owner=0, mode=0o600):
    with path.open('wb') as f:
        plistlib.dump(value, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, owner, owner)
    os.chmod(path, mode)

db = root / 'private/var/db'
for name, mode in [('diagnostics', 0o751), ('uuidtext', 0o755),
                    ('diagnostics/timesync', 0o755)]:
    dest = db / name
    directory(dest, 272, mode)
    (dest / '.migrated').touch()
    os.chown(dest / '.migrated', 272, 272)
    os.chmod(dest / '.migrated', 0o644)
directory(root / 'private/var/keybags', 0, 0o700)
save(root / 'private/var/keybags/systembag.kb', {'KeyBagKeys': b'SWEET'})
prefs = root / 'private/var/mobile/Library/Preferences'
directory(prefs, 501, 0o755)
run('python3', '/mnt/tools/sweet-root-tweaks.py', mountpoint)

# Seed the CurrentHost migration preference for bluejay's synthetic ECID.
namespace = bytes.fromhex('a6dd4ccbb5e84af5acddb6dc6a0542b8')
digest = bytearray(hashlib.sha1(namespace + (0x424c55454a415901).to_bytes(8, 'little')
                                 + (0x8015).to_bytes(4, 'little')).digest()[:16])
digest[6] = (digest[6] & 0x0f) | 0x50
digest[8] = (digest[8] & 0x3f) | 0x80
platform_uuid = str(__import__('uuid').UUID(bytes=bytes(digest)))
directory(prefs / 'ByHost', 501, 0o755)
for name in {platform_uuid.upper(), platform_uuid.lower()}:
    save(prefs / 'ByHost' / f'com.apple.migration.{name}.plist',
         {'LastSystemVersion': '20H392'}, owner=501)
print('BLUEJAY_PLATFORM_UUID', platform_uuid, flush=True)

# Keep a reproducible inventory of the launch-critical files inside the image.
critical = ['sbin/launchd', 'usr/lib/dyld', 'System/Library/xpc/launchd.plist',
            'System/Library/CoreServices/SpringBoard.app/SpringBoard',
            'usr/libexec/backboardd', 'System/Library/CoreServices/SystemVersion.plist']
manifest = {}
for name in critical:
    path = root / name
    assert path.is_file(), name
    st = path.stat()
    manifest[name] = {'size': st.st_size, 'uid': st.st_uid, 'gid': st.st_gid,
                      'mode': oct(st.st_mode & 0o7777),
                      'sha256': hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()}
version = plistlib.load((root / critical[-1]).open('rb'))
assert version['ProductBuildVersion'] == '20H392', version
assert (root / 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64').is_file()
with (root / 'private/var/db/bluejay-root-manifest.json').open('w') as f:
    json.dump(manifest, f, indent=2)
print('HFS_ROOT_MANIFEST', json.dumps(manifest), flush=True)
run('df', '-h', mountpoint)
run('sync')
run('umount', mountpoint)
# Apple fsck repairs Linux HFS+ Unicode table differences before final checking.
fsck_readwrite()
run('fsck.hfsplus', '-f', '-n', device)
print('HFS_ROOT_VERIFIED', flush=True)
