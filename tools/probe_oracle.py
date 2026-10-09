"""Compare small pure ports with the reference under emulation (tools/oracle.py):
SStrHashHT (0x140252fc0), OsPathNormalize (0x1402620d0), UnicodeConvertUtf16ToUtf8
(0x14024d4f0), SStrTokenize (0x140254100).

    python tools/probe_oracle.py
"""
import struct
import subprocess
import sys

from oracle import Oracle
from wowbin import ROOT

EXE = ROOT / "build" / "cmake" / "bin" / "Release" / "ProbeTest.exe"


def ours(*args: str) -> str:
    return subprocess.run([str(EXE), *args], capture_output=True, text=True, encoding="latin-1").stdout.strip()


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    o = Oracle()
    diffs = 0
    total = 0

    def check(label: str, reference: str, mine: str) -> None:
        nonlocal diffs, total
        total += 1
        if reference != mine:
            diffs += 1
            print(f"DIFF  {label:40} reference={reference!r} frostlite={mine!r}")

    for s in ["", "a", "set", "SET", "cvarlist", "Interface/Icons", "interface\\icons", "WTF\\Config.wtf",
              "café", "ÿœё", "日本語", "a" * 300, "x" * 1100]:
        h = o.call(0x140252FC0, [o.cstr(s)]) & 0xFFFFFFFF
        check(f"hash {s[:30]!r}", f"{h:08x}", ours("hash", s.encode("utf-8").hex() or "-"))

    for p in ["C:/a/b/../c", "c:\\WoW\\_classic_\\.\\Data", "/x//y/./z/", "a/b/../../c", "../a", "C:\\", "//server/share",
              "A:/x/..", "./.", ""]:
        out = o.alloc(b"\0" * 0x104)
        o.call(0x1402620D0, [out, 0x104, o.cstr(p)])
        check(f"normalize {p!r}", o.read_cstr(out).decode("latin-1"), ours("normalize", p))

    for units in [[0x41, 0x42], [0xE9], [0x20AC], [0xD83D, 0xDE00], [0xD800, 0x41], [0x41] * 20, [0x20AC] * 6]:
        source = o.alloc(b"".join(struct.pack("<H", u) for u in units) + b"\0\0")
        out = o.alloc(b"\xCC" * 64)
        counts = o.alloc(b"\xFF" * 16)
        result = o.call(0x14024D4F0, [out, 16, source, 0x7FFFFFFF, counts, counts + 8]) & 0xFFFFFFFF
        result = result - (1 << 32) if result >= 1 << 31 else result
        written = struct.unpack("<i", bytes(o.uc.mem_read(counts, 4)))[0]
        used = struct.unpack("<i", bytes(o.uc.mem_read(counts + 8, 4)))[0]
        ref = f"{bytes(o.uc.mem_read(out, 16)).hex()} {result} {written} {used}"
        check(f"utf8 {units[:4]}", ref, ours("utf8", "".join(f"{u:04x}" for u in units)))

    for s in ['a b c', '  lead', 'one,two;three', '"quoted arg" x', 'a"b c"d', '""', 'x "unterminated', '-flag=1 @file']:
        p = o.alloc(struct.pack("<Q", o.cstr(s)))
        token = o.alloc(b"\0" * 32)
        quoted = o.alloc(b"\0" * 4)
        ws = o.cstr(' ,;"\t\n\r\x1a')
        parts = []
        for _ in range(8):
            o.uc.mem_write(quoted, struct.pack("<i", -1))
            o.call(0x140254100, [p, token, 32, ws, quoted])
            parts.append(f"[{o.read_cstr(token).decode('latin-1')}]{struct.unpack('<i', bytes(o.uc.mem_read(quoted, 4)))[0]}")
            cur = struct.unpack("<Q", bytes(o.uc.mem_read(p, 8)))[0]
            if o.uc.mem_read(cur, 1)[0] == 0:
                break
        check(f"tokenize {s!r}", " ".join(parts), ours("tokenize", s))

    for s in ["0", "1", "-1", "0.5", "1e39", "-1e39", "1e-39", "1e-50", "3.4028235e38", "inf", "nan", "0x10", " 2.5",
              "abc", "1,5", "1.17549435e-38", "-0.0", "123456789.123", "1e-38", "7e-46"]:
        bits = o.call(0x140254020, [o.cstr(s)], ret="xmm0") & 0xFFFFFFFF
        check(f"float {s!r}", f"{bits:08x}", ours("float", s.encode().hex()))

    for s in ["", "a", "SET", "cvarList", "gxWindow", "caf\u00e9", "\u00ff", "[]^_`{"]:
        h = o.call(0x140216FC0, [o.cstr(s), 0xCBF29CE484222325])
        check(f"fnv {s!r}", f"{h:016x}", ours("fnv", s.encode("utf-8").hex() or "-"))

    pairs = [("abc", "ABC"), ("abc", "abd"), ("RunOnce1.wtf", "runonce2.WTF"), ("", "a"), ("a", ""),
             ("é", "E"), ("é", "e"), ("Æ", "AE"), ("ae", "æ"), ("ß", "ss"),
             ("Straße", "STRASSE"), ("œuvre", "OEUVRE"), ("ÿ", "Y"), ("ё", "Е"),
             ("я", "Я"), ("\U0001F600", "\U0001F601"), ("xÆy", "xAEz"), ("Æ", "A"),
             ("A", "Æ"), ("Ø", "O"), ("zz", "z"), ("cafés", "cafes")]
    raw_pairs = [(b"\xc3", b"\xc3"), (b"a\xc3", b"a"), (b"\xfc\x80\x80\x80\x80\x81", b"\xfc\x80\x80\x80\x80\x80"),
                 (b"\xfc\x80", b"\xfc\x81"), (b"\x80", b"\x81"), (b"\xe0\x80", b"\xe0\x81")]
    for a, b in [(x.encode(), y.encode()) for x, y in pairs] + raw_pairs:
        r = o.call(0x1402534D0, [o.cstr(a), o.cstr(b), 0x7FFFFFFF]) & 0xFFFFFFFF
        r = r - (1 << 32) if r >= 1 << 31 else r
        check(f"cmp {a!r} {b!r}", str(r), ours("cmp", (a.hex() or "") + "-" + b.hex()))

    codes = ["enUS", "deDE", "xxXX", "gaIE", "mnMN", "euES", "lbLU", "esAR", "enGB", "frCA", "ptPT", "zhHK",
             "nlNL", "xxAU", "xxBR", "xxCA", "xxCN", "xxIT", "xxKR", "xxMX", "xxNZ", "xxPT", "xxRU", "xxTW",
             "xxUS", "xxDE", "xxES", "xxFR", "xxGB", "xxCX", "xxIX", "itIT", "koKR", "ruRU"]
    for code in codes:
        value = int.from_bytes(code.encode(), "little")
        for fallback in (0, 7):
            locale = o.call(0x1405D57C0, [value, fallback]) & 0xFFFFFFFF
            to_code = o.call(0x1405D5B70, [value % 12]) & 0xFFFFFFFF
            check(f"locale {code} {fallback}", f"{locale} {to_code}", ours("locale", f"{value:x}", str(fallback)))

    for name in ["US", "us", "eu", "CN", "TR", "QA", "AT", "zz", "", "BL", "XX"]:
        r = o.call(0x1405D5D90, [o.cstr(name)]) & 0xFFFFFFFF
        check(f"region {name!r}", str(r), ours("region", name.encode().hex() or "-"))

    for text in ["127.0.0.1", "1.2.3.4", "255.255.255.255", "256.1.1.1", "1.2.3", "1.2.3.4.5", "1..2.3", "a.b.c.d",
                 "", "01.2.3.4", "1.2.3.4 "]:
        out = o.alloc(b"\0" * 8)
        ok = o.call(0x141238FF0, [o.cstr(text), out]) & 0xFF
        addr = int.from_bytes(bytes(o.uc.mem_read(out, 4)), "little") if ok else 0
        check(f"ipv4 {text!r}", f"{1 if ok else 0} {addr:08x}", ours("ipv4", text.encode().hex() or "-"))

    for text in ["::1", "::", "fe80::1", "2001:db8::ff00:42:8329", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8:9",
                 "::ffff:192.168.1.1", "1::2::3", "12345::", "abcd:", ":1", "g::1", "::1.2.3.4"]:
        out = o.alloc(b"\0" * 16)
        ok = o.call(0x141239090, [o.cstr(text), out]) & 0xFF
        addr = bytes(o.uc.mem_read(out, 16)).hex() if ok else "00" * 16
        check(f"ipv6 {text!r}", f"{1 if ok else 0} {addr}", ours("ipv6", text.encode().hex() or "-"))

    print(f"{total} checks, {diffs} differ from the reference")


if __name__ == "__main__":
    main()
