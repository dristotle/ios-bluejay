#!/usr/bin/env python3
"""Build the bluejay Apple device tree from verified Pixel 6a hardware facts.

Usage: mkbluejayadt.py OUT.adt [--dump]
The iPhone X product properties come from the user's IPSW via mkadt.py.
"""
import hashlib
import sys

import adt
import mkadt


def setprop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = key, flags, value
            return
    node.props.append((name, 0, value))


def build(seed=None):
    mkadt.HIGH_PHYS_BASE = 0x880000000
    mkadt.HIGH_MEM_SIZE = 0x100000000
    mkadt.GICD, mkadt.GICR = 0x10400000, 0x10440000
    mkadt.FB, mkadt.FB_SIZE = 0xfac00000, 1080 * 2400 * 4
    mkadt.UFS, mkadt.UFS_SIZE = 0x14700000, 0x2000
    mkadt.TIMEBASE, mkadt.VTIMER_INTID = 24000000, 27
    mkadt.SWEET_ECID = 0x424c55454a415901
    mkadt.BOOT_MANIFEST_HASH = hashlib.sha384(b'bluejay boot manifest').digest()
    tree = mkadt.build(seed=seed, highmem=True)
    # The final SWEET entry matches the inherited generic IOKit stand-ins.
    setprop(tree, 'compatible', b'D22AP\0iPhone10,3\0google,bluejay\0samsung,gs101\0xiaomi,sweet\0AppleARM\0')
    chosen = next(n for n in tree.children if n.name == 'chosen')
    setprop(chosen, 'firmware-version', b'bluejay-shim\0')
    setprop(chosen, 'system-firmware-version', b'bluejay-shim\0')
    # Built-in app installation IDs require MobileGestalt's device identity.
    # Without this iBoot/syscfg property it waits for IONetworkController,
    # which has no driver yet. Use a stable locally administered address.
    setprop(chosen, 'mac-address-wifi0', bytes.fromhex('02424c554501'))
    cpus = next(n for n in tree.children if n.name == 'cpus')
    for core, cpu in enumerate(cpus.children):
        kind = 'cortex-a55' if core < 4 else 'cortex-a78' if core < 6 else 'cortex-x1'
        # SweetCPU is the inherited PSCI/GIC driver. Its IOKit personality
        # still matches this alias; retain the real Cortex identities first.
        setprop(cpu, 'compatible', ('arm,' + kind).encode()
                + b'\0ARM,v8\0qcom,kryo470\0')
        setprop(cpu, 'clock-frequency', mkadt.u32(1803000000 if core < 4 else 2253000000 if core < 6 else 2802000000))
    ufs = next(n for n in tree.children if n.name == 'sweet-ufs')
    setprop(ufs, 'compatible', b'samsung,exynos-ufs\0')
    setprop(ufs, 'userdata-start-block', mkadt.u64(2290150))
    setprop(ufs, 'userdata-block-count', mkadt.u64(14799694))
    return tree


if __name__ == '__main__':
    tree = build()
    blob = adt.serialize(tree)
    with open(sys.argv[1], 'wb') as f:
        f.write(blob)
    if '--dump' in sys.argv:
        adt.dump(tree)
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
