"""Reads the FrostForge worldserver's packet log (PKT 3.1, PacketLogFile in worldserver.conf).

    python tools/pktdump.py <World.pkt>                 opcode counts per direction
    python tools/pktdump.py <World.pkt> --list          every packet: time, direction, opcode, size
    python tools/pktdump.py <World.pkt> --op 0x2DD2     the packets of one opcode, with a hex dump

Opcode names come from docs/opcodes.json (the client's own table), so the names are the
client's view of the number, not the server's.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load_names():
    names = {"CMSG": {}, "SMSG": {}}
    path = os.path.join(ROOT, "docs", "opcodes.json")
    try:
        data = json.load(open(path, encoding="utf-8"))
    except OSError:
        return names
    stack = [data]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            if node.get("direction") in names and isinstance(node.get("opcode"), int) and node.get("name"):
                names[node["direction"]].setdefault(node["opcode"], node["name"])
            stack.extend(node.values())
        elif isinstance(node, list):
            stack.extend(node)
    return names


def read_packets(path):
    blob = open(path, "rb").read()
    if blob[:3] != b"PKT":
        sys.exit("not a PKT file")
    # LogHeader, packed: sig[3] u16 u8 u32 locale[4] key[40] u32 u32 u32
    (optional,) = struct.unpack_from("<I", blob, 3 + 2 + 1 + 4 + 4 + 40 + 4 + 4)
    pos = 3 + 2 + 1 + 4 + 4 + 40 + 4 + 4 + 4 + optional
    while pos + 20 <= len(blob):
        direction, _conn, ticks, optsize, length = struct.unpack_from("<5I", blob, pos)
        pos += 20 + optsize
        if pos + 4 > len(blob):
            break
        (opcode,) = struct.unpack_from("<I", blob, pos)
        body = blob[pos + 4:pos + length]
        pos += length
        yield ("CMSG" if direction == 0x47534D43 else "SMSG"), ticks, opcode, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--op")
    args = ap.parse_args()
    names = load_names()
    packets = list(read_packets(args.file))
    if not packets:
        print("no packets")
        return
    start = packets[0][1]
    if args.op:
        want = int(args.op, 0)
        for side, ticks, opcode, body in packets:
            if opcode == want:
                print(f"{(ticks - start) / 1000:9.3f} {side} 0x{opcode:04X} {len(body)} bytes: {body[:64].hex(' ')}")
        return
    if args.list:
        for side, ticks, opcode, body in packets:
            name = names[side].get(opcode, "?")
            print(f"{(ticks - start) / 1000:9.3f} {side} 0x{opcode:04X} {name} {len(body)}")
        return
    counts = {}
    for side, _ticks, opcode, _body in packets:
        counts[(side, opcode)] = counts.get((side, opcode), 0) + 1
    for (side, opcode), n in sorted(counts.items(), key=lambda kv: (kv[0][0], -kv[1])):
        print(f"{side} 0x{opcode:04X} {names[side].get(opcode, '?'):48} {n}")


if __name__ == "__main__":
    main()
