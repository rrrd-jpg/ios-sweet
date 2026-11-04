# MT6795 (HTC One M9+) bring-up shim

Findings: LK enters at EL2 (shim drops to EL1), Cortex-A53 r0p2, 13 MHz timer,
GIC-400 at 0x10221000/0x10222000, panel is command-mode (needs mutex+DSI kick),
framebuffer 0xf79c0000 1440x2560, no 16K translation granule (TGRAN16 = NO).

Build needs `copy/tail.bin`: the appended DTB(s) of the stock recovery kernel.
Extract it with:
    python3 - <<'PY'
    import zlib
    d = open('dtb/recovery_unpacked/kernel','rb').read()
    z = zlib.decompressobj(31); z.decompress(d)
    open('copy/tail.bin','wb').write(z.unused_data)
    PY
Flash to the recovery partition (fastboot boot is not available on this phone).
