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


## Packet DMA and the ethernet front-end

The firmware does not drive a MAC register block. It parks receive buffers in a
descriptor ring, and for transmit it builds a descriptor and rings a doorbell.
So `bcm56870-pktdma` and `bcm56870-eth` stand in for the DMA/MAC boundary.

### Why the DMA has to be real

`pkt_dma_xfer` (0x4938) **polls its status register** until `(status & 3) != 0`.
Under the previous RAZ/WI treatment that read 0 forever, so any transfer hung.
The engine now performs a genuine memory-to-memory copy of `len` words and
raises the status.

Two descriptor banks, 0x80 apart, selected by the flag byte at `0x54FBC`:

| | bank A | bank B |
|---|---|---|
| src_lo | `0x03206404` | `0x03206484` |
| src_hi | `0x03206408` | `0x03206488` |
| dst_lo | `0x0320640C` | `0x0320648C` |
| dst_hi | `0x03206410` | `0x03206490` |
| length (words) | `0x03206414` | `0x03206494` |
| control | `0x03206418` | `0x03206498` |
| status | `0x0320641C` | `0x0320649C` |

Register 0 is the **source** and register 1 the **destination**.
`harness/dmatest.py` verifies a real transfer through the guest's own code path
(note that GDB's `set {int}ADDR` bypasses MMIO, so a transfer only happens when
guest code performs it).

### The receive ring

Channel state, stride 0x18, base `DAT_00004cb0` (`0x5B03C`). The BFD receive
path uses **channel index 0xFF**, so its block is at `0x5C824`:

| offset | meaning |
|---|---|
| `+0x04` | descriptor ring base (observed `0x5B2E8`) |
| `+0x08` | ring size in entries (observed 128) |
| `+0x10` | producer, advanced by enqueue |
| `+0x14` | consumer, advanced by dequeue |

A descriptor is 16 bytes; `+0x00` is the buffer's DMA address, `+0x08` holds the
length in its low half, and **bit 31 of `+0x0C` means "the engine has filled
this"**. `FUN_00004c48` dequeues by testing exactly that bit at the consumer
index, then advances the consumer itself. So the front-end only has to fill the
buffer, store the length, and set the owner bit — it must *not* touch the
producer.

> An earlier revision of this device raised bit 0 of `0x54FAC` as a "packet
> available" flag. That word is an initialisation flag which reads 1
> permanently, not a receive signal, so injected frames were never collected.

### Can the DMA raise interrupts? No — and deliberately so

This is worth stating plainly, because front-ending DMA with an interrupt is the
obvious design and it would be wrong here:

- **Only two interrupt handlers are ever installed** in the whole firmware:
  line 8 (the scheduler tick) and line 9. There is no DMA handler.
- The receive enqueue (`FUN_0000b740`) calls the descriptor builder with
  `param_5 = 0, param_6 = 0`, so no completion interrupt is requested.
- The completion wait in `FUN_00004d3c` is a **busy-loop on the owner bit**,
  not a sleep-on-interrupt.
- The host/packet handoff is a bit in a bitmap window that a thread **polls**.

Raising an IRQ from the DMA device would therefore be inventing a signal the
firmware never configured and has no handler for.

### Status of the front-end

**What is verified:**

- the DMA engine copies memory correctly (`harness/dmatest.py`);
- the device writes a frame plus the owner bit into the correct descriptor;
- the firmware's own receive parser (`FUN_00008448`) is genuinely
  ethernet-shaped and feeds `bfd_rx_packet`.

**What is NOT yet demonstrated:** the firmware consuming an injected frame.
`harness/bfdtest.py` injects a valid BFD Control packet and the owner bit is
set, but the consumer index does not advance. **This is unresolved.**

### Why BFD never starts, and what would be needed

The blocker is not the ethernet front-end. The firmware learns about BFD
sessions, and about the receive ring, over a **host message queue** — not over
ethernet:

```
bfd_msg_thread (0x6114), blocked in FUN_00004204
   <- host message 0x41  -> allocates the session table, then calls
                            FUN_00007d3c / FUN_000099a0 / FUN_00006f48
   <- host message 0x4a  -> FUN_00006f48 -> FUN_0000b834(0)
                              -> FUN_0000b740 builds the 64-descriptor RX ring
```

`bfd_msg_thread` **never starts** in our runs (`"BFD ready"` is never printed),
so there are no sessions, and an injected packet has nothing to match against.

A "Host msg" poller thread (entry `0x3cb1`) does run and watches a shared-memory
status word at `0x0127f000` — the host IN ring announced at boot. Getting BFD up
means modelling that queue (producer/consumer plus its state machine) so the
guest collects a `0x41` followed by a `0x4a` message itself.

**Two model bugs found and fixed while investigating this:**

1. **The chip-ID region was shadowing the strap registers.** It claimed a full
   `0x1000` at `0x03241000`, so reads of the strap/pinmux block at
   `0x03241784` returned the chip-ID value `0xb870`. The firmware does
   read-modify-write there (`FUN_000039b4` sets a bit, then re-reads), so the
   bit never stuck. The chip-ID region is now `0x100` and the strap block has
   real backing storage — `0x03241788` now correctly reads back `1`.
