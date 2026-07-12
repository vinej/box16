#!/usr/bin/env python3
"""Focused check: does ADVANCE_INSTRUCTIONS with step_over=1 actually step
OVER a JSR (skip the called subroutine), vs step_over=0 stepping into it?

main() at $0880 begins with `JSR x16_screen_cls` ($0947). Step-over from
$0880 must land at $0883 (the instruction after the JSR), NOT $0947.
"""
import argparse
import struct
import sys
import binmon_test as B


def one(c, addr, prg, step_over):
    B.attach_handshake(c, prg)
    bp = struct.pack("<HHBBBBB", addr, addr, 1, 1, 4, 0, 0)
    _, err, rbody, _ = c.recv_response(c.send(B.CMD_CHECKPOINT_SET, bp))
    (num,) = struct.unpack_from("<I", rbody, 0)
    c.recv_response(c.send(B.CMD_EXIT))
    err, ebody = c.wait_event(B.RESP_STOPPED, timeout=40)
    (pc,) = struct.unpack_from("<H", ebody, 0)
    assert pc == addr, f"expected stop at ${addr:04x}, got ${pc:04x}"
    # delete the checkpoint so the step is not itself caught by it
    c.recv_response(c.send(B.CMD_CHECKPOINT_DELETE, struct.pack("<I", num)))
    # ADVANCE with the given step_over flag, one instruction
    c.recv_response(c.send(B.CMD_ADVANCE, struct.pack("<BH", 1 if step_over else 0, 1)))
    c.wait_event(B.RESP_RESUMED, timeout=5)
    err, ebody = c.wait_event(B.RESP_STOPPED, timeout=15)
    (pc2,) = struct.unpack_from("<H", ebody, 0)
    return pc2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6502)
    ap.add_argument("--lbl", default="build/bounce.lbl")
    ap.add_argument("--prg", default="build/bounce.prg")
    args = ap.parse_args()

    main_addr = B.find_label(args.lbl, "main")
    cls_addr = B.find_label(args.lbl, "x16_screen_cls")

    c = B.Client(args.host, args.port)
    into = one(c, main_addr, args.prg, step_over=False)
    B.check(into == cls_addr, f"step_over=0 steps INTO x16_screen_cls (${into:04x} == ${cls_addr:04x})")

    over = one(c, main_addr, args.prg, step_over=True)
    B.check(over != cls_addr, f"step_over=1 does NOT enter x16_screen_cls (landed ${over:04x})")
    B.check(over > main_addr and over < cls_addr, f"step_over=1 lands after the JSR in main (${over:04x})")
    print("DONE step-over behavior verified")


if __name__ == "__main__":
    main()
