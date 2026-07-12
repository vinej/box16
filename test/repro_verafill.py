#!/usr/bin/env python3
"""Verify line-granular stepping tames the x16_vera_fill __asm{} loop.

The fill loop is one source line (vera.c:99) spanning a 256-iteration
sta/dex/bne loop -- hundreds of instructions. With line stepping, ONE client
ADVANCE must step the whole loop internally (no per-instruction round-trips)
and stop at the next source line, for both F10 (step over) and F11 (step
into). Previously VS64 single-stepped every instruction, which hung/crashed.
"""
import argparse
import struct
import sys
import time
import binmon_test as B


def stop_at(c, addr, prg):
    B.attach_handshake(c, prg)
    bp = struct.pack("<HHBBBBB", addr, addr, 1, 1, 4, 0, 0)
    _, err, rbody, _ = c.recv_response(c.send(B.CMD_CHECKPOINT_SET, bp))
    (num,) = struct.unpack_from("<I", rbody, 0)
    c.events.clear()
    c.recv_response(c.send(B.CMD_EXIT))
    _, ebody = c.wait_event(B.RESP_STOPPED, timeout=40)
    (pc,) = struct.unpack_from("<H", ebody, 0)
    B.check(pc == addr, f"stopped at ${pc:04x}")
    c.recv_response(c.send(B.CMD_CHECKPOINT_DELETE, struct.pack("<I", num)))
    return pc


def one_line_step(c, step_over):
    """Issue a single client ADVANCE and return (pc, seconds)."""
    c.events.clear()
    t0 = time.monotonic()
    c.recv_response(c.send(B.CMD_ADVANCE, struct.pack("<BH", 1 if step_over else 0, 1)), timeout=15)
    _, rbody = c.wait_event(B.RESP_REGISTER_INFO, timeout=15)
    pc = B.parse_registers(rbody)[3]
    c.wait_event(B.RESP_STOPPED, timeout=15)
    return pc, time.monotonic() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6502)
    ap.add_argument("--lbl", default="build/bounce.lbl")
    ap.add_argument("--prg", default="build/bounce.prg")
    args = ap.parse_args()

    fill = B.find_label(args.lbl, "x16_vera_fill")  # $0aae, first line of the loop

    for over in (True, False):
        name = "F10" if over else "F11"
        c = B.Client(args.host, args.port)
        stop_at(c, fill, args.prg)
        # One step must clear the whole loop line in one client round-trip.
        pc, dt = one_line_step(c, over)
        B.check(pc > fill, f"{name}: one ADVANCE stepped past the loop to ${pc:04x} in {dt*1000:.0f} ms")
        B.check(dt < 5.0, f"{name}: completed quickly ({dt*1000:.0f} ms), no per-instruction hang")
        # A couple more steps should exit the function back to the caller.
        steps = 1
        while pc >= fill and steps < 10:
            pc, dt = one_line_step(c, over)
            steps += 1
        B.check(pc < fill or pc > 0x0ac9, f"{name}: exited x16_vera_fill after {steps} line steps (at ${pc:04x})")

    print("DONE __asm{} fill loop steps as whole lines, no hang")


if __name__ == "__main__":
    main()
