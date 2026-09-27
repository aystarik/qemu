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

### Timer units (corrected)

An earlier version of this document said the timer was merely "uncalibrated".
That was too generous: the counter was **10⁷ too slow** and the system uptime
register was not modelled at all, so no scheduler timeout could ever expire. Both
are now fixed and the units are nanoseconds, matching the firmware's own
constants. See "CMICx responder" above for the three bugs and the evidence.

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

### Why BFD never starts

The blocker is not the ethernet front-end. BFD start-up is triggered by
**CMICx messages from a host**, not by ethernet traffic:

```
bfd_msg_thread (0x6114), blocked in FUN_00004204 waiting on its host channel
   <- message 0x41 -> allocate the session table, then
                      FUN_00007d3c / FUN_000099a0 / FUN_00006f48
   <- message 0x4a -> FUN_00006f48 -> FUN_0000b834(0)
                        -> FUN_0000b740 builds the 64-descriptor RX ring
```

`bfd_msg_thread` never starts in our runs (`"BFD ready"` is never printed), so
there are no sessions and an injected packet has nothing to match against. No
console command starts it either. What a CMICx responder has to provide is
described in the next section.

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
   again. Threads created that way never run. Firmware routines invoked this way
   should be treated as running where the scheduler is dead.

## CMICx: the control interface

CMICx is the firmware's control fabric. What looked like three separate features —
the host message ring, the packet DMA, and the system services — are all clients
of it. This is derived from the firmware; the parts that are inferred rather than
proven are marked.

### Requests and replies

Every off-chip operation is a **request descriptor with a reply routed back to
the requesting thread**.

**Send** — `FUN_00004174(host, req, timeout_lo, timeout_hi)`:

1. `FUN_00000ba8(req+8, req, 0x80)` initialises the descriptor's lists;
2. `FUN_00004114(host, req)` pushes it onto the host's send queue (`+0x64`);
3. if the status is still in flight (`status - 0x80 < 2`) it blocks in
   `FUN_0000408c`: push the current thread onto the request's wait queue, then
   `mos_thread_delay(timeout)`. On timeout the request is unlinked and its status
   set to `2`.

**Receive** — `FUN_00004278(host, req, expect, ...)`: `FUN_00004040` hands the
request over if the host state is `3`, otherwise parks it; `FUN_00003fc0` then
waits, returning the filled descriptor. Status `2` on failure, `3` on success.

### Header layout

Taken from the round trip `bfd_msg_thread` performs on its own reply
(`local_160 = 1; local_15f = opcode; local_15e = swap16(len); local_15c =
swap32(status)`), with the fields at `sp+0x58..0x5c`:

| offset | size | field |
|---|---|---|
| 0 | u8 | constant `1` |
| 1 | u8 | **opcode** — this is the "message type" in the log |
| 2 | u16 | length, **big-endian** |
| 4 | u32 | status/result, **big-endian** |
| 8 | … | payload |

### Completion is delivered by the scheduler, not an interrupt

This resolves the earlier question about DMA interrupts. A reply wakes a sleeping
thread — `FUN_00003a80` pops the request for a device and calls
`sched_add_thread()` on the waiter. That is why only two IRQ handlers exist
(lines 8 and 9): replies are matched by descriptor and delivered through the
scheduler, so no per-reply interrupt line is needed.

### Device dispatch and the per-host block

`FUN_00003a80` dispatches on a device selector `dev = FUN_00000474(req->field4) & 0xff`
with `dev < 0x25` (**37**, matching the 37 sub-queues the init loops clear).
`param_1` selecting the `0x634`-byte block is the **host index** (four hosts are
announced at boot), not a device.

Per-host block at `0x59734`, stride `0x634` (observed at runtime):

| offset | meaning |
|---|---|
| `+0x000` | init flag / state |
| `+0x001` | handshake state `0→1→2→3`, polled by `FUN_00003cb0` |
| `+0x004` | saved status word |
| `+0x008` | local state mirrored to hardware |
| `+0x00C` | **pointer** to the host status word (observed `0x0127f000`) |
| `+0x010` | **pointer** to the ack register (observed `0x00080000`) |
| `+0x014` | timer object |
| `+0x064` | send queue |
| `+0x3E4` | 37 × wait queues (requests awaiting a reply) |
| `+0x50C` | 37 × thread queues (threads blocked on a device) |

### CMICx vs the packet DMA

