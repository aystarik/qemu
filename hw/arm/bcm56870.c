/*
 * Broadcom BCM56870 (Trident3) Cortex-R5 firmware bring-up machine.
 *
 * This is NOT a complete SoC model. It exists to let the recovered
 * BCM56870_0_bfd Cortex-R5 firmware image run far enough to validate basic
 * behaviour: CPU/exception setup, TCM and heap zeroing, the logging/console
 * path, the interrupt controller, and the scheduler idle loop.
 *
 * Memory map (established by static analysis of the firmware image):
 *
 *   0x00000000 - 0x0001FFFF   ITCM      (code + rodata, image is 0x10430,
 *                                        BSS tail zeroed by Reset)
 *   0x00040000 - 0x0007FFFF   DTCM      (data + BSS + heap pools)
 *      incl. 0x0005C310 uncached window, 0x0005C920 dtcm heap pool
 *   0x01200000 - 0x0127EFFF   SYS heap pool
 *   0x02000000                SYS2 pool (disabled: base == end)
 *
 *   0x00080000                INTC register window
 *   0x00082000                system timer block
 *   0x03220000/0x03221000     16550-compatible UART (console)
 *   0x03241000                chip/device ID (must read 0xB870 in [15:4])
 *
 * The firmware's Reset code enables caches and does CP15 cache maintenance
 * over 0x40000-0x80000 and 0x1200000-0x127F000, so those regions must be
 * real RAM here or the maintenance loops misbehave.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/loader.h"
#include "hw/arm/boot.h"
#include "hw/core/boards.h"
#include "hw/char/serial-mm.h"
#include "net/net.h"
#include "net/eth.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu.h"

/* ------------------------------------------------------------------ */
/* Sizes                                                              */
/* ------------------------------------------------------------------ */
/*
 * Low RAM: ITCM + the unmapped hole + DTCM, modelled as one flat block.
 *
 * Architecturally these are separate (ITCM at 0x0, DTCM at 0x40000, with an
 * unbacked 0x20000..0x3FFFF gap), but the firmware image is a single flat
 * object spanning 0x0..0x4068F and its Reset code zero-fills across the gap.
 * Modelling them as one region keeps the load trivial and matches how the
 * image is actually delivered; the names below record the real boundaries.
 */
#define BCM_ITCM_BASE   0x00000000
#define BCM_ITCM_SIZE   0x00020000   /* 128 KiB: code + rodata + BSS tail */
#define BCM_DTCM_BASE   0x00040000
#define BCM_DTCM_SIZE   0x00040000   /* 256 KiB: data + BSS + heap pools */

#define BCM_LOWRAM_BASE 0x00000000
#define BCM_LOWRAM_SIZE 0x00080000   /* 0x0..0x7FFFF flat */

#define BCM_SYS_BASE    0x01200000
#define BCM_SYS_SIZE    0x00100000   /* 0x1200000..0x12FFFFF */

/* The firmware checks (*(u16 *)0x03241000 & 0xfff0) == 0xb870 */
#define BCM_CHIPID_BASE 0x03241000
#define BCM_CHIPID_VALUE 0x0000B870

#define BCM_UART0_BASE  0x03220000
#define BCM_UART1_BASE  0x03221000

#define BCM_CONSOLE_BASE 0x00084000

/*
 * Interrupt lines, as the firmware itself registers them:
 *   IRQ 8  scheduler tick, from the timer at 0x00082000
 *   IRQ 16 console UART at 0x00084000  (device type 0)
 *   IRQ 40 UART0 at 0x03220000         (device type 1)
 *   IRQ 41 UART1 at 0x03221000         (device type 2)
 * (IRQ 9 is also installed by board_init_thread for the 0x82020 alias.)
 */
#define BCM_UART0_IRQ   40
#define BCM_UART1_IRQ   41
#define BCM_CONSOLE_IRQ 16

#define BCM_INTC_BASE   0x00080000
#define BCM_INTC_SIZE   0x00001000

#define BCM_TIMER_BASE  0x00082000
#define BCM_TIMER_SIZE  0x00001000


/*
 * Enter the emulated world in the reset vector at 0x0. The firmware's Reset
 * is ARM (not Thumb) and is the first instruction of the image.
 */

/* ------------------------------------------------------------------ */
/* Simple 16550-ish UART                                              */
/*                                                                    */
/* The firmware only uses:                                            */
/*   +0x00  THR  (write)  / RBR (read)                                */
/*   +0x14  LSR  bit5 = THRE (tx holding register empty)              */
/* plus it writes +0x0c (LCR-ish) and +0x04/+0x08 during setup.       */
/* QEMU's generic serial-mm device implements the full 16550 register */
/* set at regshift=0, which matches this layout closely enough.       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Chip ID register                                                   */
/* ------------------------------------------------------------------ */
#define TYPE_BCM_CHIPID "bcm56870-chipid"
OBJECT_DECLARE_SIMPLE_TYPE(BCMChipIdState, BCM_CHIPID)

struct BCMChipIdState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t id;
};

static uint64_t chipid_read(void *opaque, hwaddr offset, unsigned size)
{
    BCMChipIdState *s = BCM_CHIPID(opaque);
    return s->id;
}

static void chipid_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    /* read-only */
}

static const MemoryRegionOps chipid_ops = {
    .read = chipid_read,
    .write = chipid_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void chipid_realize(DeviceState *dev, Error **errp)
{
    BCMChipIdState *s = BCM_CHIPID(dev);
    /*
     * Only a few bytes are the chip ID.  This used to claim a full 0x1000,
     * which swallowed the strap/pinmux registers at 0x03241784..0x032417A0 and
     * returned the chip-ID value 0xb870 for every read of them.  The firmware
     * does read-modify-write on those registers (FUN_000039b4 sets a bit, then
     * reads it back), so the bit never stuck and the host-message thread that
     * arms interrupt enables there could not make progress.
     */
    memory_region_init_io(&s->iomem, OBJECT(s), &chipid_ops, s,
                          TYPE_BCM_CHIPID, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void chipid_init(Object *obj)
{
    BCMChipIdState *s = BCM_CHIPID(obj);
    s->id = BCM_CHIPID_VALUE;
}

static void chipid_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = chipid_realize;
    dc->user_creatable = false;
}

static const TypeInfo chipid_info = {
    .name = TYPE_BCM_CHIPID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMChipIdState),
    .instance_init = chipid_init,
    .class_init = chipid_class_init,
};

/*
 * ------------------------------------------------------------------
 * Interrupt controller: control block at 0x00080000, and the two
 * enable/pending bitmap windows at 0x18320000 and 0x18330000.
 *
 * NOT a stock ARM GIC.  Reaching for QEMU's arm_gic model is the obvious move
 * and it does not fit, so the differences are worth recording:
 *
 *   * the current IRQ is read from INTC+0xF00; a GIC returns it from GICC_IAR
 *     at cpu+0x00C, and has no register at +0xF00;
 *   * it is acknowledged by WRITING ZERO to INTC+0xF00; a GIC uses EOIR at
 *     cpu+0x010;
 *   * the bitmaps the dispatcher scans are in a SEPARATE address window
 *     (0x18320090 / 0x183200B0) rather than inside the controller's own 0x1000
 *     block, where a GIC would put GICD_ISPENDR at dist+0x200.
 *
 * irq_dispatch() does the following, and only this much is modelled:
 *
 *   n    = read INTC+0xF00            (0 when no interrupt)
 *   g    = n & ~7                     (round down to a group of eight)
 *   scan bits g..g+7 of 0x18320x_B0   -> irq, else
 *   scan bits g..g+7 of 0x18320x_90   -> irq
 *   if irq: call handler table[irq]   (0x38-byte descriptors at 0x54FD8)
 *           write INTC+0x14 = 1 << (irq/8 & 31)
 *   write INTC+0xF00 = 0              (acknowledge)
 *
 * The bank selector is a flag byte at 0x54FBC (0 -> bank at 0x1832_xxxx).
 *
 * Devices assert individual lines through the "src" GPIO array.  Pending
 * state is kept here and mirrored into the pending register of the active
 * bank, so the dispatcher's scan sees exactly what real hardware would show.
 * The CPU IRQ line is asserted while any source is pending and released when
 * the acknowledge clears the last one.
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_INTC "bcm56870-intc"
OBJECT_DECLARE_SIMPLE_TYPE(BCMIntcState, BCM_INTC)

#define BCM_INTC_NUM_IRQ   256
#define BCM_INTC_NUM_REGS  (BCM_INTC_SIZE / 4)
#define BCM_INTC_BM_WORDS  8            /* 8 words x 32 bits = 256 sources */

/* Register offsets inside a bitmap window. */
#define BM_PENDING_OFF  0xb0
#define BM_ENABLE_OFF   0x90

struct BCMIntcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;      /* control block, 0x00080000 */
    MemoryRegion bm0;        /* bitmap window, 0x18320000 */
    MemoryRegion bm1;        /* bitmap window, 0x18330000 */

    qemu_irq irq_out;        /* to the CPU IRQ line */

    uint32_t regs[BCM_INTC_NUM_REGS];

    /*
     * Which device inputs are currently asserted, and the latched pending
     * state the dispatcher reads.  These are separate on purpose: a real
     * interrupt controller latches an incoming request and keeps it until the
     * CPU acknowledges, independently of how long the peripheral holds the
     * line.  Without that separation an acknowledge would be undone
     * immediately by the still-asserted input.
     */
    DECLARE_BITMAP(input_high, BCM_INTC_NUM_IRQ);

    /* What the dispatcher will read back. */
    uint32_t pending_bm[BCM_INTC_BM_WORDS];
    uint32_t enable_bm[BCM_INTC_BM_WORDS];
    uint32_t alt_pending_bm[BCM_INTC_BM_WORDS];
    uint32_t alt_enable_bm[BCM_INTC_BM_WORDS];

    /* Which bank the firmware selected (flag byte at 0x54FBC). */
    bool bank_b;
};

