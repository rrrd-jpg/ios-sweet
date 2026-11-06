/*
 * Galaxy S10e (SM-G970F, Exynos 9820) bring-up probe, milestone 0.
 *
 * s-boot enters us like a kernel Image: MMU/caches off, DAIF masked, x0 = DTB.
 * Nothing is written to storage and the probe never resets: it halts at the end.
 * Pages advance with VOL UP (or on a timeout). All MMIO reads go through
 * safe_rd32(), which turns a synchronous abort into 0xdeadbeef.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" :: "r"((uint64_t)(v)))

#define GICD_BASE   0x10101000UL
#define GICC_BASE   0x10102000UL
#define UFS_HCI     0x13d60000UL
#define DECON_F     0x19030000UL
#define SYSMMU_DPU0 0x190a0000UL
#define DPU_LO      0x19000000UL
#define DPU_HI      0x19100000UL
#define GPA0_DAT    0x15850004UL    /* gpa0: vol up = pin 3, vol down = pin 4, bixby = pin 6 */
#define GPA2_DAT    0x15850044UL    /* gpa2: power = pin 4 */
#define DECON_SHADOW_REQ 0x60
#define DECON_TRIG       0x70
#define DARK        0xff202020U

#define PMU_BASE    0x15860000UL

static volatile int probe_armed, probe_fault;

static void __attribute__((noreturn)) halt(void)
{
        for (;;)
                __asm__ volatile("wfe");
}

static volatile uint64_t sec_midr[8], sec_mpidr[8];
static volatile uint32_t sec_flag[8];
volatile uint64_t sec_raw_x0[8];
volatile uint32_t sec_arrived[8];

/* stackless CPU_ON target: record x0, mpidr, midr, CurrentEL, then park */
volatile uint64_t s10e_cur[4];
volatile uint32_t s10e_cur_flag;
__asm__(
"	.text\n"
"	.balign 4\n"
"	.globl s10e_sec_park\n"
"s10e_sec_park:\n"
"	msr	daifset, #0xf\n"
"	adr	x6, s10e_cur\n"
"	str	x0, [x6]\n"
"	mrs	x2, mpidr_el1\n"
"	str	x2, [x6, #8]\n"
"	mrs	x2, midr_el1\n"
"	str	x2, [x6, #16]\n"
"	mrs	x2, currentel\n"
"	str	x2, [x6, #24]\n"
"	dsb	sy\n"
"	mov	w2, #0x5ec\n"
"	adr	x6, s10e_cur_flag\n"
"	str	w2, [x6]\n"
"	dsb	sy\n"
"1:	wfe\n"
"	b	1b\n"
);

/*
 * CPU_ON target. Samsung's firmware may not pass the context id in x0, so the
 * slot comes from MPIDR (aff0 for cores 0-5, 6 + aff0 for the aff1 = 1
 * cluster). Record arrival first, then use the normal entry with x0 = slot.
 */
__asm__(
"	.text\n"
"	.balign 4\n"
"	.globl s10e_sec_entry\n"
"s10e_sec_entry:\n"
"	msr	daifset, #0xf\n"
"	mrs	x2, mpidr_el1\n"
"	and	x3, x2, #0xff\n"
"	ubfx	x4, x2, #8, #8\n"
"	add	x5, x3, #6\n"
"	cmp	x4, #0\n"
"	csel	x1, x3, x5, eq\n"
"	cmp	x1, #8\n"
"	b.hs	1f\n"
"	adr	x6, sec_raw_x0\n"
"	str	x0, [x6, x1, lsl #3]\n"
"	adr	x7, sec_arrived\n"
"	mov	w8, #0x5ec\n"
"	str	w8, [x7, x1, lsl #2]\n"
"	dsb	sy\n"
"	mov	x0, x1\n"
"	b	secondary_entry\n"
"1:	wfe\n"
"	b	1b\n"
);

