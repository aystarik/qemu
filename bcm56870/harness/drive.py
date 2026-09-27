#!/usr/bin/env python3
"""
Drive the bcm56870 machine over QEMU's gdbstub, with the monitor used for
reliable stop/start (the gdbstub's async `interrupt` is not dependable from a
batch script).

Provides a tiny library plus a CLI so individual firmware routines can be
called and inspected in isolation.
"""
import os, re, socket, subprocess, sys, time

from common import QEMU, IMAGE


class Machine:
    def __init__(self, gdb_port=1235, mon_port=1236, serial=None):
        self.gdb_port = gdb_port
        self.mon_port = mon_port
        args = [QEMU, "-M", "bcm56870", "-cpu", "cortex-r5", "-kernel", IMAGE,
                "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{mon_port},server,nowait",
                "-S", "-gdb", f"tcp::{gdb_port}"]
        if serial:
            args += ["-serial", serial]
        else:
            args += ["-serial", "null"]
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
        self._wait_port(gdb_port)
        self._wait_port(mon_port)

    def _wait_port(self, port):
        for _ in range(150):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError(f"port {port} never opened")

    def mon(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.mon_port), timeout=5)
        time.sleep(0.2)
        s.sendall(cmd.encode() + b"\n")
        time.sleep(wait)
        s.setblocking(False)
        d = b""
        try:
            while True:
                try:
                    x = s.recv(65536)
                except BlockingIOError:
                    break
                if not x:
                    break
                d += x
        except Exception:
            pass
        s.close()
        return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", d.decode("latin1", "replace"))

    def gdb(self, cmds, timeout=120):
        args = ["gdb-multiarch", "-q", "-batch"]
        for c in (["set pagination off", "set confirm off",
                   f"target remote :{self.gdb_port}"] + cmds):
            args += ["-ex", c]
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", type=float, default=0.0,
                    help="seconds to let Reset run before stopping")
    ap.add_argument("--call", default=None, help="function address to call")
    ap.add_argument("--args", default="")
    ap.add_argument("--stack", default="0x40890")
    ap.add_argument("--lr", default="0x20000",
                    help="return address (defaults to a park loop in RAM)")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--watch", default="")
    ap.add_argument("--regs", action="store_true")
    ap.add_argument("--arm", action="store_true",
                    help="call in ARM state (default is Thumb)")
    ap.add_argument("--enable-irq", action="store_true",
                    help="leave IRQs enabled so any thread the called routine "
                         "creates can actually be scheduled (default masks "
                         "them, which keeps single-routine results stable)")
    a = ap.parse_args()

    m = Machine()
    try:
        if a.boot > 0:
            m.mon("cont", wait=0.3)
            time.sleep(a.boot)
            m.mon("stop", wait=0.5)

        if a.call:
            fn = a.call if a.call.startswith("0x") else hex(int(a.call, 16))
            regs = [x for x in a.args.split(",") if x] if a.args else []
            # Enter the called routine in a clean SVC state with IRQs masked:
            # the firmware's Reset has already run, so it lives in SVC, and
            # calling from whatever mode boot happened to stop in (idle, or an
            # abort handler) would run the routine with the wrong banked
            # registers and stack.
            # The image starts in ARM (the reset vector) and switches to
            # Thumb at the 0x103xx veneers, so everything from mos_boot
            # onwards -- including memset, strlen and the log/console code --
            # is Thumb.  Getting T wrong makes the CPU decode garbage and
            # prefetch-abort to vector 0xC, so default to Thumb and allow
            # --arm for the handful of ARM routines.
            # SVC mode with the T bit set (Thumb):
            #   0x1f3 = SVC | T | F | I   (IRQs masked)  <- default
            #   0x173 = SVC | T | F       (IRQs enabled) <- --enable-irq
            #
            # The default masks IRQs so the timer cannot preempt the call and
            # clobber the result registers before we read them; without this,
            # routines that return a value (strlen, memcmp) intermittently
            # report garbage because the scheduler switched away mid-call.
            #
            # But masking IRQs also means any THREAD the routine creates is
            # never scheduled, so "did it start a worker?" always looked like
            # NO.  Pass --enable-irq when the point of the call is to let the
            # firmware's own threads run.
            cpsr = "0x1d3" if a.arm else "0x1f3"
            if a.enable_irq:
                cpsr = "0x153" if a.arm else "0x173"
            # Plant a Thumb "b ." park loop at the return address so the
            # called routine returns into a harmless spin instead of executing
            # whatever happens to be at 0x0.
            park = int(a.lr, 16) if a.lr.startswith("0x") else int(a.lr)
            setup = [
                f"set $cpsr = {cpsr}",
                f"set $sp = {a.stack}",
                f"set $lr = 0x{park | 1:x}",
                f"set {{int}}{park} = 0x0000e7fe",  # Thumb: 'b .'
            ]
            for i in range(4):
                setup.append(f"set $r{i} = {regs[i] if i < len(regs) else 0}")
            setup.append(f"set $pc = {fn}")
            print(m.gdb(setup))
            # Resume from the monitor so the stub does not hold the CPU.
            m.mon("cont", wait=0.3)
            time.sleep(a.timeout)
            m.mon("stop", wait=0.5)

        if a.regs:
            print(m.mon("info registers", wait=0.8))
        if a.watch:
            addr, _, ln = a.watch.partition(":")
            print(m.mon(f"xp/{ln or 8}xw {addr}", wait=0.8))
        print("=== monitor info registers ===")
        print(m.mon("info registers", wait=0.8))
    finally:
        m.close()


if __name__ == "__main__":
    main()