static int intc_lowest_pending(BCMIntcState *s)
{
    for (int w = 0; w < BCM_INTC_BM_WORDS; w++) {
        uint32_t v = s->pending_bm[w];
        if (v) {
            return w * 32 + ctz32(v);
        }
    }
    return -1;
}

static void intc_refresh(BCMIntcState *s)
{
    if (intc_lowest_pending(s) >= 0) {
        qemu_irq_raise(s->irq_out);
    } else {
        qemu_irq_lower(s->irq_out);
    }
}

static void intc_set_irq(void *opaque, int irq, int level)
{
    BCMIntcState *s = BCM_INTC(opaque);
    int w, b;

    if (irq < 0 || irq >= BCM_INTC_NUM_IRQ) {
        return;
    }
    w = irq / 32;
    b = 1u << (irq % 32);

    if (level) {
        s->pending_bm[w] |= b;
    } else {
        s->pending_bm[w] &= ~b;
    }
    intc_refresh(s);
}

/* ---------------- control block (0x00080000) ---------------- */

static uint64_t intc_read(void *opaque, hwaddr offset, unsigned size)
{
    BCMIntcState *s = BCM_INTC(opaque);
    unsigned idx = offset >> 2;

    if (offset == 0xf00) {
        int irq = intc_lowest_pending(s);
        return irq < 0 ? 0 : (uint32_t)irq;
    }
    if (idx >= BCM_INTC_NUM_REGS) {
        return 0;
    }
    return s->regs[idx];
}

static void intc_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    BCMIntcState *s = BCM_INTC(opaque);
    unsigned idx = offset >> 2;

    if (offset == 0xf00) {
        /* Acknowledge: retire the source the dispatcher just serviced. */
        int irq = intc_lowest_pending(s);
        if (irq >= 0) {
            s->pending_bm[irq / 32] &= ~(1u << (irq % 32));
        }
        intc_refresh(s);
        return;
    }
    if (idx >= BCM_INTC_NUM_REGS) {
        return;
    }
    s->regs[idx] = value;
}

static const MemoryRegionOps intc_ops = {
    .read = intc_read,
    .write = intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------- bitmap windows ---------------- */

static uint32_t *bm_enable(BCMIntcState *s, bool alt)
{
    return alt ? s->alt_enable_bm : s->enable_bm;
}

static uint32_t *bm_pending(BCMIntcState *s, bool alt)
{
    return alt ? s->alt_pending_bm : s->pending_bm;
}

static uint64_t bm_read(void *opaque, hwaddr offset, unsigned size, bool alt)
{
    BCMIntcState *s = BCM_INTC(opaque);

    /*
     * These are ARRAYS of 32-bit words, indexed by (irq / 32), not single
     * registers: the dispatcher does "ldr rX, [base, word, lsl #2]" with
     * word = (vector / 8) / 32, so it walks base+0, base+4, ... base+0x1C.
     * Treating the offsets as one word each makes every scan read past the
     * end and return 0, and the dispatcher then finds no interrupt at all.
     */
    if (offset >= BM_ENABLE_OFF && offset < BM_ENABLE_OFF + BCM_INTC_BM_WORDS * 4) {
        return bm_enable(s, alt)[(offset - BM_ENABLE_OFF) / 4];
    }
    if (offset >= BM_PENDING_OFF && offset < BM_PENDING_OFF + BCM_INTC_BM_WORDS * 4) {
        return bm_pending(s, alt)[(offset - BM_PENDING_OFF) / 4];
    }
    return 0;
}

static void bm_write(void *opaque, hwaddr offset, uint64_t value, unsigned size,
                     bool alt)
{
    BCMIntcState *s = BCM_INTC(opaque);

    /*
     * The firmware sets enable bits here (board_init_thread ORs 0x100 for
     * IRQ 8, the UART setup ORs in its own line).  Keep them: they are what
     * irq_dispatch falls back to when it finds no pending bit.
     */
    if (offset >= BM_ENABLE_OFF && offset < BM_ENABLE_OFF + BCM_INTC_BM_WORDS * 4) {
        bm_enable(s, alt)[(offset - BM_ENABLE_OFF) / 4] = value;
        return;
    }
    if (offset >= BM_PENDING_OFF && offset < BM_PENDING_OFF + BCM_INTC_BM_WORDS * 4) {
        bm_pending(s, alt)[(offset - BM_PENDING_OFF) / 4] = value;
        return;
    }
}

static uint64_t bm0_read(void *o, hwaddr off, unsigned sz)
{
    return bm_read(o, off, sz, false);
}
static void bm0_write(void *o, hwaddr off, uint64_t v, unsigned sz)
{
    bm_write(o, off, v, sz, false);
}
static uint64_t bm1_read(void *o, hwaddr off, unsigned sz)
{
    return bm_read(o, off, sz, true);
}
static void bm1_write(void *o, hwaddr off, uint64_t v, unsigned sz)
{
    bm_write(o, off, v, sz, true);
}

static const MemoryRegionOps bm0_ops = {
    .read = bm0_read, .write = bm0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps bm1_ops = {
    .read = bm1_read, .write = bm1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void intc_realize(DeviceState *dev, Error **errp)
{
    BCMIntcState *s = BCM_INTC(dev);

    /* mmio[0]: control block */
    memory_region_init_io(&s->iomem, OBJECT(s), &intc_ops, s,
                          TYPE_BCM_INTC ".ctrl", BCM_INTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /* mmio[1], mmio[2]: the two bitmap windows */
    memory_region_init_io(&s->bm0, OBJECT(s), &bm0_ops, s,
                          TYPE_BCM_INTC ".bm0", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->bm0);

    memory_region_init_io(&s->bm1, OBJECT(s), &bm1_ops, s,
                          TYPE_BCM_INTC ".bm1", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->bm1);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_out);
    qdev_init_gpio_in_named(dev, intc_set_irq, "src", BCM_INTC_NUM_IRQ);
}

static void intc_reset(DeviceState *dev)
{
    BCMIntcState *s = BCM_INTC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    bitmap_zero(s->input_high, BCM_INTC_NUM_IRQ);
    memset(s->pending_bm, 0, sizeof(s->pending_bm));
    memset(s->enable_bm, 0, sizeof(s->enable_bm));
    memset(s->alt_pending_bm, 0, sizeof(s->alt_pending_bm));
    memset(s->alt_enable_bm, 0, sizeof(s->alt_enable_bm));
    s->bank_b = false;
    s->regs[0x14 / 4] = 0xffffffff;
    intc_refresh(s);
}

static void intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = intc_realize;
    device_class_set_legacy_reset(dc, intc_reset);
    dc->user_creatable = false;
}

static const TypeInfo intc_info = {
    .name = TYPE_BCM_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMIntcState),
    .class_init = intc_class_init,
};

/*
 * ------------------------------------------------------------------
 * Timer block at 0x00082000 (aliased at 0x00082020).
 *
 * Register behaviour, read off FUN_00003700 (which programs it) and
 * mos_uptime_ticks() (which reads it), and confirmed against the running
 * guest:
 *
 *   +0x00 = 0x331E70A5   magic signature written by the firmware
 *   +0x04 = free-running counter.  The firmware seeds it with 1 during
 *                        bring-up, then mos_uptime_ticks() reads it (twice,
 *                        to detect a concurrent update) and scales it into
 *                        microseconds.
 *   +0x08 = mode byte (0x62 then 0xE2)
 *   +0x0C = control (written 1)
 *   +0x10 = period value
 *
 * The scheduler is driven by this block.  board_init_thread installs the tick
 * on IRQ 8: it ORs 0x100 (bit 8) into the enable bitmap and stores handler
 * 0x6B1 into descriptor 8 of the 0x38-byte handler table.  The handler stub at
 * 0x6B0 calls 0x198c, which is the scheduler tick.
 *
 * So the timer asserts line 8 periodically, and that is what makes the guest's
 * own scheduler advance.
 *
 * The period is deliberately coarse.  The guest spends a long time in device
 * initialisation threads, and a fast tick makes the scheduler reschedule
 * constantly so far less progress is made.  10 ms is a reasonable balance for
 * observing behaviour.
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_TIMER "bcm56870-timer"
OBJECT_DECLARE_SIMPLE_TYPE(BCMTimerState, BCM_TIMER)

/* Tick period in virtual nanoseconds. */
#define BCM_TIMER_TICK_NS  (10 * 1000 * 1000)

/*
 * The one-shot delay timer's unit conversion.
 *
 * FUN_00003754 programs the countdown as
 *      count = (remaining_ns * 1000) / scale        scale = *DAT_00003794 = 1166
 * so inverting, the countdown registers tick at scale/1000 ns each:
 *      ns = count * 1166 / 1000
 *
 * Verified against the firmware's own arm value: FUN_00002628(10000, 0) produces
 * count = 10000 * 1000 / 1166 = 8576, which is exactly what is observed.
 * Multiplying by 100000 instead -- an earlier error here -- made a 10 microsecond
 * delay look like 857 ms and a 1-second delay like 24 hours, so nothing ever
 * appeared to expire.
 */
#define BCM_DELAY_SCALE_NUM  1166
#define BCM_DELAY_SCALE_DEN  1000


/* The scheduler tick is published on IRQ 8, as the firmware itself sets up. */
#define BCM_TIMER_IRQ      8

struct BCMTimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    QEMUTimer tick;
    QEMUTimer delay;
    qemu_irq irq;                /* periodic scheduler tick  -> line 8 */
    qemu_irq delay_irq;          /* one-shot delay timer     -> line 9 */

    uint32_t regs[BCM_TIMER_SIZE / 4];
    uint64_t counter;             /* 64-bit: it is a nanosecond counter */
    uint32_t counter_lo;          /* what the low register reads back */
    int64_t last_ns;
    bool enabled;
};