void secondary_main(uint64_t idx)
{
        if (idx < 8) {
                sec_midr[idx] = rsr("midr_el1");
                sec_mpidr[idx] = rsr("mpidr_el1");
                __asm__ volatile("dsb sy" ::: "memory");
                sec_flag[idx] = 0xa5a50000u | (uint32_t)idx;
        }
        halt();
}

static void flush(void);
static void delay_ms(unsigned ms);

static volatile uint32_t irq_armed, irq_cnt, irq_last_iar;

void exc_handler(uint64_t slot, uint64_t *regs)
{
        uint64_t esr = rsr("esr_el1");

        /* GICv2 IRQ at EL1 (current EL, SPx): ack, quiesce the timer, EOI */
        if (slot == 5 && irq_armed) {
                uint32_t iar = *(volatile uint32_t *)(GICC_BASE + 0x0c);
                uint32_t id = iar & 0x3ff;

                irq_last_iar = iar;
                irq_cnt++;
                wsr("cntv_ctl_el0", 0);
                wsr("cntp_ctl_el0", 0);
                if (id < 1020)
                        *(volatile uint32_t *)(GICC_BASE + 0x10) = iar;
                if (irq_cnt > 100)      /* storm guard: stay masked after eret */
                        wsr("spsr_el1", rsr("spsr_el1") | 0x80);
                return;
        }

        /* a faulting MMIO read inside safe_rd32(): skip the ldr and carry on */
        if (probe_armed && slot == 4 && (esr >> 26) == 0x25) {
                probe_fault = 1;
                wsr("elr_el1", rsr("elr_el1") + 4);
                return;
        }
        con_clear(RED);
        switch (slot & 3) {
        case 0: con_str("PANIC", "SYNC"); break;
        case 1: con_str("PANIC", "IRQ"); break;
        case 2: con_str("PANIC", "FIQ"); break;
        default: con_str("PANIC", "SERROR"); break;
        }
        con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
        con_hex("ESR", esr);
        con_hex("ELR", rsr("elr_el1"));
        con_hex("FAR", rsr("far_el1"));
        con_hex("SPSR", rsr("spsr_el1"));
        con_hex("X0", regs[0]);
        con_hex("X1", regs[1]);
        con_hex("X30", regs[29]);
        con_str("HALT", "hold POWER+VOLDOWN 7s");
        for (;;) {
                flush();
                delay_ms(500);
        }
}

static uint32_t safe_rd32(uintptr_t a)
{
        uint32_t v;

        probe_fault = 0;
        probe_armed = 1;
        __asm__ volatile("ldr %w0, [%1]\n\tdsb sy" : "=&r"(v) : "r"(a) : "memory");
        probe_armed = 0;
        return probe_fault ? 0xdeadbeefU : v;
}

static inline void wr32(uintptr_t a, uint32_t v)
{
        *(volatile uint32_t *)a = v;
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
        register uint64_t x0 __asm__("x0") = fn;
        register uint64_t x1 __asm__("x1") = a1;
        register uint64_t x2 __asm__("x2") = a2;
        register uint64_t x3 __asm__("x3") = a3;
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                           "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        return x0;
}

static uint64_t freq(void)
{
        uint64_t f = rsr("cntfrq_el0");
        return f ? f : 26000000;
}

static uint64_t now(void)
{
        __asm__ volatile("isb" ::: "memory");
        return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
        uint64_t end = now() + freq() / 1000 * ms;
        while (now() < end)
                ;
}

static int volup_pressed(void)
{
        return !(safe_rd32(GPA0_DAT) & (1u << 3));
}

static void flush(void)
{
        wr32(DECON_F + DECON_TRIG, safe_rd32(DECON_F + DECON_TRIG) | (1u << 8));
        wr32(DECON_F + DECON_SHADOW_REQ, 1u << 31);
}

static void wait_next(unsigned max_s)
{
        uint64_t lastf = 0;

        uint64_t end;

        flush();
        delay_ms(1500);
        end = now() + freq() * max_s;
        while (now() < end) {
                if (now() - lastf > freq() / 2) {
                        flush();
                        lastf = now();
                }
                if (!volup_pressed())
                        continue;
                delay_ms(40);
                if (!volup_pressed())
                        continue;
                end = now() + freq() * 2;       /* wait for release, at most 2 s */
                while (volup_pressed() && now() < end)
                        ;
                return;
        }
}

