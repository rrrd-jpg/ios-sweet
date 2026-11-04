#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

#define WDT_MODE   0x10007000UL
#define MUTEX_BASE 0x14020000UL
#define DSI_BASE   0x1401b000UL

static void delay_ms(unsigned ms)
{
        for (volatile unsigned long i = 0; i < ms * 200000UL; i++)
                ;
}

/* Command-mode panel: push the buffer out (sequence proven by stub10). */
extern void kick_asm(void);
static void kick(void) { kick_asm(); }

static void hang(void)
{
        for (;;) {
                kick();
                delay_ms(200);
        }
}

void exc_handler(uint64_t slot)
{
        con_clear(BLUE);
        con_hex("PANIC", slot);
        con_hex("ESR", rsr("esr_el1"));
        con_hex("ELR", rsr("elr_el1"));
        con_hex("FAR", rsr("far_el1"));
        hang();
}

static void show(void)
{
        kick();
        delay_ms(100);
}

static void paint_c(uint32_t c, int rows)
{
        for (int y = 0; y < rows; y++) {
                volatile uint32_t *l = (volatile uint32_t *)FB_BASE + y * (FB_STRIDE / 4);
                for (int x = 0; x < FB_W; x++)
                        l[x] = c;
        }
        kick();
        delay_ms(1000);
}

void shim_main(uint64_t dtb, uint64_t el)
{
        wr32(WDT_MODE, 0x22000000);
        paint_c(0xffff00ff, 400);
        con_clear(BLUE);
        show();
        con_str("MT6795", "SHIM ALIVE");
        show();
        con_hex("ENTRY EL", el >> 2);
        con_hex("DTB", dtb);
        show();
        con_hex("MIDR", rsr("midr_el1"));
        con_hex("MPIDR", rsr("mpidr_el1"));
        con_hex("CNTFRQ", rsr("cntfrq_el0"));
        uint64_t m = rsr("id_aa64mmfr0_el1");
        con_hex("MMFR0", m);
        con_str("TGRAN16", ((m >> 20) & 0xf) ? "YES" : "NO (16K unsupported)");
        con_str("TGRAN4", ((m >> 28) & 0xf) == 0xf ? "NO" : "YES");
        con_hex("PFR0", rsr("id_aa64pfr0_el1"));
        con_hex("ISAR0", rsr("id_aa64isar0_el1"));
        con_hex("ISAR1", rsr("id_aa64isar1_el1"));
        con_hex("MMFR1", rsr("id_aa64mmfr1_el1"));
        show();
        con_str("NEXT", "READ DTB HDR");
        show();
        con_hex2("DTBHDR", "magic=", __builtin_bswap32(rd32(dtb)),
                 "size=", __builtin_bswap32(rd32(dtb + 4)));
        show();
        con_str("NEXT", "READ GICD");
        show();
        con_hex2("GICD", "typer=", rd32(0x10221004), "iidr=", rd32(0x10221008));
        show();
        con_str("NEXT", "READ GICC");
        show();
        con_hex2("GICC", "iidr=", rd32(0x1022200c), 0, 0);
        show();
        con_str("DONE", "ALL READS OK");
        hang();
}
