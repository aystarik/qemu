#!/usr/bin/env python3
"""
Functional check of firmware routines against known inputs.

Each test calls a real routine from the image on the emulated Cortex-R5 and
compares the result with a hand-computed expectation.  This validates the
*modelled* parts -- CPU execution, memory map, byte order -- without depending
on the unmodelled peripherals.
"""
import re, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "drive.py")
from common import IMAGE

def run(func, args="", watch=None, arm=False, timeout=1.0):
    cmd = ["python3", DRIVE, "--boot", "1.0", "--call", hex(func),
           "--timeout", str(timeout)]
    if args: cmd += ["--args", args]
    if watch: cmd += ["--watch", watch]
    if arm: cmd += ["--arm"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    out = r.stdout + r.stderr
    regs = {}
    for m in re.finditer(r"R(\d\d)=([0-9a-f]{8})", out):
        regs[int(m.group(1))] = int(m.group(2), 16)
    mem = {}
    for m in re.finditer(r"^0*([0-9a-f]+): ((?:0x[0-9a-f]{8} )+)", out, re.M):
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            mem[base + 4*i] = int(w, 16)
    return regs, mem, out

def as_str(mem, addr, maxlen=32):
    b = bytearray()
    for i in range(maxlen):
        w = mem.get(addr + (i & ~3))
        if w is None: break
        b.append((w >> (8 * (i % 4))) & 0xff)
    return bytes(b).split(b"\x00")[0].decode("latin1", "replace")

img = open(IMAGE, "rb").read()

def cstr(a):
    e = img.find(b"\x00", a)
    return img[a:e]

TESTS = []

def test(name, fn):
    TESTS.append((name, fn))

test("strlen('BFD ready\\n') == 10",
     lambda: run(0xc85c, "0xEBA5")[0].get(0) == 10)

test("strlen('board init') == 10",
     lambda: run(0xc85c, "0xDADD")[0].get(0) == 10)

def t_strcpy():
    regs, mem, _ = run(0xc798, "0x40100,0xDADD", watch="0x40100:4")
    return as_str(mem, 0x40100) == "board init"
test("strcpy copies 'board init'", t_strcpy)

def t_memset():
    regs, mem, _ = run(0x270e, "0x40200,0x5a,8", watch="0x40200:4")
    return all(mem.get(0x40200 + 4*i) == 0x5a5a5a5a for i in range(2))
test("memset fills with 0x5a", t_memset)

def t_memcmp_eq():
    # memcmp_bfd on two identical buffers -> 0
    run(0x270e, "0x40300,0x77,16")           # prepare
    regs, _, _ = run(0xc438, "0x40300,0x40300,16")
    return regs.get(0) == 0
test("memcmp(x,x,16) == 0", t_memcmp_eq)

def t_ntohl():
    # bfd_ntohl6(in, out) reads 6 big-endian words from `in` and writes the
    # byte-swapped values to `out`, returning in+0x18.  Feed it the string
    # "BFD ready\n" at 0xEBA5 and check the first two words.
    regs, mem, _ = run(0xa130, "0xEBA5,0x40600", watch="0x40600:3")
    ok_words = (mem.get(0x40600) == 0x42464420 and   # "BFD "
                mem.get(0x40604) == 0x72656164)      # "read"
    ok_ret = regs.get(0) == 0xEBA5 + 0x18
    return ok_words and ok_ret
test("bfd_ntohl6 byte-swaps word0 of 'board'", t_ntohl)

fails = 0
for name, fn in TESTS:
    try:
        ok = fn()
    except Exception as e:
        ok = False
        name += f"  (exception: {e})"
    print(("PASS  " if ok else "FAIL  ") + name)
    if not ok:
        fails += 1

print()
print(f"{len(TESTS)-fails}/{len(TESTS)} passed")
sys.exit(1 if fails else 0)