static void hexn(char *b, uint32_t v, int n)
{
        for (int i = n - 1; i >= 0; i--, v >>= 4)
                b[i] = "0123456789abcdef"[v & 0xf];
}

static void dump_pairs(char tag, uintptr_t base, uint32_t from, uint32_t to)
{
        char l[8];

        for (uint32_t o = from; o < to; o += 8) {
                l[0] = tag;
                hexn(l + 1, o, 4);
                l[5] = 0;
                con_hex2(l, "a=", safe_rd32(base + o), "b=", safe_rd32(base + o + 4));
        }
}

static void fill_rows(unsigned y0, unsigned y1, uint32_t c)
{
        for (unsigned y = y0; y < y1; y++)
                for (unsigned x = 0; x < FB_W; x++)
                        con_fb[y * FB_W + x] = c;
}

static uint32_t bar_color(unsigned i)
{
        switch (i) {
        case 0: return RED;
        case 1: return GREEN;
        case 2: return BLUE;
        case 3: return WHITE;
        case 4: return YELLOW;
        default: return 0xff000000U;
        }
}

static void mark(unsigned i)    /* scan progress: one green box per 64 KiB, bottom of the screen */
{
        for (unsigned y = FB_H - 60; y < FB_H - 20; y++)
                for (unsigned x = i * 64; x < i * 64 + 48; x++)
                        con_fb[y * FB_W + x] = GREEN;
}

static void page1(uint64_t dtb)
{
        uint64_t is = 0, ie = 0;
        int dtb_ok = dtb && __builtin_bswap32(safe_rd32(dtb)) == 0xd00dfeedU;

        con_clear(BLUE);
        con_str("S10E", "probe 1/5: cpu gic timer");
        con_skip();
        con_hex("EL", rsr("CurrentEL") >> 2);
        con_hex2("CPU", "mpidr=", rsr("mpidr_el1"), "midr=", rsr("midr_el1"));
        con_hex("SCTLR", rsr("sctlr_el1"));
        con_hex("PFR0", rsr("id_aa64pfr0_el1"));
        con_hex2("TIMER", "frq=", rsr("cntfrq_el0"), "lo=", now());
        con_hex2("DTB", "at=", dtb, "ok=", dtb_ok);
        if (dtb_ok) {
                fdt_chosen_u64(dtb, "linux,initrd-start", &is);
                fdt_chosen_u64(dtb, "linux,initrd-end", &ie);
        }
        con_hex2("INITRD", "s=", is, "e=", ie);
        con_hex2("GICD", "ctlr=", safe_rd32(GICD_BASE), "typer=", safe_rd32(GICD_BASE + 4));
        con_hex2("GICD", "iidr=", safe_rd32(GICD_BASE + 8), "icfgr1=", safe_rd32(GICD_BASE + 0xc04));
        con_hex2("GICC", "ctlr=", safe_rd32(GICC_BASE), "pmr=", safe_rd32(GICC_BASE + 4));
        con_hex2("GICC", "iidr=", safe_rd32(GICC_BASE + 0xfc), "bpr=", safe_rd32(GICC_BASE + 8));
        con_hex2("PSCI", "ver=", smc(0x84000000, 0, 0, 0), "rst=", smc(0x8400000a, 0x84000009, 0, 0));
        con_hex2("GPIO", "gpa0=", safe_rd32(GPA0_DAT), "gpa2=", safe_rd32(GPA2_DAT));
        con_skip();
        con_str("NEXT", "tap VOL UP (auto 15s)");
        wait_next(15);
}

