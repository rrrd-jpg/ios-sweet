.macro paint lo, hi
        movz    x9, #0xf79c, lsl #16
        movz    x10, #0x00e1, lsl #16
        add     x10, x9, x10
        movz    w5, #\lo
        movk    w5, #\hi, lsl #16
        orr     x5, x5, x5, lsl #32
7:      stp     x5, x5, [x9], #16
        cmp     x9, x10
        b.lo    7b
        dsb     sy
        movz    x6, #0x1402, lsl #16
        add     x6, x6, #0x20
        mov     w7, #1
        str     w7, [x6]
        movz    x2, #0xb000
        movk    x2, #0x1401, lsl #16
        movz    w3, #0x3909
        movk    w3, #0x002c, lsl #16
        str     w3, [x2, #0x200]
        str     w7, [x2, #0x60]
        str     wzr, [x2]
        str     w7, [x2]
        dsb     sy
        mov     x4, #0x4000000
8:      subs    x4, x4, #1
        b.ne    8b
.endm
        .section .text.head, "ax"
        .globl _head
_head:
        b       _start
        .long   0
        .quad   0
        .quad   _end
        .quad   0xa
        .quad   0, 0, 0
        .ascii  "ARM\x64"
        .long   0

        .text
        .globl _start
_start:
        msr     daifset, #0xf
        mov     x19, x0                 /* LK's DTB pointer */
        mrs     x20, CurrentEL          /* entry EL (8 = EL2) */
        cmp     x20, #8
        b.ne    el1_entry
        mov     x0, #(1 << 31)
        msr     hcr_el2, x0
        mrs     x0, cnthctl_el2
        orr     x0, x0, #3
        msr     cnthctl_el2, x0
        msr     cntvoff_el2, xzr
        mov     x0, #0x33ff
        msr     cptr_el2, x0
        msr     hstr_el2, xzr
        mrs     x0, midr_el1
        mrs     x1, mpidr_el1
        msr     vpidr_el2, x0
        msr     vmpidr_el2, x1
        movz    x0, #0x0800
        movk    x0, #0x30d0, lsl #16
        msr     sctlr_el1, x0
        mov     x0, #0x3c5
        msr     spsr_el2, x0
        adr     x0, el1_entry
        msr     elr_el2, x0
        eret
el1_entry:
        movz    x0, #0x1000, lsl #16
        add     x0, x0, #7, lsl #12
        movz    w1, #0x2200, lsl #16
        str     w1, [x0]
        movz    x9, #0xf79c, lsl #16
        movz    x10, #0x00e1, lsl #16
        add     x10, x9, x10
        movz    w5, #0xff00
        movk    w5, #0xff00, lsl #16
        orr     x5, x5, x5, lsl #32
7:      stp     x5, x5, [x9], #16
        cmp     x9, x10
        b.lo    7b
        dsb     sy
        movz    x6, #0x1402, lsl #16
        add     x6, x6, #0x20
        mov     w7, #1
        str     w7, [x6]
        movz    x2, #0xb000
        movk    x2, #0x1401, lsl #16
        movz    w3, #0x3909
        movk    w3, #0x002c, lsl #16
        str     w3, [x2, #0x200]
        str     w7, [x2, #0x60]
        str     wzr, [x2]
        str     w7, [x2]
        dsb     sy
        msr     spsel, #1
        adr     x1, _stack_top
        mov     sp, x1
        adr     x1, vectors
        msr     vbar_el1, x1
        isb
        paint 0xffff, 0xff00
        adr     x1, _bss_start
        adr     x2, _bss_end
1:      cmp     x1, x2
        b.hs    2f
        str     xzr, [x1], #8
        b       1b
        paint 0xff00, 0xffff
2:      mov     x0, x19
        mov     x1, x20
        paint 0x0000, 0xffff
        bl      shim_main
3:      wfe
        b       3b

.macro vent slot
        .balign 0x80
        mov     x0, #\slot
        b       exc_common
.endm
        .balign 0x800
        .globl vectors
vectors:
        vent 0
        vent 1
        vent 2
        vent 3
        vent 4
        vent 5
        vent 6
        vent 7
        vent 8
        vent 9
        vent 10
        vent 11
        vent 12
        vent 13
        vent 14
        vent 15
exc_common:
        bl      exc_handler
4:      wfe
        b       4b

        .text
        .globl kick_asm
kick_asm:
        dsb     sy
        movz    x6, #0x1402, lsl #16
        add     x6, x6, #0x20
        mov     w7, #1
        str     w7, [x6]
        movz    x2, #0xb000
        movk    x2, #0x1401, lsl #16
        movz    w3, #0x3909
        movk    w3, #0x002c, lsl #16
        str     w3, [x2, #0x200]
        str     w7, [x2, #0x60]
        str     wzr, [x2]
        str     w7, [x2]
        dsb     sy
        ret