static void timer_tick(void *opaque)
{
    BCMTimerState *s = BCM_TIMER(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /*
     * The counter is a free-running NANOSECOND counter, advanced in proportion
     * to elapsed virtual time rather than by one per tick.
     *
     * This matters more than it looks.  mos_uptime_ticks() reads this register
     * to decide "now", and the firmware expresses timeouts in the same units:
     * the mailbox wait arms timer 0x49 with 0x2540BE400 = 10,000,000,000, which
     * is 10 seconds only if the counter advances 1e9 per second.  An earlier
     * version of this device advanced it by +1 per 10 ms tick -- a factor of
     * 1e7 too slow -- so no scheduler timeout ever expired and every thread
     * that slept on one (the host-message thread among them) slept forever.
     */
    if (s->last_ns == 0) {
        s->last_ns = now;
    }
    if (now > s->last_ns) {
        s->counter += (uint64_t)(now - s->last_ns);
        s->last_ns = now;
    }
    s->counter_lo = (uint32_t)s->counter;

    if (s->enabled) {
        /*
         * Raise the line and leave it high.  The controller latches the
         * request into its pending bitmap and releases the input again as
         * part of the acknowledge, so this is an edge-style request from the
         * timer's point of view even though the line is held in between.
         */
        qemu_irq_raise(s->irq);
    }

    timer_mod(&s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + BCM_TIMER_TICK_NS);
}

/* One-shot delay expiry: assert line 9 so the firmware expires its delay list. */
static void timer_delay_expire(void *opaque)
{
    BCMTimerState *s = BCM_TIMER(opaque);
    s->regs[0x028 / 4] = 0;          /* the timer is no longer armed */
    qemu_irq_raise(s->delay_irq);
}

static uint64_t timer_read(void *opaque, hwaddr offset, unsigned size)
{
    BCMTimerState *s = BCM_TIMER(opaque);
    unsigned idx = offset >> 2;

    if (idx >= BCM_TIMER_SIZE / 4) {
        return 0;
    }

    /*
     * The live counter.  The firmware's own block pointer is 0x00082020 while
     * mos_uptime_ticks() reads 0x00082004: the same register seen as +0x04
     * from 0x82000, or +0x24 from 0x82020.
     */
    if (offset == 0x004 || offset == 0x024) {
        return s->counter_lo;
    }

    /*
     * +0x08 is the MODE register (the firmware writes 0x62 then 0xE2), not the
     * counter's high word.  mos_uptime_ticks() builds its 64-bit value from two
     * separate pointers it maintains, so the counter does not need a latched
     * high half here -- and returning one clobbers the mode byte.
     */
    return s->regs[idx];
}

static void timer_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    BCMTimerState *s = BCM_TIMER(opaque);
    unsigned idx = offset >> 2;

    if (idx >= BCM_TIMER_SIZE / 4) {
        return;
    }

    /*
     * +0x04 is the counter itself.  The firmware seeds it by writing 1 during
     * bring-up; accept that as a starting value, then keep counting.
     */
    if (offset == 0x004) {
        s->counter = value;
        s->counter_lo = value;
        s->last_ns = 0;
        return;
    }

    /*
     * Block B -- the ONE-SHOT delay timer at 0x00082020.
     *
     * FUN_00003754 arms it as:   [0x20] = countdown ; [0x28] = 0xA3 (arm)
     * and FUN_00001d94 then suspends the calling thread.  The matching wake is
     * IRQ 9, whose handler (0x3798 -> sched_timer_tick) expires the delay list.
     *
     * This is the gate that was blocking everything: the System msg thread's
     * first act is FUN_00002628(10000, 0), which arms this timer and suspends.
     * Only IRQ 8 was ever raised, so the thread suspended forever, never
     * evaluated its send window, never sent a request, and FUN_00006e88 -- the
     * "BFD ready" call -- was never reached.
     */
    if (offset == 0x028) {
        s->regs[offset / 4] = value;
        if (value == 0xa3) {
            /* Arm: schedule the one-shot to fire after `countdown` ticks. */
            uint32_t count = s->regs[0x020 / 4];
            int64_t ns = (int64_t)count * BCM_DELAY_SCALE_NUM / BCM_DELAY_SCALE_DEN;
            if (ns < 1000) {
                ns = 1000;
            }

            timer_mod(&s->delay, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
        }
        return;
    }

    s->regs[idx] = value;

    if (offset == 0x00c) {
        /*
         * Control: once set, the block delivers periodic interrupts.
         *
         * Enabling also publishes the period at +0x10.  The firmware never
         * writes +0x10 itself (FUN_00003700 programs +0x00, +0x04, +0x08 and
         * +0x0C only), but its IRQ-8 handler gates the entire scheduler timer
         * wheel behind a non-zero read of it:
         *
         *     if (_DAT_00082010 == 0) {
         *         return 0;            // wheel never advances
         *     }
         *
         * so leaving it at zero silently stops every timeout in the system.
         * Publish the period in the same nanosecond units as the counter that
         * mos_uptime_ticks() reads.
         */
        s->enabled = (value != 0);
        if (s->enabled) {
            s->regs[0x010 / 4] = (uint32_t)BCM_TIMER_TICK_NS;
        }
    }
}

static const MemoryRegionOps timer_ops = {
    .read = timer_read,
    .write = timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void timer_realize(DeviceState *dev, Error **errp)
{
    BCMTimerState *s = BCM_TIMER(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &timer_ops, s,
                          TYPE_BCM_TIMER, BCM_TIMER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->delay_irq);
    timer_init_ns(&s->tick, QEMU_CLOCK_VIRTUAL, timer_tick, s);
    timer_init_ns(&s->delay, QEMU_CLOCK_VIRTUAL, timer_delay_expire, s);
}

static void timer_reset(DeviceState *dev)
{
    BCMTimerState *s = BCM_TIMER(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->counter = 0;
    s->counter_lo = 0;
    s->last_ns = 0;
    s->enabled = false;
    timer_del(&s->tick);
    timer_del(&s->delay);
    timer_mod(&s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + BCM_TIMER_TICK_NS);
}

static void timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = timer_realize;
    device_class_set_legacy_reset(dc, timer_reset);
    dc->user_creatable = false;
}

static const TypeInfo timer_info = {
    .name = TYPE_BCM_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMTimerState),
    .class_init = timer_class_init,
};

/*
 * ------------------------------------------------------------------
 * Packet DMA engine at 0x03206400.
 *
 * Derived from pkt_dma_xfer() at 0x4938.  There are two identical descriptor
 * banks 0x80 apart; a flag byte at 0x54FBC selects which one is live:
 *
 *   bank A (flag == 0)              bank B (flag != 0)
 *     0x03206404  src_lo              0x03206484
 *     0x03206408  src_hi              0x03206488
 *     0x0320640C  dst_lo              0x0320648C
 *     0x03206410  dst_hi              0x03206490
 *     0x03206414  length in WORDS     0x03206494
 *     0x03206418  control (start)     0x03206498
 *     0x0320641C  status (polled)     0x0320649C
 *
 * Writing the control register starts a memory-to-memory copy of `length`
 * 32-bit words; the status register then reads back non-zero.  bit 1 of the
 * status means failure, and the caller turns that into a -1 return.
 *
 * Direction: register 0 (+0x00) is the SOURCE and register 1 (+0x08) is the
 * DESTINATION.  bfd_msg_thread calls this as
 *     pkt_dma_xfer(host_addr, 0, local_buf, 0, len, 2)
 * and then reads the bytes out of local_buf, so the copy runs
 * host_addr -> local_buf.
 *
 * This has to be a real transfer rather than an ignored register: the caller
 * POLLS the status until (status & 3) != 0, so a RAZ/WI model would spin
 * forever.  The addresses are DMA addresses, translated back to CPU ones with
 * the same mapping the firmware applies on the way out (see dma_to_cpu()).
 *
 * That mapping is the reason the host-message path works: the host buffers sit
 * at 0x1200000, which is identity-mapped, so the copy is a plain memcpy of
 * guest RAM.
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_PKTDMA "bcm56870-pktdma"
OBJECT_DECLARE_SIMPLE_TYPE(BCMPktDmaState, BCM_PKTDMA)

/* Descriptor bank field offsets, relative to each bank's base. */
#define DMA_SRC_LO   0x00
#define DMA_SRC_HI   0x04
#define DMA_DST_LO   0x08
#define DMA_DST_HI   0x0c
#define DMA_LEN      0x10
#define DMA_CTRL     0x14
#define DMA_STATUS   0x18

/*
 * Bank bases.  The firmware's register pointers are 0x03206404 and
 * 0x03206484 (not ...00/...80), so the field offsets below are relative to
 * those, giving:  +0x00 src_lo  +0x04 src_hi  +0x08 dst_lo  +0x0C dst_hi
 *                 +0x10 length   +0x14 control  +0x18 status
 */
#define DMA_BANK_A   0x04
#define DMA_BANK_B   0x84

/* Which bank the flag byte at 0x54FBC selects. */
#define DMA_BANK_FLAG_ADDR 0x00054FBC

/* Address translation window (FUN_00000838). */
#define DMA_WIN_BASE   0x01100000
#define DMA_LOW_LIMIT  0x00040000      /* below this: offset 0        */
#define DMA_MID_LIMIT  0x00100000      /* above this: identity        */
#define DMA_MID_DELTA  0x00020000      /* 0x40000..0xFFFFF: -0x20000  */

struct BCMPktDmaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x100 / 4];
    AddressSpace *as;
};