static void page2(void)
{
        con_clear(BLUE);
        con_str("S10E", "probe 2/5: ufs decon");
        con_skip();
        con_hex2("UFS", "cap=", safe_rd32(UFS_HCI + 0x00), "ver=", safe_rd32(UFS_HCI + 0x08));
        con_hex2("UFS", "is=", safe_rd32(UFS_HCI + 0x20), "hcs=", safe_rd32(UFS_HCI + 0x30));
        con_hex2("UFS", "hce=", safe_rd32(UFS_HCI + 0x34), "utrlba=", safe_rd32(UFS_HCI + 0x50));
        con_hex2("UFS", "dbr=", safe_rd32(UFS_HCI + 0x58), "rsr=", safe_rd32(UFS_HCI + 0x60));
        con_skip();
        con_str("DECON_F", "0x19030000 + off");
        dump_pairs('D', DECON_F, 0x00, 0x80);
        wait_next(15);
}

static void page3(void)
{
        con_clear(BLUE);
        con_str("S10E", "probe 3/5: decon win, sysmmu");
        con_skip();
        dump_pairs('W', DECON_F, 0x1000, 0x1040);
        dump_pairs('X', DECON_F, 0x10a0, 0x10e0);
        con_str("SYSMMU", "dpu0 0x190a0000");
        dump_pairs('M', SYSMMU_DPU0, 0x0, 0x20);
        dump_pairs('N', SYSMMU_DPU0, 0x8000, 0x8020);
        wait_next(15);
}

static void page4(void)
{
        unsigned hits = 0, faults = 0, ok = 0;

        con_clear(BLUE);
        con_str("S10E", "probe 4/5: scan for fb addr");
        con_skip();
        for (uintptr_t a = DPU_LO; a < DPU_HI; a += 4) {
                if (((a - DPU_LO) & 0xffff) == 0)
                        mark((a - DPU_LO) >> 16);
                uint32_t v = safe_rd32(a);
                if (probe_fault) {
                        faults++;
                        continue;
                }
                ok++;
                if (hits < 22 && v >= 0xc0000000U && v < 0xe0000000U && !(v & 0xfff)) {
                        con_hex2("HIT", "off=", a - DPU_LO, "val=", v);
                        hits++;
                }
        }
        con_hex2("SCAN", "ok=", ok, "fault=", faults);
        wait_next(15);
}

static void page5(void)
{
        con_clear(BLUE);
        con_str("S10E", "probe 5/5: fb phases");
        con_str("RED", "fill only");
        con_str("GREEN", "fill + update req");
        con_str("BLUE", "fill + sw trigger");
        con_str("START", "in 8 s, 7 s each");
        flush();
        delay_ms(8000);
        fill_rows(0, FB_H, RED);
        flush();
        delay_ms(7000);
        fill_rows(0, FB_H, GREEN);
        wr32(DECON_F + DECON_SHADOW_REQ, 1u << 31);
        delay_ms(7000);
        fill_rows(0, FB_H, BLUE);
        wr32(DECON_F + DECON_TRIG, safe_rd32(DECON_F + DECON_TRIG) | (1u << 8));
        wr32(DECON_F + DECON_SHADOW_REQ, 1u << 31);
        delay_ms(7000);
        con_clear(DARK);
        con_str("S10E", "probe done");
        con_hex2("DECON", "glob=", safe_rd32(DECON_F), "trig=", safe_rd32(DECON_F + DECON_TRIG));
        con_str("HALT", "hold POWER+VOLDOWN 7s");
}

