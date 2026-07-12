#!/usr/bin/env python3
"""Milestone tests for Box16's VICE binary monitor server.

Usage: the emulator must already be running with the monitor enabled, e.g.

  box16.exe -ignore_ini -binarymonitor -rom <rom.bin> -prg bounce.prg -run -sym bounce.lbl

  python binmon_test.py [--host 127.0.0.1] [--port 6502] [--lbl path\to\bounce.lbl]

Runs M1 (transport/info), M2 (memory/registers) and, when a label file is
given, M3 (checkpoints/stepping/events) assertions. Exit code 0 = all passed.
"""

import argparse
import socket
import struct
import sys
import time

STX = 0x02
API = 0x02
EVENT_ID = 0xFFFFFFFF

CMD_MEMORY_GET = 0x01
CMD_MEMORY_SET = 0x02
CMD_CHECKPOINT_GET = 0x11
CMD_CHECKPOINT_SET = 0x12
CMD_CHECKPOINT_DELETE = 0x13
CMD_CHECKPOINT_LIST = 0x14
CMD_CHECKPOINT_TOGGLE = 0x15
CMD_CONDITION_SET = 0x22
CMD_REGISTERS_GET = 0x31
CMD_REGISTERS_SET = 0x32
CMD_ADVANCE = 0x71
CMD_UNTIL_RETURN = 0x73
CMD_RESET = 0xCC
CMD_AUTOSTART = 0xDD
CMD_PING = 0x81
CMD_BANKS_AVAILABLE = 0x82
CMD_REGISTERS_AVAILABLE = 0x83
CMD_VICE_INFO = 0x85
CMD_EXIT = 0xAA

RESP_CHECKPOINT_INFO = 0x11
RESP_STOPPED = 0x62
RESP_RESUMED = 0x63


class Client:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.2)
        self.buffer = b""
        self.next_id = 1
        self.events = []

    def send(self, cmd, body=b""):
        req_id = self.next_id
        self.next_id += 1
        frame = struct.pack("<BBIIB", STX, API, len(body), req_id, cmd) + body
        self.sock.sendall(frame)
        return req_id

    def _pump(self, deadline):
        while len(self.buffer) < 12:
            if time.monotonic() > deadline:
                raise TimeoutError("timed out waiting for a frame")
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buffer += chunk
        stx, api, body_len = struct.unpack_from("<BBI", self.buffer, 0)
        assert stx == STX and api == API, f"bad frame header {self.buffer[:2].hex()}"
        total = 12 + body_len
        while len(self.buffer) < total:
            if time.monotonic() > deadline:
                raise TimeoutError("timed out inside a frame")
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buffer += chunk
        rtype, err, rid = struct.unpack_from("<BBI", self.buffer, 6)
        body = self.buffer[12:total]
        self.buffer = self.buffer[total:]
        return rtype, err, rid, body

    def recv_response(self, req_id, timeout=5.0, collect=None):
        """Wait for the response to req_id; queue events seen on the way."""
        deadline = time.monotonic() + timeout
        collected = []
        while True:
            rtype, err, rid, body = self._pump(deadline)
            if rid == req_id:
                if collect is not None and rtype == collect:
                    collected.append((rtype, err, body))
                    continue
                return rtype, err, body, collected
            if rid == EVENT_ID:
                self.events.append((rtype, err, body))
            # responses to other ids are dropped

    def wait_event(self, rtype, timeout=30.0):
        deadline = time.monotonic() + timeout
        while True:
            for i, (etype, err, body) in enumerate(self.events):
                if etype == rtype:
                    del self.events[i]
                    return err, body
            etype, err, rid, body = self._pump(deadline)
            if rid == EVENT_ID:
                self.events.append((etype, err, body))


def check(cond, name):
    status = "PASS" if cond else "FAIL"
    print(f"{status} {name}")
    if not cond:
        sys.exit(1)


def parse_registers(body):
    (count,) = struct.unpack_from("<H", body, 0)
    pos, regs = 2, {}
    for _ in range(count):
        size = body[pos]
        reg_id = body[pos + 1]
        (value,) = struct.unpack_from("<H", body, pos + 2)
        regs[reg_id] = value
        pos += 1 + size
    return regs