/*
 * Convert a DMA address back into a CPU address.  The firmware maps CPU
 * addresses INTO the DMA window on the way out; we invert it to find the
 * buffer the descriptor refers to.
 */
static uint32_t dma_to_cpu(uint32_t dma)
{
    if (dma >= DMA_WIN_BASE && dma < DMA_WIN_BASE + DMA_LOW_LIMIT) {
        return dma - DMA_WIN_BASE;
    }
    if (dma >= DMA_WIN_BASE + DMA_LOW_LIMIT &&
        dma < DMA_WIN_BASE + DMA_MID_LIMIT) {
        return dma - DMA_WIN_BASE + DMA_MID_DELTA;
    }
    return dma;                 /* identity: host buffers at 0x1200000 */
}

static void dma_do_transfer(BCMPktDmaState *s, hwaddr bank)
{
    uint32_t src = s->regs[(bank + DMA_SRC_LO) / 4];
    uint32_t dst = s->regs[(bank + DMA_DST_LO) / 4];
    uint32_t len = s->regs[(bank + DMA_LEN) / 4];
    uint32_t cpu_src = dma_to_cpu(src);
    uint32_t cpu_dst = dma_to_cpu(dst);
    uint32_t bytes = len * 4;
    g_autofree uint8_t *buf = NULL;

    if (len == 0 || bytes > 64 * 1024 * 1024) {
        s->regs[(bank + DMA_STATUS) / 4] = 2;   /* error */
        return;
    }

    buf = g_malloc(bytes);
    address_space_read(s->as, cpu_src, MEMTXATTRS_UNSPECIFIED, buf, bytes);
    address_space_write(s->as, cpu_dst, MEMTXATTRS_UNSPECIFIED, buf, bytes);

    s->regs[(bank + DMA_STATUS) / 4] = 1;       /* done, no error */
}

static uint64_t pktdma_read(void *opaque, hwaddr offset, unsigned size)
{
    BCMPktDmaState *s = BCM_PKTDMA(opaque);

    if (offset + 4 > sizeof(s->regs)) {
        return 0;
    }
    return s->regs[offset / 4];
}

static void pktdma_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    BCMPktDmaState *s = BCM_PKTDMA(opaque);

    if (offset + 4 > sizeof(s->regs)) {
        return;
    }
    s->regs[offset / 4] = value;

    /* Writing control kicks the transfer. */
    if (offset == DMA_BANK_A + DMA_CTRL ||
        offset == DMA_BANK_B + DMA_CTRL) {
        dma_do_transfer(s, offset - DMA_CTRL);
    }
}

