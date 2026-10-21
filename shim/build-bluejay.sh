#!/bin/bash
# Build Pixel 6a temporary boot image; installed vendor_boot supplies the FDT.
set -euo pipefail
cd "$(dirname "$0")"
out=out/bluejay
mkdir -p "$out"
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align
       -fno-stack-protector -Wall -Wextra -DSHIM_BLUEJAY)
objects=()
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
    o="$out/${f%.*}.o"
    aarch64-linux-gnu-gcc "${flags[@]}" -c "$f" -o "$o"
    objects+=("$o")
done
if [[ -n ${1:-} ]]; then
    cp "$1" "$out/payload.bin"
else
    : > "$out/payload.bin"
fi
printf '.section .payload,"a"\n.balign 16\n.incbin "%s/payload.bin"\n' "$out" > "$out/payload.S"
aarch64-linux-gnu-gcc "${flags[@]}" -c "$out/payload.S" -o "$out/payload.o"
objects+=("$out/payload.o")
aarch64-linux-gnu-gcc "${flags[@]}" -static -T shim.ld -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments -Wl,-N "${objects[@]}" -o "$out/shim.elf"
aarch64-linux-gnu-objcopy -O binary "$out/shim.elf" "$out/Image"
size=$(aarch64-linux-gnu-nm "$out/shim.elf" | awk '$3=="_image_end"{print strtonum("0x"$1)}')
truncate -s "$size" "$out/Image"
# Pixel's bootloader decompresses the legacy LZ4 frame; Image cannot inflate itself.
lz4 -l -f -9 "$out/Image" "$out/Image.lz4"
mkdir -p "$out/rd"
(cd "$out/rd" && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.gz)
mkbootimg --header_version 4 --kernel "$out/Image.lz4" --ramdisk "$out/ramdisk.gz" \
    --cmdline "console=null" --os_version 16.0.0 --os_patch_level 2026-10 \
    -o "$out/boot.img"
python3 ../tools/check-bluejay-boot.py "$out/boot.img" "$out/Image"
sha256sum "$out/boot.img"
