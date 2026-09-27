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
#include "hw/core/irq.h"
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
    memory_region_init_io(&s->iomem, OBJECT(s), &chipid_ops, s,
                          TYPE_BCM_CHIPID, 0x1000);
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
 * Interrupt controller at 0x00080000.
 *
 * This is hardware we cannot model.  The firmware's accesses are IGNORED
 * (RAZ/WI): reads return 0, writes have no effect, and the block is NOT
 * wired to the CPU's IRQ line.  No interrupts are ever delivered.
 *
 * That is intentional.  Inventing an interrupt controller would fabricate
 * scheduler activity the real machine gets from real hardware, making the
 * guest's behaviour unrepresentative of the actual device.  The CPU, memory
 * map and UARTs are what this model is for; the guest's own bring-up is
 * exercised by calling its initialisation routines directly instead (see
 * emu/harness/).
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_INTC "bcm56870-intc"
OBJECT_DECLARE_SIMPLE_TYPE(BCMIntcState, BCM_INTC)

struct BCMIntcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
};

static uint64_t intc_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void intc_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    /* ignored */
}

static const MemoryRegionOps intc_ops = {
    .read = intc_read,
    .write = intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void intc_realize(DeviceState *dev, Error **errp)
{
    BCMIntcState *s = BCM_INTC(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &intc_ops, s,
                          TYPE_BCM_INTC, BCM_INTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = intc_realize;
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
 * Timer block at 0x00082000 / 0x00082020, and the interrupt enable/pending
 * bitmap banks at 0x18320000 / 0x18330000.
 *
 * All of these are hardware we cannot model, so every access is IGNORED
 * (RAZ/WI): reads return 0 and writes have no effect.  In particular this
 * block raises no interrupts.
 *
 * That is deliberate.  Synthesising a tick or a fake interrupt controller
 * would fabricate scheduler activity that the real machine obtains from real
 * hardware, so the guest would behave differently from the actual device and
 * any conclusion drawn from the run would be unsound.  The purpose of this
 * model is the CPU, the memory map and the UARTs; the guest's own bring-up
 * is exercised by calling its initialisation routines directly under a
 * harness (see emu/harness/).
 * ------------------------------------------------------------------
 */
#define TYPE_BCM_TIMER "bcm56870-timer"
OBJECT_DECLARE_SIMPLE_TYPE(BCMTimerState, BCM_TIMER)

struct BCMTimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
};

static uint64_t timer_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void timer_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    /* ignored */
}

static const MemoryRegionOps timer_ops = {
    .read = timer_read,
    .write = timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void timer_realize(DeviceState *dev, Error **errp)
{
    BCMTimerState *s = BCM_TIMER(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &timer_ops, s,
                          TYPE_BCM_TIMER, BCM_TIMER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = timer_realize;
    dc->user_creatable = false;
}

static const TypeInfo timer_info = {
    .name = TYPE_BCM_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMTimerState),
    .class_init = timer_class_init,
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
     * --- Interrupt controller -------------------------------------
     * Created before the UART so the UART can be wired to an INTC input.
     */
    dev = qdev_new(TYPE_BCM_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BCM_INTC_BASE);
    s->intc = BCM_INTC(dev);


    /* --- System timer --------------------------------------------- */
    dev = qdev_new(TYPE_BCM_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BCM_TIMER_BASE);
    s->timer = BCM_TIMER(dev);

    /* --- Chip ID -------------------------------------------------- */
    dev = qdev_new(TYPE_BCM_CHIPID);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BCM_CHIPID_BASE);

    /* --- UARTs (16550-compatible) --------------------------------- */
    /*
     * 16550-compatible UARTs with a 4-byte register stride: the firmware
     * writes THR at +0x00 and polls LSR (THRE/TEMT) at +0x14, i.e.
     * register 5, so regshift is 2 (5 << 2 == 0x14).  With regshift 0 the
     * LSR would sit at +0x05 and the firmware's ready-poll would never
     * succeed, leaving the console thread stuck in a delay loop.
     */
    serial_mm_init(sysmem, BCM_UART0_BASE, 2, 0,
                   1843200, serial_hd(0), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sysmem, BCM_UART1_BASE, 2, 0,
                   1843200, serial_hd(1), DEVICE_LITTLE_ENDIAN);

    /* --- Mark remaining device windows as unimplemented ----------- */
    create_unimplemented_device("bcm56870.pktdma", 0x03205000, 0x1000);
    create_unimplemented_device("bcm56870.pktdma-desc", 0x03206400, 0x100);
    create_unimplemented_device("bcm56870.intc-status", 0x03235000, 0x100);
    create_unimplemented_device("bcm56870.pinmux", 0x03241700, 0x100);
    create_unimplemented_device("bcm56870.cmic", 0x00030300, 0x100);
    /*
     * Fallback console when the detected device type is neither 1 nor 2.
     *
     * The image's device-type byte (0xB0) is 0, so FUN_0000335c takes the
     * "else" path and uses 0x00084000 as the console block, then polls
     * [base+0x14] (LSR) for THRE|TEMT before transmitting.  Modelling this
     * window as unimplemented makes LSR read 0, the ready-poll never
     * succeeds, and the console thread spins in delay(1000) forever with no
     * output.  So this must be a real 16550-compatible UART too.
     */
    serial_mm_init(sysmem, 0x00084000, 2, 0,
                   1843200, serial_hd(2), DEVICE_LITTLE_ENDIAN);
    /*
     * Interrupt enable/pending bitmap banks.  Unmodelled hardware: accesses
     * are ignored (RAZ/WI) rather than backed by storage, so the guest cannot
     * be misled into thinking it has configured a real interrupt controller.
     */
    create_unimplemented_device("bcm56870.irqbm0", 0x18320000, 0x1000);
    create_unimplemented_device("bcm56870.irqbm1", 0x18330000, 0x1000);

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
    type_register_static(&bcm56870_machine_type);
}

type_init(bcm56870_register_types)