static const MemoryRegionOps pktdma_ops = {
    .read = pktdma_read,
    .write = pktdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void pktdma_realize(DeviceState *dev, Error **errp)
{
    BCMPktDmaState *s = BCM_PKTDMA(dev);

    s->as = &address_space_memory;
    memory_region_init_io(&s->iomem, OBJECT(s), &pktdma_ops, s,
                          TYPE_BCM_PKTDMA, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void pktdma_reset(DeviceState *dev)
{
    BCMPktDmaState *s = BCM_PKTDMA(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void pktdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = pktdma_realize;
    device_class_set_legacy_reset(dc, pktdma_reset);
    dc->user_creatable = false;
}

static const TypeInfo pktdma_info = {
    .name = TYPE_BCM_PKTDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMPktDmaState),
    .class_init = pktdma_class_init,
};

/*
 * ------------------------------------------------------------------
 * Ethernet front-end for the packet DMA engine.
 *
 * The firmware does not talk to a MAC register block directly.  Instead it
 * parks receive buffers in a descriptor ring and polls a "packet available"
 * bitmap; for transmit it builds a descriptor and rings a doorbell.  So this
 * device stands in for the MAC/DMA boundary:
 *
 *   RX   a frame arrives from the netdev.  We walk the channel's descriptor
 *        ring, find a free slot, write the frame into its buffer, set the
 *        length, and set the channel's bit in the ready bitmap (0x54FAC for
 *        channel 0).  The firmware's poll (FUN_0000b58c) then picks it up and
 *        hands it to its own ethernet parser.
 *
 *   TX   the firmware enqueues a descriptor and writes the doorbell.  We read
 *        the buffer and hand it to the netdev.
 *
 * Ring geometry, verified against a running guest (FUN_0000b740):
 *   channel block base 0x53328, stride 0xe40
 *     +0x000  channel id
 *     +0x004  descriptor ring base -- observed 0, see the note below
 *     +0x00c  head/count
 *   descriptor ring at 0x5B2E8, 64 entries of 16 bytes:
 *     +0x00  DMA address of the buffer
 *     +0x04  0
 *     +0x08  low 16 bits = buffer length, high bits = control
 *     +0x0c  owner/status
 *
 * NOTE ON THE RING BASE: the channel's +0x004 field reads back as 0 even
 * though the ring lives at 0x5B2E8, so the base is NOT taken from there.  The
 * firmware passes the ring address to FUN_00004b44 itself, and the ring the
 * descriptors actually land in is the one FUN_0000b740 builds.  We therefore
 * locate descriptors by following the DMA address held in the ready
 * bookkeeping rather than trusting +0x004.  This is the least certain part of
 * the model and is called out in the README.
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_ETH "bcm56870-eth"
OBJECT_DECLARE_SIMPLE_TYPE(BCMEthState, BCM_ETH)

/*
 * Channel state, stride 0x18, base DAT_00004cb0 (0x5B03C).  The firmware uses
 * channel index 0xFF for the BFD receive path, so the block is at
 *     0x5B03C + 0xFF * 0x18 = 0x5C824
 * and the fields, read off FUN_00004b44 (enqueue) and FUN_00004c48 (dequeue):
 *
 *   +0x04  descriptor ring base            (observed 0x0005B2E8)
 *   +0x08  ring size in entries            (observed 0x80 = 128)
 *   +0x0C  enqueue limit                   (observed 0x40 = 64 buffers given)
 *   +0x10  producer, advanced by enqueue
 *   +0x14  consumer, advanced by dequeue
 *
 * A descriptor is 16 bytes:
 *   +0x00  DMA address of the receive buffer
 *   +0x04  0
 *   +0x08  low 16 bits = frame length, high bits = control
 *   +0x0C  owner/status; BIT 31 SET means "the DMA engine has filled this"
 *
 * FUN_00004c48 dequeues by reading the descriptor at the consumer index and
 * testing bit 31 of +0x0C.  So handing a received frame to the firmware is:
 *   write the bytes into the descriptor's buffer, store the length, then set
 *   bit 31 of +0x0C.  The firmware's own driver thread (FUN_0000b5b0) does the
 *   dequeue and passes the descriptor to its ethernet parser.
 *
 * NOTE ON AN EARLIER MISTAKE: an initial version of this device raised bit 0 of
 * 0x54FAC as a "packet available" flag.  That word is an initialisation flag
 * which reads 1 permanently, not a receive interrupt, so injected frames were
 * never collected.  The owner bit above is the real handoff.
 */
#define ETH_CHAN_IDX         0xFF
#define ETH_CHAN_BASE        0x0005B03C
#define ETH_CHAN_STRIDE      0x18
#define ETH_CHAN_STATE       (ETH_CHAN_BASE + ETH_CHAN_IDX * ETH_CHAN_STRIDE)

#define ETH_CH_DESC_RING     0x04
#define ETH_CH_RING_SIZE     0x08
#define ETH_CH_PRODUCER      0x10
#define ETH_CH_CONSUMER      0x14

#define ETH_DESC_SIZE        16
#define ETH_DESC_DMA_ADDR    0x00
#define ETH_DESC_LEN         0x08
#define ETH_DESC_STATUS      0x0c
#define ETH_DESC_OWNER       0x80000000u   /* set by the engine when filled */

#define ETH_MAX_FRAME        0x120

struct BCMEthState {
    SysBusDevice parent_obj;
    NICState *nic;
    NICConf conf;
    AddressSpace *as;
};

static uint32_t eth_dma_to_cpu(uint32_t dma)
{
    if (dma >= 0x01100000 && dma < 0x01140000) {
        return dma - 0x01100000;
    }
    if (dma >= 0x01140000 && dma < 0x01200000) {
        return dma - 0x010E0000;
    }
    return dma;              /* identity, e.g. the host buffers at 0x1200000 */
}

static uint32_t eth_rd32(BCMEthState *s, uint32_t addr)
{
    uint8_t b[4];
    address_space_read(s->as, addr, MEMTXATTRS_UNSPECIFIED, b, 4);
    return ldl_le_p(b);
}

static void eth_wr32(BCMEthState *s, uint32_t addr, uint32_t v)
{
    uint8_t b[4];
    stl_le_p(b, v);
    address_space_write(s->as, addr, MEMTXATTRS_UNSPECIFIED, b, 4);
}

/* Deliver one frame into the firmware's receive ring. */
static void eth_rx_frame(BCMEthState *s, const uint8_t *buf, size_t len)
{
    uint32_t ring, size, cons, desc, dma_buf, ctl;

    if (len > ETH_MAX_FRAME) {
        return;
    }

    ring = eth_rd32(s, ETH_CHAN_STATE + ETH_CH_DESC_RING);
    size = eth_rd32(s, ETH_CHAN_STATE + ETH_CH_RING_SIZE);
    cons = eth_rd32(s, ETH_CHAN_STATE + ETH_CH_CONSUMER);

    /* The ring is only set up once the firmware has started its RX driver. */
    if (ring == 0 || size == 0) {
        return;
    }
    cons %= size;

    desc = ring + cons * ETH_DESC_SIZE;
    dma_buf = eth_rd32(s, desc + ETH_DESC_DMA_ADDR);
    if (dma_buf == 0) {
        return;
    }

    address_space_write(s->as, eth_dma_to_cpu(dma_buf),
                        MEMTXATTRS_UNSPECIFIED, buf, len);

    ctl = eth_rd32(s, desc + ETH_DESC_LEN);
    ctl = (ctl & 0xffff0000u) | (uint16_t)len;
    eth_wr32(s, desc + ETH_DESC_LEN, ctl);

    /* Hand ownership to the firmware: bit 31 says "filled by the engine". */
    eth_wr32(s, desc + ETH_DESC_STATUS, ETH_DESC_OWNER);

    /*
     * Do NOT advance the producer: that side belongs to the firmware, which
     * advances it when it queues empty buffers.  The driver's dequeue moves
     * the consumer forward itself.
     */
}

static bool eth_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t eth_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    BCMEthState *s = qemu_get_nic_opaque(nc);

    eth_rx_frame(s, buf, size);
    return size;
}

static void eth_cleanup(NetClientState *nc)
{
}

static const NetClientInfo eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = eth_can_receive,
    .receive = eth_receive,
    .cleanup = eth_cleanup,
};

static void eth_realize(DeviceState *dev, Error **errp)
{
    BCMEthState *s = BCM_ETH(dev);

    s->as = &address_space_memory;


    if (!qemu_configure_nic_device(dev, true, NULL)) {
        /*
         * Running without -netdev is perfectly fine: the rest of the machine
         * (console, scheduler, timers) still works, there is simply no
         * ethernet.  Do not make that a hard error.
         */
        return;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

/*
 * QEMU 11.x terminates property arrays with the closing brace; there is no
 * DEFINE_PROP_END_OF_LIST() sentinel.
 */
static const Property eth_props[] = {
    DEFINE_NIC_PROPERTIES(BCMEthState, conf),
};

static void eth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = eth_realize;
    device_class_set_props(dc, eth_props);
    /*
     * Left user-creatable so it can be attached explicitly with
     * -device bcm56870-eth,netdev=... ; the machine also creates one
     * automatically when a netdev is configured.
     */
}

static const TypeInfo eth_info = {
    .name = TYPE_BCM_ETH,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BCMEthState),
    .class_init = eth_class_init,
};

/*
 * ------------------------------------------------------------------
 * Strap / pinmux scratch registers at 0x03241784.
 *
 * The firmware treats these as plain read-modify-write bits: FUN_000039b4
 * ORs bit 0 and FUN_000039d8 clears bits, then re-reads.  They therefore need
 * backing storage.  If they are left RAZ/WI the read-back is always 0, the bit
 * the firmware just set vanishes, and the host-message thread that arms its
 * interrupt enables here never makes progress.
 *
 * No behaviour is inferred from the values; they are only retained.
 * ------------------------------------------------------------------
 */
static uint64_t strap_read(void *opaque, hwaddr offset, unsigned size)
{
    static const uint8_t zero[4];
    const uint8_t *base = opaque ? (const uint8_t *)opaque : zero;

    if (offset + size > 0x7c) {
        return 0;
    }
    switch (size) {
    case 1: return base[offset];
    case 2: return lduw_le_p(base + offset);
    default: return ldl_le_p(base + offset);
    }
}

static void strap_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    uint8_t *base = opaque;

    if (!base || offset + size > 0x7c) {
        return;
    }
    switch (size) {
    case 1: base[offset] = value; break;
    case 2: stw_le_p(base + offset, value); break;
    default: stl_le_p(base + offset, value); break;
    }
}

static const MemoryRegionOps strap_ops = {
    .read = strap_read,
    .write = strap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * ------------------------------------------------------------------
 * CMICx responder.
 *
 * CMICx is the firmware's control fabric: every off-chip operation is a request
 * descriptor whose reply is routed back to the requesting thread.  The host
 * side of that fabric is what we have to stand in for, because BFD start-up is
 * triggered by CMICx messages and nothing else will start it.
 *
 * THE MAILBOX HANDSHAKE, read off FUN_00003cb0 (the "Host msg" thread):
 *
 *     host = 0x59734 + hostidx * 0x634          (four hosts)
 *     *(host+0x0C) = pointer the firmware READS  (host -> firmware)
 *     *(host+0x10) = pointer the firmware WRITES (firmware -> host)
 *     *(host+0x08) = the firmware's local state
 *
 *     loop {
 *         firmware_state = 1; publish; while ((READ  & 3) != 1) sleep();
 *         firmware_state = 2; publish; while ((READ  & 3) != 2) sleep();
 *         firmware_state = 3; publish; while ((READ  & 3) != 3) sleep();
 *         process the message; repeat
 *     }
 *
 * So the host must walk the input word through 1, 2 and 3 once per message.
 * The firmware publishes its own progress to the output word in the same order,
 * which gives a simple and race-free rule: MIRROR the phase.
 *
 * PACING.  The wait loop is NOT interrupt-driven.  It arms scheduler timer
 * 0x49 with a timeout and sleeps:
 *
 *     *puVar6 |= 0x200;                        enable a bit in the bitmap
 *     *(*DAT_00003f64 + 0x10) = 0x200;         write it to the controller
 *     FUN_00001a10(0x49, 0x2540BE400, 0);      arm timer 0x49 for 10 s
 *
 * and DAT_00003f6c/f70 is 0x2540BE400 = 10,000,000,000 ns -- a ten-second
 * timeout.  Timer slot 0x49 lives at 0x54FD8 + 0x49*0x38 = 0x55FD0 in BSS; it
 * is a SCHEDULER timer, not an interrupt line, and nothing in the firmware ever
 * raises line 9 for this.  (An earlier version of this device asserted IRQ 9
 * as a "doorbell", reading 0x200 as an IRQ number.  That was wrong: 0x200 is an
 * interrupt-ENABLE bit, and the wait is purely timeout-paced.)
 *
 * So the firmware re-samples the phase only when its 10-second timer expires.
 * The responder must therefore MIRROR the phase and then WAIT -- a full
 * three-phase exchange takes tens of seconds of guest time, and that is the
 * firmware's own pacing, not a stall.
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_CMICX "bcm56870-cmicx"
OBJECT_DECLARE_SIMPLE_TYPE(BCMCMICXState, BCM_CMICX)

#define CMICX_HOST_BASE    0x00059734
#define CMICX_HOST_STRIDE  0x634
#define CMICX_HOST_OFF_IN  0x0c
#define CMICX_HOST_OFF_OUT 0x10
#define CMICX_HOST_OFF_ST  0x08

/* Poll period in virtual nanoseconds. */
#define CMICX_POLL_NS      (1000 * 1000)

/*
 * How many polls to keep a round open before closing it.  The System msg thread
 * sleeps in 10000-unit delays between checks of the send window, so the window
 * must stay open for noticeably longer than one poll.
 */
#define CMICX_ROUND_HOLD_POLLS  3000

struct BCMCMICXState {
    SysBusDevice parent_obj;
    QEMUTimer poll;
    qemu_irq irq;                /* the host-message wake line (73) */
    AddressSpace *as;
    bool attached[4];
    bool sent;
    bool answered;
    uint32_t last_slot;
    int stage;
    int round_hold;
    uint32_t last_phase[4];
};

static uint32_t cmicx_rd32(AddressSpace *as, uint32_t addr)
{
    uint8_t b[4];
    address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED, b, 4);
    return ldl_le_p(b);
}

