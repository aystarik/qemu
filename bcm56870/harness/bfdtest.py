#!/usr/bin/env python3
"""
Inject an ethernet-encapsulated BFD Control packet into the firmware's receive
ring and observe whether the firmware's own receive path consumes it.

Handoff mechanism (from FUN_00004c48, the dequeue):
  the firmware reads the descriptor at the channel's CONSUMER index and looks at
  bit 31 of descriptor+0x0C.  If set, it takes the descriptor and advances the
  consumer itself.  So the engine only has to fill the buffer and set the owner
  bit -- it must NOT touch the producer.

BUFFER LAYOUT (from FUN_00008448; an earlier version of this script got it
wrong, which is why nothing was consumed):
    buffer[+0x18] bits[29:8] = frame length  (NOT a descriptor field)
    buffer[+0x20] bits[18:7] = id; 0 means "parse as a normal frame"
    buffer[+0x40]            = start of the Ethernet frame
  The parser computes (dma_addr + 0x40) - 0x1100000, so a 0x40-byte BCM header
  sits in front of the frame.

KNOWN STATUS: this does not yet produce a consumed frame, because BFD is never
started (it needs host mailbox messages 0x41 then 0x4a) and so no receive ring
is built.  See the README section "What it takes to verify the firmware reads a
packet".

Channel 0xFF is the BFD receive path; its state block is at
   0x5B03C + 0xFF*0x18 = 0x5C824
"""
import os, socket, subprocess, time, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import QEMU, IMAGE

MON, GDBP = 1452, 1451
CHAN = 0x5C824
CH_RING, CH_SIZE, CH_PROD, CH_CONS = 0x04, 0x08, 0x10, 0x14
DESC_LEN, DESC_ST = 0x08, 0x0C

def bfd_frame():
    """Minimal valid BFD Control packet (RFC 5880 4.1) in UDP/IPv4/Ethernet."""
    hdr = struct.pack(">BBBBIIII", 0x20, 0xc0, 3, 24,
                      0x11223344, 0x00000000, 1000000, 1000000)
    udp = struct.pack(">HHHH", 49152, 3784, 8 + len(hdr), 0) + hdr
    eth = (b"\x00\x11\x22\x33\x44\x55" + b"\x66\x77\x88\x99\xaa\xbb" +
           struct.pack(">H", 0x0800))
    ip = (struct.pack(">BBHHHBBH", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0) +
          bytes([10, 0, 0, 1]) + bytes([10, 0, 0, 2]))
    return eth + ip + udp

p = subprocess.Popen([QEMU, "-M", "bcm56870", "-cpu", "cortex-r5",
                      "-kernel", IMAGE, "-display", "none", "-serial", "null",
                      "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
                      "-S", "-gdb", "tcp::%d" % GDBP],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for port in (GDBP, MON):
    for _ in range(200):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.1)

def raw(cmd, w=0.6):
    s = socket.create_connection(("127.0.0.1", MON), timeout=5); time.sleep(0.2)
    s.sendall(cmd.encode() + b"\n"); time.sleep(w)
    s.setblocking(False); d = b""
    try:
        while True:
            try: x = s.recv(65536)
            except BlockingIOError: break
            if not x: break
            d += x
    except Exception: pass
    s.close()
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", d.decode("latin1", "replace"))

def rdw(addr):
    t = raw("xp/1xw 0x%x" % addr)
    m = re.search(r"^([0-9a-f]{8}):\s*(0x[0-9a-f]+)", t, re.M)
    return int(m.group(2), 16) if m else None

def gdb(cmds, t=40):
    a = ["gdb-multiarch", "-q", "-batch"]
    for c in ["set pagination off", "set confirm off",
              "target remote :%d" % GDBP] + cmds:
        a += ["-ex", c]
    try:
        return subprocess.run(a, capture_output=True, text=True, timeout=t).stdout
    except Exception:
        return "TIMEOUT"

try:
    raw("cont", 0.2); time.sleep(8); raw("stop", 0.5)
    gdb(["set $cpsr = 0x173", "set $sp = 0x40890", "set $lr = 0x20001",
         "set {int}0x20000 = 0x0000e7fe", "set $pc = 0x6f49"])
    raw("cont", 0.2); time.sleep(6); raw("stop", 0.6)

    ring = rdw(CHAN + CH_RING)
    size = rdw(CHAN + CH_SIZE)
    prod = rdw(CHAN + CH_PROD)
    cons = rdw(CHAN + CH_CONS)
    print("channel 0xFF: ring=0x%08x size=%d producer=%d consumer=%d" %
          (ring, size, prod, cons))
    if not ring:
        print("RESULT: the firmware has not set up the RX ring")
        sys.exit(2)

    desc = ring + cons * 16
    dma = rdw(desc)
    cpu = dma - 0x1100000 if 0x1100000 <= dma < 0x1140000 else dma
    frame = bfd_frame()
    print("desc[%d] dma=0x%08x cpu=0x%08x  injecting %d bytes at buffer+0x40" %
          (cons, dma, cpu, len(frame)))

    cmds = []
    for i in range(0, len(frame), 4):
        w = struct.unpack("<I", frame[i:i+4].ljust(4, b"\0"))[0]
        cmds.append("set {int}0x%x = 0x%x" % (cpu + 0x40 + i, w))
    # Length lives in buffer+0x18 bits[29:8]; id in buffer+0x20 bits[18:7] = 0;
    # the Ethernet frame itself starts at buffer+0x40.
    cmds.append("set {int}0x%x = 0x%x" % (cpu + 0x18, (len(frame) & 0x3fffff) << 8))
    cmds.append("set {int}0x%x = 0" % (cpu + 0x20))
    cmds.append("set {int}0x%x = 0x80000000" % (desc + DESC_ST))
    gdb(cmds)

    print("after injection: desc status =", hex(rdw(desc + DESC_ST) or 0))
    print()
    print("letting the guest run 10s ...")
    raw("cont", 0.2); time.sleep(10); raw("stop", 0.5)

    cons2 = rdw(CHAN + CH_CONS)
    prod2 = rdw(CHAN + CH_PROD)
    print("after 10s     : producer=%d consumer=%d" % (prod2, cons2))
    print()
    if cons2 is not None and cons != cons2:
        print("RESULT: the firmware CONSUMED the frame (consumer %d -> %d)"
              % (cons, cons2))
    else:
        print("RESULT: consumer unchanged - frame not taken")
finally:
    p.terminate()