They are two halves of one mechanism: **CMICx carries the control descriptor;
the packet DMA moves the payload.** The relationship is explicit in
`FUN_00003a80`, which calls `pkt_dma_xfer(src, 0, &local_30, 0, 0x10, 2, …)` for
a reply payload, and in `bfd_msg_thread`'s request/reply pair (flags `1` and `2`
selecting direction). The log strings "BFD ERROR: reply DMA failed 0x%x" and
"SYSTEM TOD/VERSION/THREAD INFO ERROR: reply DMA failed" are this transfer
failing on each path.

### What is uncertain

The boot log prints four `msg host N in … out …` pairs, but the addresses are
**not** simply host-indexed: hosts 0 and 1 share an IN pointer and hosts 1 and 2
share an OUT pointer, with a constant `0x84` stride. Either the ring is a small
shared pool rather than one ring per host, or the log prints two pointers per
entry that alias. **I could not establish which**, so I am not claiming a ring
layout. What is verified is the per-host control block, the pointer fields, the
handshake, and the queue offsets above.

### Why this matters for the BFD work

The host mailbox that BFD start-up depends on is a CMICx client. Bringing BFD up
honestly means implementing a **CMICx responder** that answers the requests the
firmware issues — delivering `0x41` (BFD application init) and `0x4a` (RX ring
start) as proper CMICx messages with replies, rather than poking a memory flag.
That is a larger, more principled job than what I attempted before, and it is the
correct way to get a frame consumed end-to-end.

## CMICx responder

`bcm56870-cmicx` implements the host side of the CMICx mailbox handshake, so the
firmware's own "Host msg" thread can run instead of being driven by a CPU hijack.
It mirrors the phase the firmware publishes into the word the firmware waits on:

```
firmware publishes 1 to OUT -> waits for IN == 1
firmware publishes 2 to OUT -> waits for IN == 2
firmware publishes 3 to OUT -> waits for IN == 3
```

Per-host block `0x59734` + `host*0x634`; `+0x0C` is the input word the firmware
reads, `+0x10` the output word it writes. The device polls every 1 ms, grants the
published phase into the input word, and skips hosts whose pointers alias or are
not yet published.

### Three real bugs this work uncovered

The responder could not make progress at first, and chasing it found genuine
faults in the machine model — all three silently disabled every timeout in the
system, which is why the host-message thread slept forever after arming its
10-second timer.

**1. The system uptime counter was not modelled at all.**
`mos_uptime_ticks()` (`0x1fb4`) reads a 64-bit "now" from `0x0323501C` (low) and
`0x03235020` (high) — inside the window previously registered as the
unimplemented `bcm56870.intc-status`. Every read returned 0, so "now" never
advanced and no deadline could ever be reached. Now a live free-running
nanosecond counter, verified tracking real time (3.28 s, 7.48 s, 11.68 s …).

**2. The timer counter was 10⁷ too slow.**
`0x00082004` was incremented by +1 per 10 ms tick. The firmware's own 10-second
constant (`0x2540BE400` = 10,000,000,000) shows the unit is nanoseconds, so the
counter must advance 1e9 per second. It now advances in proportion to elapsed
virtual nanoseconds.

**3. The tick handler's wheel gate was never satisfied.**
The IRQ-8 handler (`0x198c`) advances the scheduler timer wheel only if the
period register is non-zero:

```c
if (_DAT_00082010 == 0) {
    return 0;                 /* wheel never advances */
}
```

`FUN_00003700` programs `+0x00`, `+0x04`, `+0x08` and `+0x0C` but never `+0x10`,
so the model left it at 0 and the handler returned immediately every tick. The
timer now publishes its period (`0x989680` = 10 ms in ns) when enabled.

With those fixed, the scheduler clock advances 1e9 per tick, the mailbox
handshake advanced from phase 1 to phase 2, and `ps` shows the "Host msg" thread
running and re-blocking (`SchedCnt` 1 → 2) rather than sitting inert.

### The CMICx responder, implemented

`bcm56870-cmicx` does three things, all derived from the firmware:

**1. Mirrors the handshake.** The firmware publishes a phase to the output word and
waits for the same value in the input word, so the device grants each published
phase (per-host block `0x59734 + host*0x634`; `+0x0C` = word the firmware reads,
`+0x10` = word it writes).

**2. Raises the host-message interrupt line.** This was the missing signal that
made the handshake actually advance. The firmware's wait loop enables bit 9 of
enable-word 2 — **absolute line 73** — immediately before sleeping, and slot 73
has **no handler**, so `irq_dispatch` skips the call and invokes
`sched_timer_expire(*(0x54FD0), 1)` directly on wheel `0x55FD8`. That is what pops
the sleeping thread off the wheel and makes it runnable. The device pulses line 73
on every phase change (held level, not `qemu_irq_pulse`, which TCG never samples).