static void cmicx_wr32(AddressSpace *as, uint32_t addr, uint32_t v)
{
    uint8_t b[4];
    stl_le_p(b, v);
    address_space_write(as, addr, MEMTXATTRS_UNSPECIFIED, b, 4);
}

/*
 * Host side of the handshake.
 *
 * The firmware publishes its phase to OUT and then waits for the SAME value in
 * IN before advancing to the next phase:
 *
 *     publish 1 -> wait IN==1 -> publish 2 -> wait IN==2 -> publish 3 -> wait IN==3
 *
 * so the host simply echoes the firmware's published phase back into IN.  We
 * keep the last value we granted per host so we do not rewrite the word on
 * every poll (the firmware samples it between ten-second sleeps, and rewriting
 * constantly made the exchange look stalled in earlier testing).
 */
/*
 * Build and enqueue one CMICx message, so the firmware has work to do.
 *
 * The firmware polls its receive queue and dispatches on the message opcode.
 * The opcode that starts BFD is 2, and it additionally requires
 * revsh(payload[0x36]) == 1 (dispatch at 0x43c4 in FUN_00004278):
 *
 *     opcode == 2 && revsh(msg[0x36]) == 1
 *         -> bl FUN_00006e88        creates "BfdMsgThread", logs "BFD ready"
 *
 * The node is a dlist entry pushed onto the queue that FUN_00003fc0 pops from:
 *     host + param_2*8 + 0x2bc     with host 0, param_2 = 1  ->  0x599f8
 * and the queue head it parks on when empty is host + param_2*8 + 0x50c.
 *
 * Node layout (a mos_dlist_node):
 *     +0x00 next        +0x04 prev
 *     +0x08 word A      +0x0c word B     (the message handles)
 *     +0x1c status
 */
/*
 * How the firmware actually exchanges messages (from FUN_00004278/
 * FUN_00003fc0):
 *
 *   FUN_00004278(host, chan, param_3, ...)
 *       node = FUN_00003fc0(host, chan, ...)   pops from host+chan*8+0x2bc
 *       if (node != param_3) log_fatal("Msg receive got a different msg")
 *
 * so the node that comes back off the queue MUST be the caller's own buffer.
 * The firmware posts its request buffer and waits for the host to fill that
 * same buffer and hand the identical pointer back -- the reply is matched by
 * pointer identity, not by content.  Inventing our own node would make the
 * firmware's comparison fail.
 *
 * The caller then reads the reply out of its stack buffer param_3, which is
 * copied from the posted node.  The dispatcher indexes it as:
 *      opcode  at +0x11   (sp+0x35 with the buffer at sp+0x24)
 *      payload at +0x12   (sp+0x36, the value revsh() is applied to)
 *      word    at +0x14   (sp+0x38)
 *
 * So the responder's job is: watch the queues the firmware parks requests on,
 * and when one appears, fill its buffer in place and mark it done.  It must NOT
 * fabricate a node.
 */

static uint32_t cmicx_rd32(AddressSpace *as, uint32_t addr);
static void cmicx_wr32(AddressSpace *as, uint32_t addr, uint32_t v);

/*
 * The firmware posts its request buffer on host+chan*8+0x3e4
 * (FUN_00004040 pushes it there when the host state is 3) and then waits in
 * FUN_00003fc0, which pops the reply from host+chan*8+0x2bc.  So the host's job
 * is:
 *
 *     take the posted buffer off  +0x3e4
 *     fill in the reply fields
 *     push the SAME buffer onto     +0x2bc
 *
 * Keeping the same pointer matters: FUN_00004278 compares the buffer it gets
 * back against the caller's own buffer and log_fatal()s if they differ
 * ("Msg receive got a different msg").  Only the content may change.
 */
/*
 * Fill a reply into the firmware's own posted buffer.
 *
 * The dispatcher in FUN_000042e8 (the "System msg" thread) reads the buffer it
 * posted as its stack frame at sp+0x24, so the field offsets are:
 *
 *      +0x11   opcode        (sp+0x35)
 *      +0x12   halfword      (sp+0x36) -- passed through revsh()
 *      +0x14   word          (sp+0x38) -- passed through bswap32()
 *      +0x1c   status        (sp+0x40), tested == 1 for success
 *
 * The BFD-start condition is  status == 1 && opcode == 2 &&
 * revsh(halfword) == 1.  revsh byte-reverses the 16-bit value, so the RAW
 * halfword must be 0x0100 for revsh to yield 1.  Writing 0x0001 there -- which
 * an earlier version did -- makes revsh return 0 and the branch is never taken.
 *
 * Note the status is 1 here, not 0x81: 0x80/0x81 belong on REQUEST descriptors
 * (FUN_0000408c waits while (status - 0x80) < 2), whereas this is the value the
 * receiver compares against 1.
 */
static void cmicx_fill_reply(BCMCMICXState *s, uint32_t node, uint8_t opcode,
                             uint16_t raw_halfword, uint32_t word)
{
    uint8_t b[4];

    /* +0x10: byte 1 = opcode, bytes 2..3 = the raw halfword (little-endian) */
    b[0] = 0;
    b[1] = opcode;
    b[2] = raw_halfword & 0xff;
    b[3] = (raw_halfword >> 8) & 0xff;
    address_space_write(s->as, node + 0x10, MEMTXATTRS_UNSPECIFIED, b, 4);

    cmicx_wr32(s->as, node + 0x14, word);

    /* status: FUN_000042e8 requires exactly 1 */
    cmicx_wr32(s->as, node + 0x1c, 1);
}

/* Pop one node from a dlist, or 0 when empty. */
static uint32_t cmicx_dlist_pop(BCMCMICXState *s, uint32_t list)
{
    uint32_t node = cmicx_rd32(s->as, list);

    if (node == list || node < 0x1000 || node > 0x800000) {
        return 0;
    }
    {
        uint32_t next = cmicx_rd32(s->as, node + 0x00);
        if (next < 0x1000) {
            return 0;
        }
        cmicx_wr32(s->as, list, next);
        cmicx_wr32(s->as, next + 0x04, list);
        cmicx_wr32(s->as, node + 0x00, 0);
        cmicx_wr32(s->as, node + 0x04, 0);
    }
    return node;
}

/* Push a node onto the tail of a dlist. */
static void cmicx_dlist_push(BCMCMICXState *s, uint32_t list, uint32_t node)
{
    uint32_t tail = cmicx_rd32(s->as, list + 0x04);

    if (tail < 0x1000) {
        tail = list;
    }
    cmicx_wr32(s->as, node + 0x00, list);
    cmicx_wr32(s->as, node + 0x04, tail);
    cmicx_wr32(s->as, tail + 0x00, node);
    cmicx_wr32(s->as, list + 0x04, node);
}