2. **Hijacking the CPU to call firmware routines wedges interrupts.** After a
   gdb-driven `$pc`/`$cpsr` call returns, `CPSR` reads `0x1f3` (IRQs masked)
   with the timer still pending in the controller, so nothing is ever scheduled
   again. This is why threads created that way never run. It also explains the
   earlier `--enable-irq` finding. Firmware routines invoked this way should be
   treated as running in an environment where the scheduler is dead.

## What it takes to verify the firmware reads a packet

This is the honest answer to "how do we know the firmware consumed a frame?",
and it is a scope statement as much as a plan.

### The receive path is polled, so it *can* be driven — but the ring needs a host

No DMA interrupt is ever configured (see above), so the receive path is a
polling loop. In principle that means it can be exercised synchronously. The
obstacle is not the polling: it is that **the receive ring only ever comes into
existence as a consequence of a host message.**

Verified by call graph:

- `bfd_msg_thread` (`0x6114`) is the **only** caller of every function that
  builds the ring:
  - message `0x41` → `FUN_0000b980`, `FUN_00007d3c`, `FUN_000099a0`, `FUN_00006f48`
  - message `0x4a` → `FUN_00006f48` → `FUN_0000b834(0)` → `FUN_0000b740`
    (allocates the 64-descriptor ring at `0x5B2E8`)
- `FUN_00006f48`'s **only** caller is `bfd_msg_thread` (at `0x6592`).

So: no ring without `bfd_msg_thread`; no `bfd_msg_thread` without a host
message. There is no console command that starts it.

### The three ways to check, and which ones are legitimate

**(A) Model the host mailbox — the only faithful option.**
Reproduce, in the machine model:

- the shared-memory message ring the boot log announces
  (host 0: IN `0x0127f000`, OUT `0x0127f108`, four hosts);
- its handshake on the word at `IN+0`, a three-state sequence `0 → 1 → 2 → 3`
  driven by `FUN_000039b4` / `FUN_000039d8` and polled by the "Host msg" thread
  (`FUN_00003cb0`);
- the `0x634`-byte per-host control block at `0x59734`
  (`+0x01` state, `+0x0C` → status word, `+0x10` → ack register);
- delivery of message `0x41` (BFD application init), then `0x4a` (RX ring).

Then BFD starts on its own, the driver thread runs under the real scheduler, and
a frame arriving through `bcm56870-eth` is consumed by firmware code with
nothing forced from outside. **This is what an end-to-end check requires.**

**(B) Call the parser directly — legitimate but narrow.**
Invoke `FUN_00004c48` → `FUN_0000b4e4` → `FUN_00008448` with a buffer in the
layout the parser expects, and inspect the fields it fills. This does exercise
real firmware code rather than a reimplementation, so it is a fair unit test of
the parser. It does **not** demonstrate the end-to-end path, because it runs
with the scheduler dead after a CPU hijack and says nothing about the driver
thread or the ring handoff. It must not be reported as end-to-end.

**(C) Build the ring ourselves and poke the poll — rejected.**
That tests our own idea of the ring format rather than the firmware's. Circular.

### The buffer layout, and an earlier mistake

The frame does **not** start at the buffer base. From `FUN_00008448`:

| offset | meaning |
|---|---|
| `buffer+0x18` bits[29:8] | frame length (**not** the descriptor field) |
| `buffer+0x20` bits[18:7] | id; `0` means "parse as a normal frame" |
| `buffer+0x40` | start of the Ethernet frame |

The parser computes `puVar18 = (dma_addr + 0x40) - 0x1100000`, i.e. CPU
`buffer+0x40`, so a 0x40-byte BCM header sits in front of the frame.

An earlier revision of `harness/bfdtest.py` wrote the frame at offset `0` and
put the length in the descriptor. The parser would have read garbage — which
would explain a silent non-consumption **even with a correct ring**. That bug is
fixed in the current script, but the end-to-end result is still not obtained.

## Harness note: IRQs and single-routine calls

`drive.py` masks IRQs by default. That keeps a single routine's return value
stable, because otherwise the timer can preempt the call and the scheduler
switches away before the result register is read.

The side effect is that any *thread* the called routine creates is never
scheduled — which made "did this routine start a worker?" look like NO for every
test. Pass `--enable-irq` when the point of the call is to let the firmware's own
threads run:

```
./drive.py --boot 8 --call 0x6f48 --enable-irq
```

**But be aware of the stronger limitation found later:** after a hijacked call
returns, the CPU ends up IRQ-masked (`CPSR = 0x1f3`) with the timer pending, and
a gdb `$cpsr` write does not stick. `--enable-irq` sets the mask on entry; it
cannot guarantee it stays clear afterwards. Threads that only ever appear after
a hijacked call should be assumed *not* to run. Driving the firmware through its
own interfaces (the console, or the host queue) avoids this entirely.

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

- BFD never starts, because it is triggered by host queue messages that no host
  sends (see above). Until that queue is modelled, injected frames cannot reach
  a session.
- The ethernet front-end writes frames into the ring but the firmware's
  consumption of an injected frame is not yet demonstrated.
- Only the interrupt sources listed above are driven. Non-timer interrupts that
  real hardware would raise (from the packet DMA, CMIC, link events) never fire,
  so code paths waiting on those do not run.
- The packet DMA, CMIC and other peripherals are inert, so networking and the
  BFD packet path cannot be exercised.
- Timer/uptime values are not calibrated to real hardware (see the caveat above).
- Single CPU only.
- The 128 KiB hole at `0x20000..0x3FFFF` is backed by RAM here for load
  simplicity, though architecturally it is unbacked.
