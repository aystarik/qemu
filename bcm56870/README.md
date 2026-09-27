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
```

Achieved with **zero exceptions** and **zero unassigned-memory accesses**.

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
| `0x00080000` | 4 KiB | Interrupt controller (ignored) |
| `0x00082000` | 4 KiB | System timer (ignored) |
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
  sits at `+0x14` where the firmware polls it.
- The chip-ID register.

**What is deliberately NOT modelled: every peripheral whose semantics we do not
know.** All such MMIO windows — the interrupt controller at `0x00080000`, the
system timer at `0x00082000`, the IRQ enable/pending bitmap banks at
`0x18320000`/`0x18330000`, the packet DMA, CMIC, strap/pinmux and others — are
**ignored (RAZ/WI)**: reads return 0, writes have no effect, and nothing raises
an interrupt.

This is a deliberate choice. An earlier revision synthesised a timer tick and an
interrupt controller so the scheduler would keep running. That produced a guest
that looked busier but was behaving differently from the real device, which
makes any conclusion drawn from a run unsound: the scheduler was being driven by
emulator-invented events rather than by hardware. Ignoring unknown MMIO is the
honest option — the guest sees "device present but inert", and nothing is
fabricated.

**Consequences.** Because no interrupts are delivered, paths that depend on them
do not progress:
- The CLI reads from a software ring buffer filled by a console ISR, so **typed
  commands are not consumed**; the `=>` prompt appears and the machine idles
  there.
- Tick-driven scheduler preemption does not advance.

The boot messages above do not depend on interrupts, so they appear normally.

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

- No interrupt delivery (see above), so CLI input and tick-driven preemption do
  not work.
- The packet DMA, CMIC and other peripherals are inert, so networking and the
  BFD packet path cannot be exercised.
- Single CPU only.
- The 128 KiB hole at `0x20000..0x3FFFF` is backed by RAM here for load
  simplicity, though architecturally it is unbacked.