Measured effect: the handshake went from **stuck at phase 1 forever** to
**completing all three phases** (`IN=OUT=3`), and `ps` showed the "System msg"
thread move from blocked to **READY and running** — the semaphore deadlock is
broken.

**3. Answers posted requests in place.** Directly from `FUN_00004278`:

```c
node = FUN_00003fc0(host, chan, ...);       /* pops host+chan*8+0x2bc */
if (node != param_3) log_fatal("Msg receive got a different msg");
```

so the reply is matched **by pointer identity**: the firmware posts its own
request buffer and the host must fill that same buffer, not invent a node. The
dispatcher reads the reply with the buffer base at `sp+0x24`:

| field | offset in buffer |
|---|---|
| opcode | `+0x11` |
| payload (passed through `revsh`) | `+0x12` |
| word | `+0x14` |

and the BFD-start path is `opcode == 2 && revsh(payload) == 1` → `FUN_00006e88` →
creates `"BfdMsgThread"` → logs `"BFD ready"`.

### When FUN_00006e88 runs (the "BFD ready" call)

This is answered definitively: there is **exactly one call site for
`FUN_00006e88` in the whole image**, at `0x043E8`, inside `FUN_000042e8` — the
"System msg" thread (entry `0x42e9`).

Ghidra had been folding `FUN_000042e8` into `FUN_00004278`, which is why the
dispatcher looked like one oversized function. Defining `0x42e8` as its own
function exposes the path:

```c
FUN_00004278(host, 0, buf, -1, 0x7fffffff);   /* receive on channel 0 */
if (buf[0x1c] == 1) {                         /* status: success */
    opcode = buf[0x11];
    if (opcode == 2) {
        if (revsh(u16 @ buf[0x12]) == 1) {
            if (g_pfnFUN_00006e88 != NULL) {  /* == 0x6E89 */
                FUN_00006e88();               /* creates BfdMsgThread, logs "BFD ready" */
            }
        }
    }
}
```

So three conditions on the received buffer, all of which must hold:

| field | offset | required |
|---|---|---|
| status | `+0x1c` | `== 1` |
| opcode | `+0x11` | `== 2` |
| halfword, passed through `revsh` | `+0x12` | `revsh(...) == 1` |

**A real bug this exposed in my responder.** `revsh` byte-reverses the 16-bit
value, so the *raw* halfword must be `0x0100` for `revsh` to yield 1. I had been
writing `0x0001`, which makes `revsh` return 0 — the branch could never be taken.
I had also used status `0x81`; that value belongs on *request* descriptors (where
`FUN_0000408c` waits while `(status - 0x80) < 2`), whereas this receiver compares
against exactly `1`. Both are now fixed and the arithmetic is verified:

```
bytes 00 02 00 01  ->  opcode 0x02, raw halfword 0x0100, revsh = 0x0001  ✓
```

### The round protocol, and a second missing step

Reading `FUN_00003cb0` to the end revealed why the thread never reached the
queues even with the phases completing. After the three phases it does:

```c
*(host+4) = *(host+0x0c);          /* save the granted word */
*(host+1) = 3;                     /* host state 3: System msg may now send */
FUN_00001ab8(host+0x14);           /* give the semaphore */
while (FUN_00003a80(idx) == 0) sleep;   /* returns 1 iff (IN & 3) == 1 */
*(host+1) = 0;                     /* close the send window */
FUN_00001a60(host+0x14);           /* take it back */
... drain all the queues ...
```

So the host must return the input word to **phase 1** to close each round —
mirroring alone leaves `IN == OUT == 3` forever. The responder now does this, and
rounds cycle: `grant 1 -> grant 2 -> grant 3 -> ROUND-CLOSE`, repeatedly.

### Status

`"BFD ready"` is still never printed. The call site, the conditions and the
message contents are all now known and correct, but the firmware never posts a
request for the responder to answer — `host+chan*8+0x3e4` and the send queue
`0x59798` both stay empty across all 37 channels, so the System msg thread's
receive never returns a buffer to dispatch. That is the remaining gap.

### Why the firmware never posts a request

After the phase handshake was working and rounds were cycling, the responder
still saw an empty send queue. Tracing the two message threads explains it, and
the reason is a **mutual exclusion**, not a missing signal.