__attribute__((unused)) static void page6(void)
{
        static const uint32_t tgt[7] = { 0x001, 0x002, 0x003, 0x004, 0x005, 0x100, 0x101 };
        char l[5];
        uint64_t entry, end, lastf = 0;
        int go = 0;

        con_clear(BLUE);
        con_str("S10E", "probe 6/6: cpu_on, swreset");
        con_skip();
        __asm__ volatile("adr %0, s10e_sec_entry" : "=r"(entry));
        con_hex("ENTRY", entry);
        flush();
        l[0] = 'C'; l[1] = 'P'; l[2] = 'U'; l[4] = 0;
        for (unsigned i = 0; i < 7; i++) {
                uint32_t ret = (uint32_t)smc(0xc4000003, tgt[i], entry, i + 1);
                unsigned n = 0;

                while (!sec_flag[i + 1] && n++ < 200)
                        delay_ms(5);
                l[3] = '1' + i;
                con_hex2(l, "ret=", ret, "midr=", sec_flag[i + 1] ? (uint32_t)sec_midr[i + 1] : 0);
                flush();
        }
        for (unsigned i = 1; i < 8; i++) {
                if (!sec_arrived[i])
                        continue;
                con_hex2("ARRIVED", "cpu=", i, "x0=", (uint32_t)sec_raw_x0[i]);
                if (sec_flag[i])
                        con_hex2("MPIDR", "cpu=", i, "val=", (uint32_t)sec_mpidr[i]);
        }
        con_skip();
        con_str("NEXT", "VOL UP = test PMU SWRESET");
        flush();
        end = now() + freq() * 30;
        while (now() < end) {
                if (now() - lastf > freq() / 2) {
                        flush();
                        lastf = now();
                }
                if (volup_pressed()) {
                        delay_ms(40);
                        if (volup_pressed()) {
                                go = 1;
                                break;
                        }
                }
        }
        if (go) {
                con_str("RESET", "writing PMU SWRESET");
                flush();
                delay_ms(500);
                wr32(PMU_BASE + 0x404, 0);
                wr32(PMU_BASE + 0x3a00, 1u << 1);
                delay_ms(3000);
                con_str("RESET", "no effect after 3 s");
        }
        con_str("HALT", "hold POWER+VOLDOWN 7s");
        for (;;) {
                flush();
                delay_ms(500);
        }
}

static void page7(void)
{
        static const uint32_t tgt[] = {
                0x101, 0x100, 0x200, 0x300, 0x102, 0x103, 0x10000, 0x10100, 0x000
        };
        char l[8];
        uint64_t entry, end, lastf = 0;
        int go = 0;

        con_clear(BLUE);
        con_str("S10E", "probe 7: cpu_on sweep, swreset");
        con_skip();
        __asm__ volatile("adr %0, s10e_sec_park" : "=r"(entry));
        flush();
        for (unsigned i = 0; i < sizeof(tgt) / sizeof(tgt[0]); i++) {
                uint32_t ret;
                unsigned n = 0;

                s10e_cur_flag = 0;
                s10e_cur[0] = s10e_cur[1] = s10e_cur[2] = s10e_cur[3] = 0;
                l[0] = 'T';
                hexn(l + 1, tgt[i], 5);
                l[6] = 0;
                con_str(l, "calling");
                flush();
                ret = (uint32_t)smc(0xc4000003, tgt[i], entry, 0);
                con_hex2("", "back ret=", ret, "t=", (uint32_t)now());
                flush();
                while (!s10e_cur_flag && n++ < 100)
                        delay_ms(5);
                l[0] = 'T';
                hexn(l + 1, tgt[i], 5);
                l[6] = 0;
                con_hex2(l, "ret=", ret, "mp=", s10e_cur_flag ? (uint32_t)s10e_cur[1] : 0);
                if (s10e_cur_flag)
                        con_hex2("", "midr=", (uint32_t)s10e_cur[2], "x0=", (uint32_t)s10e_cur[0]);
                flush();
        }
        con_skip();
        con_str("NEXT", "VOL UP = test PMU SWRESET");
        flush();
        end = now() + freq() * 30;
        while (now() < end) {
                if (now() - lastf > freq() / 2) {
                        flush();
                        lastf = now();
                }
                if (volup_pressed()) {
                        delay_ms(40);
                        if (volup_pressed()) {
                                go = 1;
                                break;
                        }
                }
        }
        if (go) {
                con_str("RESET", "writing PMU SWRESET");
                flush();
                delay_ms(500);
                wr32(PMU_BASE + 0x404, 0);
                wr32(PMU_BASE + 0x3a00, 1u << 1);
                delay_ms(3000);
                con_str("RESET", "no effect after 3 s");
        }
        con_str("HALT", "hold POWER+VOLDOWN 7s");
        for (;;) {
                flush();
                delay_ms(500);
        }
}