static void cmicx_poll(void *opaque)
{
    BCMCMICXState *s = BCM_CMICX(opaque);
    bool deliver = false;

    /*
     * 1. Drive the mailbox handshake.
     *
     * The firmware publishes a phase to the output word and waits for the same
     * value in the input word, so we grant whatever it has published.
     */
    for (int h = 0; h < 4; h++) {
        uint32_t host = CMICX_HOST_BASE + h * CMICX_HOST_STRIDE;
        uint32_t in_ptr, out_ptr, in_word, out_word, fw, granted;

        in_ptr  = cmicx_rd32(s->as, host + CMICX_HOST_OFF_IN);
        out_ptr = cmicx_rd32(s->as, host + CMICX_HOST_OFF_OUT);

        /* Published only once the firmware is up, and they live in guest RAM,
         * so bound-check before dereferencing. */
        if (in_ptr < 0x1000 || out_ptr < 0x1000) {
            continue;
        }

        out_word = cmicx_rd32(s->as, out_ptr);
        in_word  = cmicx_rd32(s->as, in_ptr);
        fw       = out_word & 3u;
        granted  = in_word & 3u;

        if (fw != 0 && fw != granted && in_ptr != out_ptr) {
            cmicx_wr32(s->as, in_ptr, (in_word & ~3u) | fw);
            deliver = true;
        }

        /*
         * Complete any request the firmware has sent.
         *
         * FUN_00003a80 walks the slot array at host+0x24+i*4, and for every
         * occupied slot:
         *     slot[i] = 0;
         *     if (*(node+0x1c) == 0x81)
         *         *(node+0x1c) = (IN >> (i + 0x10)) & 1;
         *     while (pop(node+8)) sched_add_thread(...);
         *
         * so the host completes slot i by setting bit (16+i) in the input word,
         * and advances the index in bits[9:6] to make the walk visit it.  This
         * has to run on its own, not only when a phase changes: once the phase
         * handshake settles at 3 the firmware is holding an outstanding request
         * and waiting for exactly this.
         */
        if (fw == 3 && in_ptr != out_ptr) {
            for (int i = 0; i < 4; i++) {
                uint32_t node = cmicx_rd32(s->as, host + 0x24 + i * 4);

                if (!node) {
                    continue;
                }
                in_word = cmicx_rd32(s->as, in_ptr);
                {
                    uint32_t idx = ((in_word & 0x3ffu) >> 6) + 1;
                    uint32_t new_in = (in_word & ~0x3ffu)
                                      | ((idx & 0xf) << 6)
                                      | (1u << (16 + i));
                    cmicx_wr32(s->as, in_ptr, new_in);
                    deliver = true;
                }
                break;
            }
        }
    }

    /*
     * 1b. Deliver replies and complete requests, exactly as FUN_00003a80 does.
     *
     * That function runs two independent index walks, both driven by the single
     * input word the HOST writes:
     *
     *   slot walk   bits[9:6]   host+0x24+i*4
     *       node = slot[i]; slot[i] = 0;
     *       if (node+0x1c == 0x81) node+0x1c = (IN >> (16+i)) & 1;
     *       wake waiters from node+8
     *
     *   handle walk bits[5:2]
     *       hA = bswap32(*(IN + i*8 + 4));
     *       hB = bswap32(*(IN + (i+1)*8));
     *       dev  = hA & 0xff;
     *       node = pop(host + dev*8 + 0x3e4);     <-- the FIRMWARE's posted buffer
     *       node+0x10 = hA;  node+0x14 = hB;  node+0x1c = 1;
     *       wake the waiter parked on host + dev*8 + 0x50c
     *
     * Each walk spans saved_index..current_index, where "saved" is host+4 (the
     * input word as of last time) -- so the host must ADVANCE the index or the
     * loop body never executes.  And (IN & 3) == 1 returns 1 immediately without
     * walking at all, so completion cannot be folded into the phase-1 write that
     * ends the round.
     *
     * The reply payload therefore travels as handle A.  The dispatcher in
     * FUN_000042e8 reads the buffer it gets back as:
     *     +0x11  opcode            (byte 1 of node+0x10)
     *     +0x12  halfword          (bytes 2..3 of node+0x10, through revsh)
     * so handle A = 0x01000200 gives opcode 2 and a raw halfword of 0x0100,
     * whose revsh is 1 -- the BFD-start message.  It is stored byte-swapped
     * because the firmware applies bswap32 to what it reads.
     */
    for (int h = 0; h < 4; h++) {
        uint32_t host = CMICX_HOST_BASE + h * CMICX_HOST_STRIDE;
        uint32_t in_ptr  = cmicx_rd32(s->as, host + CMICX_HOST_OFF_IN);
        uint32_t out_ptr = cmicx_rd32(s->as, host + CMICX_HOST_OFF_OUT);
        uint32_t in_word, out_word;
        bool posted = false, slot = false;

        /* Aliased IN/OUT share one word, so no distinct phase can be granted. */
        if (in_ptr < 0x1000 || out_ptr < 0x1000 || in_ptr == out_ptr) {
            continue;
        }
        out_word = cmicx_rd32(s->as, out_ptr);
        if ((out_word & 3u) != 3u) {
            s->stage = 0;              /* only inside the open round */
            continue;
        }

        /* The firmware posts its receive buffer on channel 0 via FUN_00004040. */
        posted = cmicx_rd32(s->as, host + 0x3e4) != (host + 0x3e4);
        for (int i = 0; i < 16; i++) {
            if (cmicx_rd32(s->as, host + 0x24 + i * 4)) {
                slot = true;
                break;
            }
        }

        in_word = cmicx_rd32(s->as, in_ptr);

        if (posted && s->stage == 0) {
            cmicx_wr32(s->as, in_ptr + 4, 0x00020001);   /* handle A */
            cmicx_wr32(s->as, in_ptr + 8, 0x00000000);   /* handle B */
            cmicx_wr32(s->as, in_ptr,
                       (in_word & ~0x3ffu) | 3u
                       | (1u << 2) | (1u << 6) | (1u << 16));
            s->stage = 1;
            deliver = true;
        } else if (s->stage == 1) {
            /* Return to phase 1 so the spin exits and Host msg drains. */
            cmicx_wr32(s->as, in_ptr, (in_word & ~3u) | 1u);
            s->stage = 2;
            deliver = true;
        } else if (!posted && !slot && s->stage == 2) {
            s->stage = 0;              /* consumed; ready for the next round */
        }
    }

    /*
     * 3. The host-message interrupt line.
     *
     * The firmware's wait loop enables bit 9 of enable-word 2 -- absolute line
     * 73 -- immediately before sleeping, and slot 73 has no handler, so
     * irq_dispatch skips the call and invokes sched_timer_expire on wheel
     * 0x55FD8 directly.  That is what makes the sleeping thread runnable, so
     * raise the line whenever we have changed something for it to see.  A held
     * level is correct: the controller latches the request and the acknowledge
     * releases the input.
     */
    if (deliver) {
        qemu_irq_raise(s->irq);
    }

    timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CMICX_POLL_NS);
}

static void cmicx_realize(DeviceState *dev, Error **errp)
{
    BCMCMICXState *s = BCM_CMICX(dev);

    s->as = &address_space_memory;
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->poll, QEMU_CLOCK_VIRTUAL, cmicx_poll, s);
}

static void cmicx_reset(DeviceState *dev)
{
    BCMCMICXState *s = BCM_CMICX(dev);
    memset(s->attached, 0, sizeof(s->attached));
    s->sent = false;
    s->answered = false;
    s->last_slot = 0;
    s->stage = 0;
    s->round_hold = 0;
    memset(s->last_phase, 0, sizeof(s->last_phase));
    timer_del(&s->poll);
    timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CMICX_POLL_NS);
}

static void cmicx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = cmicx_realize;
    device_class_set_legacy_reset(dc, cmicx_reset);
}

static const TypeInfo cmicx_info = {
    .name = TYPE_BCM_CMICX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMCMICXState),
    .class_init = cmicx_class_init,
};

/*
 * ------------------------------------------------------------------
 * System uptime counter at 0x03235000.
 *
 * mos_uptime_ticks() (0x1fb4) reads its 64-bit "now" from two register
 * pointers: 0x0323501C (low) and 0x03235020 (high).  The scheduler arms
 * deadlines as now + timeout and fires them when "now" reaches the deadline,
 * so this counter has to be real.
 *
 * It was previously inside the unimplemented "intc-status" window, which made
 * every read return 0.  With "now" pinned at zero, a deadline of
 * 0 + 0x2540BE400 could never be met and NO scheduler timeout ever expired --
 * the host-message thread among them, which is why it slept forever after
 * arming its 10-second timer.
 *
 * Units are nanoseconds: the firmware's own 10-second constant is
 * 0x2540BE400 = 10,000,000,000, so this must advance 1e9 per second.
 * ------------------------------------------------------------------
 */
static uint64_t uptime_ns(void)
{
    return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t uptime_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t now = uptime_ns();

    switch (offset) {
    case 0x1c:                      /* low word  -- read first */
        return (uint32_t)now;
    case 0x20:                      /* high word -- read second */
        return (uint32_t)(now >> 32);
    default:
        return 0;
    }
}

static void uptime_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    /* read-only counter */
}