`FUN_000042e8` (System msg) is the thread that sends requests. Its body is:

```c
FUN_00001a60(host+0x14, -1, 0x7fffffff);   /* TAKE the host semaphore */
FUN_00002628(10000, 0);                    /* delay */
do {
    if (*(host+1) == 3) {                  /* the send window */
        FUN_00004174(host, msg, ...);      /* SEND */
        if (done) break;
    }
    delay();
} while (true);
```

and `FUN_00003cb0` (Host msg) owns that same semaphore for the whole round:

```c
... phases 1..3 ...
*(host+1) = 3;                             /* OPEN the send window */
FUN_00001ab8(host+0x14);                   /* GIVE */
while (FUN_00003a80(idx) == 0) sleep;      /* wait for IN == 1 */
*(host+1) = 0;                             /* CLOSE */
FUN_00001a60(host+0x14);                   /* TAKE */
... drain the queues ...
```

Measured: `System msg` sits at `SchedCnt = 3` and never advances, i.e. it is
parked in `FUN_00001a60` → `mos_thread_delay`, **blocked on the semaphore while
the Host msg thread holds it**. The window is open but nobody can use it.

I tried holding the round open longer (`CMICX_ROUND_HOLD_POLLS`, 200 → 3000
polls); it changed nothing, because the gate is the semaphore, not the window
duration. By the time the Host msg thread releases the semaphore it has already
set `host+1 = 0`, so the sender's `if (host+1 == 3)` test fails on its one chance.

So the mailbox is a **host-present / host-gone handshake**, not a message pump:
it exists to tell the firmware a host is there, and the BFD-start message is not
something the firmware asks for. It has to arrive as a **host-originated handle**
in the IN area — which is exactly what `FUN_00003a80` scans:

```c
for (i = (saved & 0x3ff) >> 6; i != (IN & 0x3ff) >> 6; i = (i+1) & 0xf) {
    hA = bswap32(*(IN + i*8 + 4));
    hB = bswap32(*(IN + (i+1)*8));
    dev = hA & 0xff;                       /* valid when dev < 0x25 */
    ...
}
```

The host advances a handle index in the IN word's bits[9:6] and puts handle
values at `IN + n*8 + 4` / `IN + (n+1)*8`. I have been driving only the phase
bits and leaving every handle zero, so the scan walks an interval containing no
handles and does nothing. Implementing that handle write — with a value that
selects the device whose message triggers `FUN_00006e88` — is the next step.

### Status of this round

`"BFD ready"` is not printed, and `0x41010` (the BFD thread handle) stays 0.

Net progress across the CMICx work, all verified:

- the phase handshake completes (1 → 2 → 3) and rounds cycle, where it was
  previously stuck at phase 1 forever;
- `System msg` was unblocked from never-running to running;
- the `FUN_00006e88` call site and its three conditions are known exactly, and
  the reply encoding bug (`revsh` raw halfword must be `0x0100`, status must be
  `1`) is fixed;
- the line-73 wake interrupt is implemented.

Regression stays clean: 0 aborts, 0 unassigned, suite 6/6, DMA PASS.

### What prevents "BFD ready" — answered

Using Ghidra to follow the delay path (rather than guessing) gives a definite
answer, and it is **two model bugs**, both now fixed.

**The gate is the blocking delay in `FUN_00002628`, not the mailbox.**

`FUN_000042e8` (System msg) begins with

```c
FUN_00001a60(host+0x14, -1, 0x7fffffff);   /* acquire */
FUN_00002628(10000, 0);                    /* <- blocks HERE */
```

and `FUN_00002628` expands to:

```c
deadline = mos_uptime_ticks() + delay;     /* absolute ns deadline */
FUN_00001ce0(&w);                          /* insert into the delay list */
  FUN_00003754(0x00082020, ...);           /* arm the ONE-SHOT hardware timer */
FUN_00001d94(&w);                          /* mos_thread_suspend_self()  SLEEP */
```

So the thread suspends, and the wake is **IRQ 9** (`0x3798` → `sched_timer_tick`),
which expires the delay list.

**Bug 1: the one-shot delay timer was not modelled.** `FUN_00003754` arms a
one-shot countdown at **`0x00082020`** (`[0x20] = countdown`, `[0x28] = 0xA3`
arm) and the firmware registers it on **IRQ 9**. Only IRQ 8 (the periodic tick)
was ever raised, so any thread that slept on a delay suspended forever. The timer
now models block B as a real one-shot whose expiry asserts line 9.

