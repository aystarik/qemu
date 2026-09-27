# BCM56870 Cortex-R5 emulator (QEMU)

A QEMU machine that runs the recovered Broadcom BCM56870 (Trident3)
Cortex-R5 firmware image far enough to check basic behaviour: the CPU, the
memory map, and the console UART.

This is a **local bring-up / debugging aid**, not a faithful device model, and
it is not intended for upstream QEMU. See "Scope and honesty" below for what
is and is not emulated.

## Result

The firmware boots to its interactive CLI prompt:

```
MOS uKernel: SW Version:  Release 4.4.0 Thu Apr 28 19:51:04 PDT 2022
 4.4.0.0 Release Thu Apr 28 19:51:04 PDT 2022 BFD
pkt DMA Inited
msg host 0 in 0x0127f000 out 0x0127f108
msg host 1 in 0x0127f000 out 0x0127f084
msg host 2 in 0x0127f108 out 0x0127f108
msg host 3 in 0x0127f210 out 0x0127f18c
Board initialization complete
=>
help
Help:
	       bfd	BFD unit tests
	     board	Board specific commands
	     cache	Cache control
	   history	history
	      help	list commands
	         ?	list commands
	  loglevel	Enable tracing. Valid values are error|warn|info|debug0|debug1
	    memory	[test|report|read|write|dump|bench]
	       mpu	MPU display
	        ps	list threads
	      rupt	rupt commands
	     stack	stack usage report
	     stats	[show|reset] - statistics control
	   version	version info

ps
ID         Name       Priority State  lock SchedCnt         run_time   %
0x00058c7c Idle              0     2     1        1    1385999998383  99
0x0007ff68 board init       15     3     3        4               -2  13261498
0x0007eec8 cmicx_uart slih  11     1     3        2       2000000000   0
0x0007ea30 Host msg         10     2     4        1       1000000000   0
0x0007e5b0 System msg        6     1     2        1                0   0
```

Achieved with **zero data aborts** and **zero unassigned-memory accesses**, and
the CLI is fully interactive (interrupts are delivered).

## Layout

Everything for this experiment lives under `qemu/`, so the whole thing can be
committed to the QEMU checkout's git tree:

```
qemu/
  hw/arm/bcm56870.c           the machine model (the only file added to QEMU)
  hw/arm/Kconfig              + config BCM56870
  hw/arm/meson.build          + the bcm56870.c source entry

  bcm56870/                   this experiment's own files
    README.md                 this file
    harness/
      common.py               locates the QEMU binary and the firmware image
      drive.py                boot / call-one-routine / dump-registers harness
      suite.py                functional tests against known inputs
      uart_test.py            UART register and transmit check

  build/qemu-system-arm       the built emulator (build/ is git-ignored)
```

The firmware image is a vendor binary and is deliberately **not** committed.
The harness finds it by searching upward from the QEMU tree, or you can point at
it explicitly:

```
export BCM56870_IMAGE=/path/to/BCM56870_0_bfd_cortex-r5.bin
```

`BCM56870_QEMU` likewise overrides the path to the emulator binary.

## Building

QEMU 11.1.50 rejects newer meson option syntax, so use the bundled meson 1.3.2:

```
cd qemu
PATH="$(pwd)/../.venv/bin:$PATH" ./configure --target-list=arm-softmmu \
    --disable-docs --disable-tools --disable-gtk --disable-sdl --disable-werror
ninja -C build qemu-system-arm
```

Requires the usual glib-2.0 / pixman-1 dev packages. QEMU ships its own
`cortex-r5` and `cortex-r5f` CPU models, so no CPU model work was needed.

## Running

```
qemu/build/qemu-system-arm -M bcm56870 -cpu cortex-r5 \
    -kernel BCM56870_0_bfd_cortex-r5.bin \
    -serial null -serial null -serial stdio -display none -monitor none
```

Note the **third** `-serial`: the console is the UART at `0x00084000`, which is
the machine's third serial port. Mapping `stdio` to the first port shows
nothing, because the first and second UARTs (`0x03220000`, `0x03221000`) are not
the active console in this build.

Exit with `Ctrl-A x`.

## Memory map

| Base | Size | What |
|---|---|---|
| `0x00000000` | 512 KiB | Low RAM: ITCM + gap + DTCM, flat |
| `0x01200000` | 1 MiB | System heap / message buffers |
| `0x00080000` | 4 KiB | Interrupt controller (modelled) |
| `0x00082000` | 4 KiB | System timer, IRQ 8 (modelled) |
| `0x18320000` | 4 KiB | IRQ enable/pending bitmap bank A |
| `0x18330000` | 4 KiB | IRQ enable/pending bitmap bank B |
| `0x03220000` | — | 16550 UART 0 |
| `0x03221000` | — | 16550 UART 1 |
| `0x00084000` | — | 16550 UART 2 — **the console** |
| `0x03241000` | 4 KiB | Chip/device ID (reads `0xB870`) |

The firmware image is a single flat object spanning `0x0..0x4068F`. It crosses
what are architecturally separate regions (ITCM at `0x0`, an unbacked hole at
`0x20000..0x3FFFF`, DTCM at `0x40000`), and its Reset code zero-fills across
the hole, so the machine maps one flat block and loads the image into it
directly. `-kernel` is handled by the machine itself rather than
`arm_load_kernel`, because a single `load_image_targphys` cannot express a file
that spans two regions.

## Scope and honesty

This is a local experiment and is **not** proposed for upstream merge.