def milestone1(c):
    rtype, err, body, _ = c.recv_response(c.send(CMD_PING))
    check(rtype == CMD_PING and err == 0, "M1 ping")

    rtype, err, body, _ = c.recv_response(c.send(CMD_VICE_INFO))
    check(err == 0 and body[0] == 4 and body[1] == 3, "M1 vice_info reports as VICE 3.x")

    rtype, err, body, _ = c.recv_response(c.send(CMD_REGISTERS_AVAILABLE, b"\x00"))
    (count,) = struct.unpack_from("<H", body, 0)
    names, pos = [], 2
    for _ in range(count):
        size = body[pos]
        name_len = body[pos + 3]
        names.append(body[pos + 4 : pos + 4 + name_len].decode())
        pos += 1 + size
    check(err == 0 and names == ["a", "x", "y", "pc", "sp", "fl"], f"M1 registers_available {names}")

    rtype, err, body, _ = c.recv_response(c.send(CMD_BANKS_AVAILABLE))
    (count,) = struct.unpack_from("<H", body, 0)
    banks, pos = [], 2
    for _ in range(count):
        size = body[pos]
        name_len = body[pos + 3]
        banks.append(body[pos + 4 : pos + 4 + name_len].decode())
        pos += 1 + size
    check(err == 0 and banks == ["cpu", "ram", "rom", "io"], f"M1 banks_available {banks}")

    rtype, err, body, _ = c.recv_response(c.send(0xF0))
    check(err == 0x83, "M1 unknown command -> invalid-command error")


def milestone2(c):
    payload = bytes([0xDE, 0xAD, 0xBE, 0xEF])
    start, end = 0x0400, 0x0403  # scratch RAM, unused by KERNAL/bounce
    body = struct.pack("<BHHBH", 0, start, end, 0, 0) + payload
    rtype, err, _, _ = c.recv_response(c.send(CMD_MEMORY_SET, body))
    check(rtype == CMD_MEMORY_SET and err == 0, "M2 memory_set")

    body = struct.pack("<BHHBH", 0, start, end, 0, 0)
    rtype, err, rbody, _ = c.recv_response(c.send(CMD_MEMORY_GET, body))
    (length,) = struct.unpack_from("<H", rbody, 0)
    check(err == 0 and length == 4 and rbody[2:6] == payload, "M2 memory_get reads back what was written")

    body = struct.pack("<BHHBH", 0, start, end, 5, 0)
    rtype, err, rbody, _ = c.recv_response(c.send(CMD_MEMORY_GET, body))
    check(err == 0x02, "M2 memory_get bad memspace -> error")

    rtype, err, rbody, _ = c.recv_response(c.send(CMD_REGISTERS_GET, b"\x00"))
    regs = parse_registers(rbody)
    check(err == 0 and set(regs.keys()) == {0, 1, 2, 3, 4, 5}, f"M2 registers_get {regs}")