**Bug 2: the countdown unit conversion was ~86,000× too large.** From the
firmware's own arithmetic:

```
count = (remaining_ns * 1000) / scale     scale = *DAT_00003794 = 1166
=> ns = count * 1166 / 1000
```

Confirmed against the firmware's own arm value: `FUN_00002628(10000, 0)` gives
`10000 * 1000 / 1166 = 8576`, exactly the count observed at runtime. I had been
multiplying by `100000`, so a 10 µs delay became 857 ms and a 1 s delay became
~24 hours — which is why the delay looked like it never expired. Fixed, and the
conversion is now verified: `count=8576 → 9999 ns`.

### Where it stands now

With both fixed, the delay timer arms and fires correctly and IRQs are delivered
(2496 per 25 s, 0 aborts, 0 unassigned). `"BFD ready"` is **still not printed**,
and the remaining blocker is the semaphore mutual exclusion described in the
previous section: `System msg` acquires `host+0x14` and then blocks in the delay,
while `Host msg` holds that semaphore for the whole handshake round. The send
window sits open (`host+1 = 3`) but the only thread that could use it is asleep
on a delay that — while now modelled correctly — still precedes its window check.

So the precise answer to "what prevents BFD ready" is a chain of three, of which
the first two are now fixed:

1. the one-shot delay timer at `0x82020` was unmodelled (fixed);
2. its countdown conversion was wrong by ~86,000× (fixed);
3. `System msg` cannot reach its `if (host+1 == 3)` send test while `Host msg`
   owns the shared semaphore — still open.

### The completion protocol — the actual missing action

This is what finally came out of tracing rather than theorising, and it also
invalidated a series of earlier measurements.

**My gdb measurements were unreliable.** The `commands N / silent / printf /
continue / end` pattern followed by a trailing `continue` conflicts: once the
first breakpoint fires, the outer `continue` is rejected with *"Cannot execute
this command while the target is running"* and the guest stays **stopped**. So
every "0 hits" count I reported from that harness was measuring a halted target,
not a thread that failed to reach a location. The firmware was making far more
progress than those numbers suggested. Sampling through the monitor has the same
class of problem: each `xp` stops and restarts the VM, which perturbs exactly the
timing being observed.

**What direct sampling then showed.** The firmware really does send. With the
machine running undisturbed, the host block contains:

```
host+0x24 = 0x0007e564   slot[0] occupied -> an outstanding request
host+0x00 = 0x00000300   state byte 3      -> the send window is OPEN
0x127f108 = 0x0000000f   OUT word: phase 3, handle index 3
0x127f10c = 0x00040001   0x127f110 = 0x00000404    |  the handles it published
0x127f114 = 0x00100000   /
```

so the System msg thread had already passed its window check and sent a request.

**The completion mechanism.** `FUN_00003a80` walks the slot array
`host+0x24+i*4`; for each occupied slot it clears the slot, and:

```c
if (*(node+0x1c) == 0x81)
    *(node+0x1c) = (IN >> (i + 0x10)) & 1;    /* host sets bit 16+i */
while (pop(node+8)) sched_add_thread(...);     /* wake the waiter */
```

The host completes slot *i* by setting **bit 16+i** in the input word and
advancing the handle index in bits[9:6] so the walk reaches it. That is the
action I had never performed, and it is now implemented, decoupled from the phase
grant so it still runs once the handshake settles at 3.

`"BFD ready"` is still not printed, but the outstanding request is now known to
exist and the completion path for it is in place rather than missing.

## RESULT: BFD ready

**`"BFD ready"` is reached, and `_bfd_msg_thread` is a live scheduled thread.**
Confirmed by the firmware's own `ps`:

```
ID         Name       Priority State  lock SchedCnt         run_time   %
0x00007e128 _bfd_msg_thread  10     1     3        1        -11693540  7120159
```

with the console showing, in the same run:

```
BFD Message thread started: _bfd_msg_thread
BFD _bfd_msg_thread: in_buffer=0x127ee68, size=380
BFD _bfd_msg_thread: out_buffer=0x127ecc8, size=380
BFD ready
```

`ps` listing the thread is independent confirmation: it is not merely a log line.

### Why it looked impossible for so long

**The blocker was mostly my own instrumentation, not the firmware.** Two
measurement faults sent me chasing phantoms for many rounds:

