"""Survey a PE image: sections and their entropy, imports, TLS callbacks, debug
directory. Answers the first question about a binary: is the code on disk real,
or is it packed and only decrypted at runtime?

    python tools/pe_survey.py <exe>
"""
import math
import sys
from collections import Counter

import pefile


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in Counter(data).values())


def main() -> None:
    path = sys.argv[1]
    pe = pefile.PE(path, fast_load=False)
    oh = pe.OPTIONAL_HEADER
    print(f"machine 0x{pe.FILE_HEADER.Machine:x}  image base 0x{oh.ImageBase:x}  "
          f"entry rva 0x{oh.AddressOfEntryPoint:x}  size of image 0x{oh.SizeOfImage:x}")
    print(f"timestamp {pe.FILE_HEADER.TimeDateStamp}  subsystem {oh.Subsystem}  "
          f"dll chars 0x{oh.DllCharacteristics:x}")

    entry = oh.AddressOfEntryPoint
    print("\nsections:")
    print(f"  {'name':<10}{'rva':>12}{'vsize':>12}{'rawsize':>12}  flags     entropy  entry?")
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        data = s.get_data()
        has_entry = s.VirtualAddress <= entry < s.VirtualAddress + s.Misc_VirtualSize
        print(f"  {name:<10}{s.VirtualAddress:>#12x}{s.Misc_VirtualSize:>#12x}{s.SizeOfRawData:>#12x}"
              f"  {s.Characteristics:08x}  {entropy(data):5.2f}   {'<-' if has_entry else ''}")

    print("\nimports:")
    total = 0
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        names = [i.name.decode() if i.name else f"#{i.ordinal}" for i in d.imports]
        total += len(names)
        print(f"  {d.dll.decode():<32} {len(names):>4}  {', '.join(names[:6])}{' ...' if len(names) > 6 else ''}")
    print(f"  total {total}")

    tls = getattr(pe, "DIRECTORY_ENTRY_TLS", None)
    if tls:
        cb = tls.struct.AddressOfCallBacks
        print(f"\nTLS directory present, callbacks array at 0x{cb:x}")
        if cb:
            rva = cb - oh.ImageBase
            for i in range(16):
                v = pe.get_qword_at_rva(rva + 8 * i)
                if not v:
                    break
                print(f"  callback {i}: 0x{v:x}")

    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        e = d.entry
        info = ""
        if hasattr(d, "entry") and getattr(d.entry, "PdbFileName", None):
            info = d.entry.PdbFileName.rstrip(b"\0").decode(errors="replace")
        print(f"\ndebug entry type {d.struct.Type}  {info}")

    exc = getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", None)
    if exc is not None:
        print(f"\n.pdata runtime functions: {len(exc)}")

    sec = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_SECURITY"]]
    print(f"authenticode signature: {'yes' if sec.Size else 'no'} ({sec.Size} bytes)")


if __name__ == "__main__":
    main()