static void __attribute__((noreturn)) reset_prompt(void)
{
        uint64_t end, lastf = 0;
        int go = 0;

        con_str("NEXT", "VOL UP = test PMU SWRESET");
        flush();
        end = now() + freq() * 30;
        while (now() < end) {
                if (now() - lastf > freq() / 2) {
                        flush();
                        lastf = now();
                }
                if (volup_pressed()) {
                        delay_ms(40);
                        if (volup_pressed()) {
                                go = 1;
                                break;
                        }
                }
        }
        if (go) {
                con_str("RESET", "writing PMU SWRESET");
                flush();
                delay_ms(500);
                wr32(PMU_BASE + 0x404, 0);
                wr32(PMU_BASE + 0x3a00, 1u << 1);
                delay_ms(3000);
                con_str("RESET", "no effect after 3 s");
        }
        con_str("HALT", "hold POWER+VOLDOWN 7s");
        for (;;) {
                flush();
                delay_ms(500);
        }
}

static void irq_test(const char *name, unsigned id, int virt)
{
        uint32_t ctl, pend, en, hppir;
        unsigned n = 0;

        irq_cnt = 0;
        irq_last_iar = 0;
        wr32(GICD_BASE + 0x000, 1);                             /* EnableGrp1 (NS view) */
        *(volatile uint8_t *)(GICD_BASE + 0x400 + id) = 0x80;   /* priority */
        wr32(GICD_BASE + 0x100, 1u << id);                      /* set-enable */
        wr32(GICC_BASE + 0x04, 0xf0);
        wr32(GICC_BASE + 0x00, 1);
        en = safe_rd32(GICD_BASE + 0x100);
        irq_armed = 1;
        __asm__ volatile("msr daifset, #2" ::: "memory");
        if (virt) {
                wsr("cntv_tval_el0", freq() / 1000);
                wsr("cntv_ctl_el0", 1);
        } else {
                wsr("cntp_tval_el0", freq() / 1000);
                wsr("cntp_ctl_el0", 1);
        }
        delay_ms(5);
        ctl = virt ? rsr("cntv_ctl_el0") : rsr("cntp_ctl_el0");
        pend = safe_rd32(GICD_BASE + 0x200);
        hppir = safe_rd32(GICC_BASE + 0x18);
        __asm__ volatile("msr daifclr, #2" ::: "memory");
        while (!irq_cnt && n++ < 50)
                delay_ms(1);
        __asm__ volatile("msr daifset, #2" ::: "memory");
        wsr("cntv_ctl_el0", 0);
        wsr("cntp_ctl_el0", 0);
        con_hex2(name, "ctl=", ctl, "pend=", pend);
        con_hex2("", "en=", en, "hppir=", hppir);
        con_hex2("", "irqs=", irq_cnt, "iar=", irq_last_iar);
        wr32(GICD_BASE + 0x180, 1u << id);
        irq_armed = 0;
        flush();
}

static void page8(void)
{
        con_clear(BLUE);
        con_str("S10E", "probe 8: gicv2 + timer irq");
        con_skip();
        con_hex2("GICD", "ctlr=", safe_rd32(GICD_BASE), "typer=", safe_rd32(GICD_BASE + 4));
        con_hex2("GICC", "ctlr=", safe_rd32(GICC_BASE), "pmr=", safe_rd32(GICC_BASE + 4));
        con_hex2("PPI", "igrp=", safe_rd32(GICD_BASE + 0x80), "pri27=", safe_rd32(GICD_BASE + 0x418));
        con_skip();
        flush();
        irq_test("VIRT27", 27, 1);
        irq_test("PHYS30", 30, 0);
        con_skip();
        reset_prompt();
}

void shim_main(uint64_t dtb)
{
        con_fb = (volatile uint32_t *)FB_BASE;
        /* raw bars first, before any text code can fail */
        for (unsigned y = 0; y < FB_H; y++)
                fill_rows(y, y + 1, bar_color(y / (FB_H / 6)));
        flush();
        delay_ms(4000);
        page1(dtb);
        page8();
        halt();
}
