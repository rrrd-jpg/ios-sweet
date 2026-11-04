#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim_mt.c console.c; do $CC $CFLAGS -c $f -o out/${f%.*}.o; done
abs=$(aarch64-linux-gnu-readelf -r out/shim_mt.o out/console.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations" >&2; exit 1; }
$CC $CFLAGS -static -T ../shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
    out/start.o out/shim_mt.o out/console.o -o out/shim.elf
aarch64-linux-gnu-objcopy -O binary out/shim.elf out/Image
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz ../../copy/tail.bin > out/Image.gz-dtb
mkbootimg --kernel out/Image.gz-dtb \
  --ramdisk ../../dtb/recovery_unpacked/ramdisk \
  --base 0x40000000 --kernel_offset 0x00080000 \
  --ramdisk_offset 0x04000000 --tags_offset 0x0e000000 \
  --pagesize 2048 --header_version 0 \
  --cmdline 'bootopt=64S3,32N2,64N2 androidboot.selinux=permissive androidboot.bootdevice=mtk-msdc.0' \
  -o out/mt6795-shim.img
ls -l out/Image out/mt6795-shim.img
