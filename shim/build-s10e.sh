#!/bin/bash
# Build the s10e probe shim and wrap it in a Samsung-style boot.img (header v1).
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -DBOARD_S10E"
O=out/s10e
mkdir -p $O
for f in start.S s10e.c console.c fdt.c; do
        $CC $CFLAGS -c $f -o $O/${f%.*}.o
done
abs=$(aarch64-linux-gnu-readelf -r $O/s10e.o $O/console.o $O/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
        $O/start.o $O/s10e.o $O/console.o $O/fdt.o -o $O/shim.elf
$OBJCOPY -O binary $O/shim.elf $O/Image
size=$(aarch64-linux-gnu-nm $O/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" $O/Image
(cd $O && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
if [ -n "${RD_MIB:-}" ]; then
python3 - "$RD_MIB" "$O/ramdisk.bin" <<'PY'
import array, sys
n = int(sys.argv[1]) * 1024 * 1024 // 4
a = array.array('I', ((i * 0x9E3779B1) & 0xffffffff for i in range(n)))
with open(sys.argv[2], 'wb') as f:
    a.tofile(f)
PY
fi
# Same addresses as the stock boot.img (header v1, 2 KiB pages, ramdisk addr 0).
mkbootimg --header_version 1 --kernel $O/Image --ramdisk $O/ramdisk.bin \
        --base 0 --kernel_offset 0x10008000 --ramdisk_offset 0 --second_offset 0 \
        --tags_offset 0x10000100 --pagesize 2048 --board SRPRI28A016KU \
        --os_version 16.0.0 --os_patch_level 2025-08 \
        --cmdline "loop.max_part=7" -o $O/s10e-boot.img
printf 'SEANDROIDENFORCE' >> $O/s10e-boot.img
ls -l $O/Image $O/s10e-boot.img
