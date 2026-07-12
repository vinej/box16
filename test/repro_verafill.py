#!/usr/bin/env python3
"""Reproduce the crash: step into x16_vera_fill and single-step its loop."""
import argparse
import struct
import sys
import binmon_test as B


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6502)
    ap.add_argument("--lbl", default="build/bounce.lbl")
    ap.add_argument("--prg", default="build/bounce.prg")
    ap.add_argument("--steps", type=int, default=1000)
    args = ap.parse_args()

    fill = B.find_label(args.lbl, "x16_vera_fill")
    c = B.Client(args.host, args.port)
    B.attach_handshake(c, args.prg)
    bp = struct.pack("<HHBBBBB", fill, fill, 1, 1, 4, 0, 0)
    _, err, rbody, _ = c.recv_response(c.send(B.CMD_CHECKPOINT_SET, bp))
    (num,) = struct.unpack_from("<I", rbody, 0)
    c.events.clear()
    c.recv_response(c.send(B.CMD_EXIT))
    _, ebody = c.wait_event(B.RESP_STOPPED, timeout=40)
    (pc,) = struct.unpack_from("<H", ebody, 0)
    B.check(pc == fill, f"stopped at x16_vera_fill ${pc:04x}")
    c.recv_response(c.send(B.CMD_CHECKPOINT_DELETE, struct.pack("<I", num)))

    last = pc
    for i in range(args.steps):
        try:
            c.events.clear()
            c.recv_response(c.send(B.CMD_ADVANCE, struct.pack("<BH", 0, 1)), timeout=10)
            _, rbody = c.wait_event(B.RESP_REGISTER_INFO, timeout=10)
            last = B.parse_registers(rbody)[3]
            c.wait_event(B.RESP_STOPPED, timeout=10)
            # VS64 refreshes its whole memory cache on each stop; this reads
            # VERA data/registers while the fill has port 0 pointed at VRAM.
            body = struct.pack("<BHHBH", 0, 0x0000, 0xffff, 0, 0)
            c.recv_response(c.send(B.CMD_MEMORY_GET, body), timeout=10)
        except Exception as e:
            print(f"FAIL crashed/hung after {i} steps at last pc=${last:04x}: {e!r}")
            sys.exit(1)
    print(f"DONE stepped {args.steps} times through x16_vera_fill, last pc=${last:04x}, no crash")


if __name__ == "__main__":
    main()