def milestone3(c, bp_addr):
    # The exact 9-byte exec checkpoint body VS64 sends.
    body = struct.pack("<HHBBBBB", bp_addr, bp_addr, 1, 1, 4, 0, 0)
    rtype, err, rbody, _ = c.recv_response(c.send(CMD_CHECKPOINT_SET, body))
    check(rtype == RESP_CHECKPOINT_INFO and err == 0, "M3 checkpoint_set responds with CHECKPOINT_INFO")
    (cp_num,) = struct.unpack_from("<I", rbody, 0)
    (cp_start,) = struct.unpack_from("<H", rbody, 5)
    check(cp_start == bp_addr, f"M3 checkpoint start echoed (${cp_start:04x})")

    err, ebody = c.wait_event(RESP_CHECKPOINT_INFO, timeout=30)
    (hit_num,) = struct.unpack_from("<I", ebody, 0)
    check(hit_num == cp_num and ebody[4] == 1, f"M3 checkpoint {hit_num} hit event")
    err, ebody = c.wait_event(RESP_STOPPED, timeout=5)
    (stop_pc,) = struct.unpack_from("<H", ebody, 0)
    check(stop_pc == bp_addr, f"M3 stopped at ${stop_pc:04x} == breakpoint address")

    rtype, err, rbody, _ = c.recv_response(c.send(CMD_REGISTERS_GET, b"\x00"))
    regs = parse_registers(rbody)
    check(regs[3] == bp_addr, f"M3 registers pc == ${regs[3]:04x}")

    # step one instruction: expect RESUMED then STOPPED at a new pc
    rtype, err, _, _ = c.recv_response(c.send(CMD_ADVANCE, struct.pack("<BH", 0, 1)))
    check(err == 0, "M3 advance_instructions accepted")
    c.wait_event(RESP_RESUMED, timeout=5)
    err, ebody = c.wait_event(RESP_STOPPED, timeout=5)
    (step_pc,) = struct.unpack_from("<H", ebody, 0)
    check(step_pc != bp_addr, f"M3 stepped to ${step_pc:04x}")

    # step over and step out should also stop again
    rtype, err, _, _ = c.recv_response(c.send(CMD_ADVANCE, struct.pack("<BH", 1, 1)))
    c.wait_event(RESP_RESUMED, timeout=5)
    c.wait_event(RESP_STOPPED, timeout=10)
    check(True, "M3 step over")

    rtype, err, _, _ = c.recv_response(c.send(CMD_UNTIL_RETURN))
    c.wait_event(RESP_RESUMED, timeout=5)
    c.wait_event(RESP_STOPPED, timeout=10)
    check(True, "M3 execute until return")

    # checkpoint list should report exactly one checkpoint
    req = c.send(CMD_CHECKPOINT_LIST)
    rtype, err, rbody, infos = c.recv_response(req, collect=RESP_CHECKPOINT_INFO)
    (count,) = struct.unpack_from("<I", rbody, 0)
    check(count == 1 and len(infos) == 1, "M3 checkpoint_list")

    # resume; it must hit again, then delete and confirm free running
    rtype, err, _, _ = c.recv_response(c.send(CMD_EXIT))
    check(err == 0, "M3 exit resumes")
    c.wait_event(RESP_RESUMED, timeout=5)
    c.wait_event(RESP_STOPPED, timeout=30)
    check(True, "M3 breakpoint hits again after resume")

    rtype, err, _, _ = c.recv_response(c.send(CMD_CHECKPOINT_DELETE, struct.pack("<I", cp_num)))
    check(err == 0, "M3 checkpoint_delete")
    rtype, err, _, _ = c.recv_response(c.send(CMD_EXIT))
    c.wait_event(RESP_RESUMED, timeout=5)
    time.sleep(1.0)
    try:
        c.wait_event(RESP_STOPPED, timeout=1)
        stopped_again = True
    except TimeoutError:
        stopped_again = False
    check(not stopped_again, "M3 no further stops after delete")


def milestone4(c, bp_addr, prg_path):
    """Reproduce VS64's attach handshake: RESET then AUTOSTART then break.

    This is the sequence VS64 sends on 'vice attach' -- the machine must
    come back up running the program, not sitting at the BASIC prompt, so
    the breakpoint has something to hit.
    """
    rtype, err, _, _ = c.recv_response(c.send(CMD_RESET, b"\x00"))
    check(rtype == CMD_RESET and err == 0, "M4 reset accepted")

    name = prg_path.encode()
    body = bytes([1, 0, 0, len(name)]) + name  # run flag, index, len, filename
    rtype, err, _, _ = c.recv_response(c.send(CMD_AUTOSTART, body))
    check(err == 0, "M4 autostart accepted")

    bp = struct.pack("<HHBBBBB", bp_addr, bp_addr, 1, 1, 4, 0, 0)
    rtype, err, _, _ = c.recv_response(c.send(CMD_CHECKPOINT_SET, bp))
    check(rtype == RESP_CHECKPOINT_INFO and err == 0, "M4 checkpoint set after reset")

    # After reset the program reloads and runs; the breakpoint must fire,
    # proving the machine did not just idle at the BASIC prompt.
    err, ebody = c.wait_event(RESP_STOPPED, timeout=40)
    (stop_pc,) = struct.unpack_from("<H", ebody, 0)
    check(stop_pc == bp_addr, f"M4 program restarted and hit breakpoint at ${stop_pc:04x}")


def find_label(lbl_path, name):
    with open(lbl_path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "al" and parts[2] == "." + name:
                return int(parts[1], 16)
    raise SystemExit(f"label {name} not found in {lbl_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6502)
    ap.add_argument("--lbl", help="VICE label file; enables the M3 checkpoint tests")
    ap.add_argument("--label", default="move_sprite", help="label to break on for M3/M4")
    ap.add_argument("--prg", default="build/bounce.prg", help="prg path for the M4 autostart test")
    args = ap.parse_args()

    c = Client(args.host, args.port)
    milestone1(c)
    milestone2(c)
    if args.lbl:
        addr = find_label(args.lbl, args.label)
        milestone3(c, addr)
        milestone4(c, addr, args.prg)
    print("DONE all milestones passed")


if __name__ == "__main__":
    main()
