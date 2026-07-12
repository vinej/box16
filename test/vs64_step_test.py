#!/usr/bin/env python3
"""Replicate VS64's C-line step-over algorithm end-to-end to prove a step
completes (does not run away).

VS64 steps like this: remember the source line of the start PC; issue
ADVANCE_INSTRUCTIONS(step_over, 1); on each stop read the cached PC from the
REGISTER_INFO event; if that PC maps to a source line AND is outside the
start line's address range, the step is done; otherwise advance again.

Before the register-info-on-stop fix, the cached PC never updated, so this
loop never terminated. This test asserts a step-over of main()'s first line
completes within a small bound and lands on the next C line.
"""
import argparse
import json
import struct
import sys
import binmon_test as B


def build_address_map(dbj_path):
    """addr -> (source, line), covering [start, end) of every line entry,
    exactly like VS64's _addressMap."""
    d = json.load(open(dbj_path))
    amap = {}
    for f in d.get("functions", []):
        for c in f.get("lines", []):
            for a in range(c["start"], c["end"]):
                amap[a] = (c["source"], c["line"])
    return amap


def step_one_c_line(c, amap, step_over, max_steps=200):
    """One VS64-style source-line step. Returns (start_line, end_line, count)."""
    start_pc = current_pc(c)
    start_info = amap.get(start_pc)
    # start line's contiguous address range
    lo = hi = start_pc
    while amap.get(lo - 1) == start_info:
        lo -= 1
    while amap.get(hi + 1) == start_info:
        hi += 1

    count = 0
    while count < max_steps:
        c.events.clear()
        c.recv_response(c.send(B.CMD_ADVANCE, struct.pack("<BH", 1 if step_over else 0, 1)))
        _, rbody = c.wait_event(B.RESP_REGISTER_INFO, timeout=10)
        pc = B.parse_registers(rbody)[3]
        c.wait_event(B.RESP_STOPPED, timeout=10)
        count += 1
        info = amap.get(pc)
        # VS64 completes when the PC maps to a line and has left the start range
        if info is not None and (pc < lo or pc > hi):
            return start_info, info, count
    return start_info, None, count


def current_pc(c):
    _, err, rbody, _ = c.recv_response(c.send(B.CMD_REGISTERS_GET, b"\x00"))
    return B.parse_registers(rbody)[3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6502)
    ap.add_argument("--lbl", default="build/bounce.lbl")
    ap.add_argument("--dbj", default="build/bounce.dbj")
    ap.add_argument("--prg", default="build/bounce.prg")
    args = ap.parse_args()

    amap = build_address_map(args.dbj)
    main_addr = B.find_label(args.lbl, "main")

    c = B.Client(args.host, args.port)
    B.attach_handshake(c, args.prg)
    bp = struct.pack("<HHBBBBB", main_addr, main_addr, 1, 1, 4, 0, 0)
    _, err, rbody, _ = c.recv_response(c.send(B.CMD_CHECKPOINT_SET, bp))
    (num,) = struct.unpack_from("<I", rbody, 0)
    c.events.clear()
    c.recv_response(c.send(B.CMD_EXIT))
    _, ebody = c.wait_event(B.RESP_STOPPED, timeout=40)
    (pc,) = struct.unpack_from("<H", ebody, 0)
    B.check(pc == main_addr, f"stopped at main ${pc:04x}")
    c.recv_response(c.send(B.CMD_CHECKPOINT_DELETE, struct.pack("<I", num)))

    # Step over several C lines, VS64-style, and confirm each completes.
    prev = amap.get(main_addr)
    B.check(prev is not None, f"main maps to a source line {prev}")
    for i in range(5):
        start_info, end_info, count = step_one_c_line(c, amap, step_over=True)
        B.check(end_info is not None, f"step {i+1}: completed in {count} advance(s), not runaway")
        B.check(end_info != start_info, f"step {i+1}: {start_info[0].split('/')[-1]}:{start_info[1]} -> {end_info[0].split('/')[-1]}:{end_info[1]} ({count} instr)")
        prev = end_info

    print("DONE VS64-style stepping completes and advances C lines")


if __name__ == "__main__":
    main()