1. **A gdb script that silently halted the guest.** `commands N / silent /
   printf / continue / end` followed by a trailing `continue` means the outer
   `continue` is rejected (*"Cannot execute this command while the target is
   running"*) once the first breakpoint fires, leaving the CPU stopped. Every
   "0 hits" count from that harness described a halted target, not the guest.
2. **Monitor sampling perturbed timing.** Each `xp` stop/restarts the VM, so
   sampling the mailbox froze exactly the handshake being observed.

Replacing both with QEMU's `-d plugin` + `contrib/plugins/libexeclog.so,afilter=`
gives a genuine, non-intrusive execution trace, and that is what produced the
real answer.

### The actual mechanism (all four pieces, now implemented)

**1. The one-shot delay timer at `0x00082020` (IRQ 9).** `FUN_00002628` expands
to `FUN_00001ce0` → `FUN_00003754(0x82020, ...)` → `FUN_00001d94`
(`mos_thread_suspend_self`). Only IRQ 8 was ever raised, so every sleeping thread
slept forever.

**2. Its unit conversion.** `count = (ns * 1000) / 1166`, so `ns = count * 1166 /
1000`. Verified against the firmware's own arm value: `FUN_00002628(10000, 0)`
yields exactly the observed `count = 8576`. My factor was ~86,000× too large.

**3. Two index walks in one input word.** `FUN_00003a80` runs a **slot walk** on
bits[9:6] (`host+0x24+i*4`, completing `node+0x1c` from bit `16+i`) and a
**handle walk** on bits[5:2]:

```c
hA  = bswap32(*(IN + i*8 + 4));
dev = hA & 0xff;
node = dlist_pop_front(host + dev*8 + 0x3e4);   /* the firmware's OWN buffer */
node+0x10 = hA;  node+0x14 = hB;  node+0x1c = 1;
wake the waiter parked on host + dev*8 + 0x50c
```

Both walks span `saved_index..current_index` (saved = `host+4`), so the host must
**advance the index** or the loop body never runs. And `(IN & 3) == 1` returns 1
*without* walking, so completion cannot be folded into the phase-1 write that
ends the round — hence the two-step sequence: advance the index with the
completion bit, then return to phase 1.

**4. The reply payload is a handle.** The dispatcher reads the buffer it gets
back at `+0x11` (opcode) and `+0x12` (halfword, via `revsh`). Since the firmware
applies `bswap32`, handle A = `0x00020001` decodes to `node+0x10 = 0x01000200`:
opcode 2, raw halfword `0x0100`, `revsh(0x0100) = 1` — the BFD-start message.

### Verification

- `"BFD ready"` appears **3× per run, deterministically across repeated runs**.
- `ps` independently lists `_bfd_msg_thread` as a live thread.
- The non-intrusive trace shows the full chain executing: `0x4364` receive,
  `0x43c4` opcode test, `0x6e88` BFD-start call, `0x6114` thread entry.

### One caveat, stated plainly

The `"BFD ready"` *log line* is gated on `*g_pLogLevel > 0`:

```c
if (iVar4 == 0) {
    if (0 < *g_pLogLevel)
        log_printf(g_szBFDReady);
}
```

At the default log level the thread **is still created and scheduled** (confirmed
via `ps`), but the message is suppressed. Raise it with the console command
`loglevel debug1` to see the line. This is firmware behaviour, not a model gap,
so the machine is left at the firmware's own default.

### Historical: the earlier blocked state

The responder is implemented and the handshake now completes, but **`"BFD ready"`
is still never printed** and no receive ring is built. The remaining gap is that
the firmware never posts a request for the responder to answer: across all 37
channels, `host+chan*8+0x3e4` stays empty and the send queue `0x59798` stays
empty. So there is nothing on the wire yet to reply to.

Two earlier mistakes corrected along the way:

- I queued the message on **channel 1** (`0x599f8`). That belongs to
  `bfd_msg_thread`, which does not exist until this very message starts it. The
  System msg thread (`0x42e9`) receives on **channel 0** (`0x599f0`).
- I built my own node and queued it. The pointer-identity check above means the
  host must fill the firmware's own posted buffer instead.

I am reporting this as unfinished. What stands on its own: the three timer fixes,
the handshake now completing, the System msg thread unblocking, and a clean
regression (boot 0 aborts / 0 unassigned, suite 6/6, DMA PASS).

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

- BFD never starts, because it is triggered by CMICx host messages that no host
  sends (see above). Until a CMICx responder exists, injected frames cannot
  reach a session.
- The CMICx host-ring geometry is not fully established (see "What is
  uncertain").
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
