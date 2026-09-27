#!/usr/bin/env python3
"""
Verify the packet-DMA model copies memory.

GDB's 'set {int}ADDR' writes RAM directly and does NOT go through MMIO, so the
device only sees a transfer when GUEST CODE performs it.  This test therefore
drives pkt_dma_xfer (0x4938) inside the guest:

    pkt_dma_xfer(src_lo, src_hi, dst_lo, dst_hi, len_bytes, flags)

The length argument is in BYTES; the function converts it to the word count it
programs into the length register with (len + 3) >> 2.

Buffers are identity-mapped at 0x1200000, which the model maps 1:1.
"""
import os, socket, subprocess, time, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import QEMU, IMAGE

MON, GDBP = 1302, 1301
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

def mon(cmd, w=0.7):
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
    t = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", d.decode("latin1", "replace"))
    return [l for l in t.split("\n") if l.startswith("0")]

def gdb(cmds, t=60):
    a = ["gdb-multiarch", "-q", "-batch"]
    for c in ["set pagination off", "set confirm off",
              "target remote :%d" % GDBP] + cmds:
        a += ["-ex", c]
    try:
        return subprocess.run(a, capture_output=True, text=True, timeout=t).stdout
    except Exception as e:
        return "TIMEOUT " + str(e)

try:
    mon("cont", 0.2); time.sleep(6); mon("stop", 0.5)

    # Fill the source with a known pattern using the GUEST's memset (0x270e).
    gdb(["set $cpsr = 0x1f3", "set $sp = 0x40890", "set $lr = 0x20001",
         "set {int}0x20000 = 0x0000e7fe",
         "set $r0 = 0x01200000", "set $r1 = 0xab", "set $r2 = 16",
         "set $pc = 0x270f"])
    mon("cont", 0.2); time.sleep(2); mon("stop", 0.5)
    src_before = mon("xp/2xw 0x1200000")

    # Now drive pkt_dma_xfer in the guest: src 0x1200000 -> dst 0x1200100.
    gdb(["set $cpsr = 0x1f3", "set $sp = 0x40800", "set $lr = 0x20001",
         "set {int}0x20000 = 0x0000e7fe",
         "set {int}0x40800 = 16", "set {int}0x40804 = 0",  # stack: len BYTES, flags
         "set $r0 = 0x01200000", "set $r1 = 0",            # src_lo, src_hi
         "set $r2 = 0x01200100", "set $r3 = 0",            # dst_lo, dst_hi
         "set $pc = 0x4939"])
    mon("cont", 0.2); time.sleep(3); mon("stop", 0.5)

    src = mon("xp/4xw 0x1200000")
    dst = mon("xp/4xw 0x1200100")

    print("source 0x01200000:", src)
    print("dest   0x01200100:", dst)

    def words(lines):
        out = []
        for l in lines:
            parts = l.split(":", 1)[1].split()
            out += [int(x, 16) for x in parts]
        return out

    s_w, d_w = words(src), words(dst)
    ok = s_w[:4] == d_w[:4] and s_w[0] != 0
    print()
    print("PASS: DMA copied 4 words" if ok else "FAIL: no copy")
    sys.exit(0 if ok else 1)
finally:
    p.terminate()
