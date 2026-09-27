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

/* The scheduler tick is published on IRQ 8, as the firmware itself sets up. */
#define BCM_TIMER_IRQ      8

struct BCMTimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    QEMUTimer tick;
    qemu_irq irq;

    uint32_t regs[BCM_TIMER_SIZE / 4];
    uint32_t counter;
    bool enabled;
};

static void timer_tick(void *opaque)
{
    BCMTimerState *s = BCM_TIMER(opaque);

    /* The counter is free-running and always advances. */
    s->counter++;

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
        return s->counter;
    }

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
        return;
    }

    s->regs[idx] = value;

    if (offset == 0x00c) {
        /* Control: once set, the block delivers periodic interrupts. */
        s->enabled = (value != 0);
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
    timer_init_ns(&s->tick, QEMU_CLOCK_VIRTUAL, timer_tick, s);
}

static void timer_reset(DeviceState *dev)
{
    BCMTimerState *s = BCM_TIMER(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->counter = 0;
    s->enabled = false;
    timer_del(&s->tick);
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
    create_unimplemented_device("bcm56870.intc-status", 0x03235000, 0x100);
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
    type_register_static(&bcm56870_machine_type);
}

type_init(bcm56870_register_types)