static const MemoryRegionOps uptime_ops = {
    .read = uptime_read,
    .write = uptime_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* The machine                                                        */
/* ------------------------------------------------------------------ */
struct BCM56870State {
    MachineState parent;

    ARMCPU *cpu;

    MemoryRegion lowram;
    MemoryRegion sysram;

    BCMIntcState *intc;
    BCMTimerState *timer;
    MemoryRegion strap_iomem;
    MemoryRegion uptime_iomem;
    uint8_t strap[0x7c];
    SerialMM *uart0;
    SerialMM *uart1;
    SerialMM *uart2;
};

#define TYPE_BCM56870_MACHINE MACHINE_TYPE_NAME("bcm56870")
OBJECT_DECLARE_SIMPLE_TYPE(BCM56870State, BCM56870_MACHINE)

static void bcm56870_init(MachineState *machine)
{
    BCM56870State *s = BCM56870_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev;

    /* --- CPU ------------------------------------------------------- */
    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));
    if (!s->cpu) {
        error_report("could not create CPU");
        exit(1);
    }

    /* --- RAM ------------------------------------------------------- */
    memory_region_init_ram(&s->lowram, NULL, "bcm56870.lowram",
                           BCM_LOWRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BCM_LOWRAM_BASE, &s->lowram);

    memory_region_init_ram(&s->sysram, NULL, "bcm56870.sys",
                           BCM_SYS_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BCM_SYS_BASE, &s->sysram);

    /*
     * --- Interrupt controller --------------------------------------
     * Created first, because the timer and UARTs route their interrupt lines
     * into it.  mmio[0] is the control block at 0x00080000; mmio[1] and
     * mmio[2] are the two enable/pending bitmap windows that irq_dispatch()
     * scans, at 0x18320000 and 0x18330000.
     */
    dev = qdev_new(TYPE_BCM_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BCM_INTC_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0x18320000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, 0x18330000);
    s->intc = BCM_INTC(dev);

    /* The controller's single output drives the CPU's IRQ input. */
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));

    /* --- System timer ---------------------------------------------- */
    dev = qdev_new(TYPE_BCM_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BCM_TIMER_BASE);
    /*
     * The firmware publishes this timer on IRQ 8 (board_init_thread ORs 0x100
     * into the enable bitmap and installs handler 0x6B1 in descriptor 8).
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(DEVICE(s->intc), "src",
                                              BCM_TIMER_IRQ));
    /* Output 1: the one-shot delay line, which the firmware registers on 9. */
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
                       qdev_get_gpio_in_named(DEVICE(s->intc), "src", 9));
    s->timer = BCM_TIMER(dev);

    /* --- Chip ID --------------------------------------------------- */
    dev = qdev_new(TYPE_BCM_CHIPID);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BCM_CHIPID_BASE);

    /*
     * --- UARTs (16550-compatible) ----------------------------------
     *
     * 16550-compatible UARTs with a 4-byte register stride: the firmware
     * writes THR at +0x00 and polls LSR (THRE/TEMT) at +0x14, i.e. register 5,
     * so regshift is 2 (5 << 2 == 0x14).  With regshift 0 the LSR would sit at
     * +0x05 and the firmware's ready-poll would never succeed, leaving the
     * console thread stuck in a delay loop.
     *
     * Each is wired to the interrupt line the firmware registers it on.  The
     * console's line matters most: its ISR (FUN_00003278) is what drains the
     * receive FIFO into the ring buffer that console_getc() reads, so without
     * it the CLI never sees typed input.
     */
    s->uart0 = serial_mm_init(sysmem, BCM_UART0_BASE, 2,
                              qdev_get_gpio_in_named(DEVICE(s->intc), "src",
                                                     BCM_UART0_IRQ),
                              1843200, serial_hd(0), DEVICE_LITTLE_ENDIAN);
    s->uart1 = serial_mm_init(sysmem, BCM_UART1_BASE, 2,
                              qdev_get_gpio_in_named(DEVICE(s->intc), "src",
                                                     BCM_UART1_IRQ),
                              1843200, serial_hd(1), DEVICE_LITTLE_ENDIAN);

    /* --- CMICx responder (host side of the control fabric) -------- */
    dev = qdev_new(TYPE_BCM_CMICX);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    /*
     * The firmware's host-message wait enables bit 9 of enable-word 2, which is
     * absolute interrupt line 73, and arms timer 0x49 (decimal 73) -- the same
     * number.  Slot 73 has no handler, so irq_dispatch calls
     * sched_timer_expire directly on wheel 0x55FD8, waking the sleeping thread.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(DEVICE(s->intc), "src", 73));

    /* --- Ethernet front-end for the DMA rings --------------------- */
    dev = qdev_new(TYPE_BCM_ETH);
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    /* --- Packet DMA engine ---------------------------------------- */
    dev = qdev_new(TYPE_BCM_PKTDMA);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x03206400);

    /* --- Mark remaining device windows as unimplemented ----------- */
    create_unimplemented_device("bcm56870.pktdma", 0x03205000, 0x1000);
    create_unimplemented_device("bcm56870.pktdma-desc", 0x03206400, 0x100);
    /*
     * System uptime counter at 0x03235000.
     *
     * mos_uptime_ticks() reads a 64-bit free-running value from 0x0323501C
     * (low) and 0x03235020 (high), and the scheduler compares deadlines against
     * it.  This window used to be an unimplemented device, so every read
     * returned 0: "now" never advanced, no deadline was ever reached, and every
     * thread that slept on a timeout slept forever -- which is precisely why
     * the host-message thread armed its 10-second timer and never woke.
     *
     * The unit is nanoseconds: the firmware's own 10-second timeout constant is
     * 0x2540BE400 = 10,000,000,000, so the counter must advance 1e9 per second.
     */
    memory_region_init_io(&s->uptime_iomem, OBJECT(machine), &uptime_ops,
                          s, "bcm56870.uptime", 0x100);
    memory_region_add_subregion(sysmem, 0x03235000, &s->uptime_iomem);
    /*
     * Strap/pinmux scratch.  The firmware uses these as ordinary
     * read-modify-write bits (FUN_000039b4 ORs bit 0, FUN_000039d8 clears
     * bits), so they need BACKING STORAGE: with RAZ/WI the read-back returns 0
     * and the bit the firmware just set disappears, which stalls the
     * host-message thread.  Nothing here is interpreted; it is only state.
     */
    memory_region_init_io(&s->strap_iomem, OBJECT(machine), &strap_ops,
                          s->strap, "bcm56870.strap", sizeof(s->strap));
    memory_region_add_subregion(sysmem, 0x03241784, &s->strap_iomem);
    create_unimplemented_device("bcm56870.cmic", 0x00030300, 0x100);
    /*
     * --- Fallback console UART at 0x00084000 ------------------------
     *
     * The image's device-type byte (offset 0xB0) is 0, so the UART setup
     * routine takes its "else" branch and uses 0x84000 as the console block
     * (types 1 and 2 would select 0x03220000 / 0x03221000).  It polls
     * [base+0x14] (LSR) for THRE|TEMT before transmitting, so this must be a
     * real 16550: if it is left unimplemented, LSR reads 0 and the console
     * thread spins in delay(1000) forever with no output at all.
     *
     * Its interrupt line is 16, which is the value the firmware registers the
     * console ISR on.
     */
    s->uart2 = serial_mm_init(sysmem, BCM_CONSOLE_BASE, 2,
                              qdev_get_gpio_in_named(DEVICE(s->intc), "src",
                                                     BCM_CONSOLE_IRQ),
                              1843200, serial_hd(2), DEVICE_LITTLE_ENDIAN);

    /* --- Load the firmware ---------------------------------------- */
    /*
     * The file is a flat image covering 0x0..0x4068F: it starts in ITCM
     * (0x0..0x1FFFF), is all-zero across the 0x20000..0x3FFFF hole, and
     * ends inside DTCM (0x40000..0x4068F).  A single load_image_targphys()
     * cannot express that, because its max_sz must bound the whole file
     * while each region is mapped separately here.  So read the file once
     * and copy each part into the region that owns it.
     *
     * -kernel is deliberately NOT used: arm_load_kernel() would try to
     * load the file itself (and fail, since it does not fit one region),
     * so we take the path entirely and just set the reset PC.
     */
    {
        gsize fsize = 0;
        gchar *fbuf = NULL;
        GError *gerr = NULL;

        if (!machine->kernel_filename) {
            error_report("bcm56870: -kernel <firmware.bin> is required");
            exit(1);
        }
        if (!g_file_get_contents(machine->kernel_filename, &fbuf, &fsize,
                                 &gerr)) {
            error_report("bcm56870: could not read '%s': %s",
                         machine->kernel_filename, gerr->message);
            exit(1);
        }

        /* The whole image lives in the flat low RAM block. */
        if (fsize > BCM_LOWRAM_SIZE) {
            error_report("bcm56870: image is %zu bytes, larger than low RAM "
                         "(%u bytes)", (size_t)fsize, BCM_LOWRAM_SIZE);
            exit(1);
        }
        address_space_write(&address_space_memory, BCM_LOWRAM_BASE,
                            MEMTXATTRS_UNSPECIFIED, fbuf, fsize);

        qemu_log_mask(LOG_UNIMP, "bcm56870: loaded %s (%zu bytes)\n",
                      machine->kernel_filename, (size_t)fsize);
        g_free(fbuf);

        /*
         * arm_load_kernel() re-reads ms->kernel_filename and would try to
         * load it as an ELF/uImage/raw single image; clear it so the boot
         * helper only sets up the reset PC and leaves our copy in place.
         */
        machine->kernel_filename = NULL;
    }

    /*
     * Boot at the ITCM reset vector.  The image's first instruction is the
     * ARM reset vector at address 0, so pointing the CPU there is all the
     * "boot" this board needs.  arm_load_kernel() is still called so that
     * the standard ARM reset handling runs, but with no kernel filename it
     * takes the firmware path and leaves our loaded image alone.
     */
    {
        static struct arm_boot_info binfo;

        binfo.loader_start = BCM_ITCM_BASE;
        binfo.ram_size = BCM_LOWRAM_SIZE;
        binfo.board_id = -1;
        arm_load_kernel(s->cpu, machine, &binfo);
    }
}

static void bcm56870_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char *const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-r5"),
        ARM_CPU_TYPE_NAME("cortex-r5f"),
        NULL
    };

    mc->desc = "Broadcom BCM56870 (Cortex-R5) bring-up board";
    mc->init = bcm56870_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-r5");
    mc->valid_cpu_types = valid_cpu_types;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

static const TypeInfo bcm56870_machine_type = {
    .name = TYPE_BCM56870_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(BCM56870State),
    .class_init = bcm56870_machine_class_init,
};

static void bcm56870_register_types(void)
{
    type_register_static(&chipid_info);
    type_register_static(&intc_info);
    type_register_static(&timer_info);
    type_register_static(&pktdma_info);
    type_register_static(&eth_info);
    type_register_static(&cmicx_info);
    type_register_static(&bcm56870_machine_type);
}

type_init(bcm56870_register_types)