**What is modelled**
- The `cortex-r5` CPU (QEMU's existing model).
- The memory map above, byte-exact against the image.
- Real 16550 UARTs with a 4-byte register stride (`regshift = 2`), so `LSR`
  sits at `+0x14` where the firmware polls it, wired to their interrupt lines.
- The chip-ID register.
- The interrupt controller and the system timer, to the extent the firmware
  actually uses them (see below).

**Interrupts are modelled**, because the firmware's own operation depends on
them: without them the console's receive ISR never fills the ring buffer that
`console_getc()` reads, so the CLI cannot accept input at all.

Peripherals whose semantics we *cannot* determine are still ignored (RAZ/WI) —
the packet DMA, CMIC, strap/pinmux and the second DMA engine. Nothing is
invented for those.

### Interrupt architecture

Recovered from the firmware, not assumed:

| Line | Device | Handler |
|---|---|---|
| 8 | timer `0x00082000` | `0x6B1` → scheduler tick |
| 9 | timer alias `0x00082020` | `0x3799` |
| 16 | console UART `0x00084000` | `FUN_00003278` (RX/TX ISR) |
| 40 | UART0 `0x03220000` | registered by the UART setup |
| 41 | UART1 `0x03221000` | registered by the UART setup |

The controller at `0x00080000` is **not a stock ARM GIC**, and it is worth
recording why, since reaching for QEMU's `arm_gic` is the obvious move:

- the current IRQ is read from `INTC+0xF00`; a GIC returns it from `GICC_IAR`
  and has no register there;
- it is acknowledged by **writing zero to `INTC+0xF00`**; a GIC uses `GICC_EOIR`;
- the enable/pending bitmaps live in a **separate window** at `0x18320090` /
  `0x183200B0`, not inside the controller's own 0x1000 block where a GIC would
  put `GICD_ISPENDR` at `dist+0x200`.

`irq_dispatch()` reads `+0xF00`, rounds down to a group of eight, scans that
group in the two bitmap windows to find the line, calls
`handler_table[irq]` (0x38-byte descriptors at `0x54FD8`), writes
`1 << (irq/8 & 31)` to `+0x14`, then clears `+0xF00` to acknowledge.

Two details that were easy to get wrong:
- the bitmaps are **arrays of words indexed by `irq/32`**, so the dispatcher
  walks `base+0, base+4, … base+0x1C`. Treating the offsets as single registers
  makes every scan read past the end and find nothing.
- the controller must **latch** an incoming request independently of how long
  the peripheral holds the line, and the acknowledge must release the device
  input. Otherwise the acknowledge is undone by the still-asserted input and the
  CPU re-enters the vector forever.

### Timer rate caveat

The tick period is 10 ms of virtual time, chosen so the guest makes visible
progress; the real part's clock rate is not known, so **timing values reported
by the guest are not meaningful in absolute terms.** The `ps` output, for
example, shows the idle thread with an implausible `run_time` and 99% — the
counter advances (13.9e12 → 23.8e12 over 10 s of wall time, so the plumbing is
correct) but the scale does not correspond to real hardware. Treat these as
"the mechanism works", not as measurements.

## Testing routines directly

Because the full boot depends on hardware we cannot model, individual routines
are exercised directly instead. `harness/drive.py` boots the machine under
QEMU's gdbstub, plants a Thumb park loop as a return address, calls a chosen
function with caller-supplied arguments, and reports registers and memory.

```
cd qemu/bcm56870/harness
./drive.py --boot 1.0 --call 0x270e --args 0x40100,0xAA,16 --watch 0x40100:4
./suite.py          # functional tests
./uart_test.py      # UART check
```

Note the call convention: everything from `mos_boot` onwards is **Thumb** (the
image starts in ARM at the reset vector and switches at the `0x103xx` veneers),
so `drive.py` sets the CPSR T bit by default; pass `--arm` for the few ARM
routines. Getting this wrong makes the CPU decode garbage and prefetch-abort to
vector `0xC`, which happened repeatedly during bring-up.

`suite.py` currently passes 6/6:

```
PASS  strlen('BFD ready\n') == 10
PASS  strlen('board init') == 10
PASS  strcpy copies 'board init'
PASS  memset fills with 0x5a
PASS  memcmp(x,x,16) == 0
PASS  bfd_ntohl6 byte-swaps word0 of 'board'
```

`uart_test.py` confirms the UART registers read back correctly (`LSR = 0x60`,
i.e. THRE|TEMT) and that a character written to `THR` reaches the host.

## How the hardware details were established

Every address and register offset above was recovered from the firmware by
static analysis (Ghidra), not guessed:

- The UART uses a 4-byte register stride: `console_putc_direct` writes `THR` at
  `+0x00` and polls `LSR` at `+0x14`, and `0x14 >> 2 == 5` is the LSR register.
  With `regshift = 0` the ready-poll never succeeds and the console thread spins
  forever.
- The console base is `0x00084000` because the image's device-type byte at
  offset `0xB0` is `0`, which selects the fallback branch of the UART setup
  routine (`1 -> 0x03220000`, `2 -> 0x03221000`, else `0x00084000`). This was
  found only after the UART appeared to be working at the other addresses.
- The device-ID gate is `(*(u16 *)0x03241000 & 0xfff0) == 0xB870`, checked by
  `mos_boot`, which is why the chip-ID register is modelled.

## Known gaps

- Only the interrupt sources listed above are driven. Non-timer interrupts that
  real hardware would raise (from the packet DMA, CMIC, link events) never fire,
  so code paths waiting on those do not run.
- The packet DMA, CMIC and other peripherals are inert, so networking and the
  BFD packet path cannot be exercised.
- Timer/uptime values are not calibrated to real hardware (see the caveat above).
- Single CPU only.
- The 128 KiB hole at `0x20000..0x3FFFF` is backed by RAM here for load
  simplicity, though architecturally it is unbacked.
