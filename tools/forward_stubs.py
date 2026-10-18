"""Forward the port's FROST_UNIMPLEMENTED stubs to the reference's own code, translated.

A stub that a run reached (its name in Logs/FrostLite.log as `[unimplemented] <name>`) and that
carries a `// ref: FUN_x` tag is rewritten in place as a forwarder:

    // translated: FUN_x
    void Sub1412345() {
        __try { FUN_x(); } __except (GenFault(0x..., GetExceptionInformation())) {}
    }

tools/translate.py takes every `// translated:` forwarder as a root, so the next translation
brings in FUN_x and what it calls. Only free functions whose parameters and result are plain
values are forwarded: a member function or a pointer parameter would hand a port object to code
that reads it at the reference's offsets, which is only right where the layouts were checked
(translate.py --risks). The network services stay offline stubs (docs/stubs.md), and the crash,
error and allocator hooks stay the port's.

    python tools/forward_stubs.py --log client/_classic_/Logs/FrostLite.log [--apply]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEEP_FILES = ("src/services/", "src/client/CrashReport.cpp", "src/storm/ErrorReport.cpp", "src/storm/Memory.cpp",
              "src/client/Telemetry", "src/glue/ScriptAPIStubs.cpp")
VALUE_TYPE = re.compile(r"^(?:const\s+)?(?:bool|int|unsigned|unsigned int|int(?:8|16|32|64)_t|uint(?:8|16|32|64)_t|"
                        r"size_t|DWORD|BOOL|float|double|long|unsigned long)$")
# a pointer to plain data (a string, a float buffer): no object, so no layout to agree on. Not
# `uint8_t *`, which the port also uses for an object it has not typed (`DownloadQueueDestroy(
# uint8_t* queue)`), so a layout after all
DATA_POINTER = re.compile(r"^(?:const\s+)?(?:char|float|double)\s*\*$")
# teardown: the port leaves through its own quit path, and the shutdown faults came from code
# walking its containers at exit (2026-10-17)
TEARDOWN = re.compile(r"Shutdown|Destroy|Release|Clear|AddRef")
# stubs kept the port's by what they do: the WowConnection network layer (offline), the
# reference's unhandled-exception filter, the allocator hooks
KEEP_NAMES = {"Sub1415002E0", "OsInitializeExceptionHandling", "Sub14165C0E0",
              # the final shutdown: the port tears its subsystems down itself
              "Sub140216880"}
# the stubs held back on purpose, by the registry of them (docs/stubs.md: the network services
# until they are ported against our own server), and the families it covers by name, whose
# members also live outside their own files (`TelemetryEncryptionHandler` in Startup.cpp,
# forwarded with --all, ran the telemetry key setup and CAS init failed, 2026-10-18)
DELIBERATE = set(re.findall(r"\b0x(1[0-9a-f]{8})\b", (ROOT / "docs/stubs.md").read_text(encoding="utf-8")))
OFFLINE_NAMES = re.compile(r"^(?:Telemetry|Agent|WowDownload|ClientServices|BattleNet|RealmConnection|NetClient)")
DEFAULT_RESULT ={"void": "", "bool": "false", "float": "0.0f", "double": "0.0"}


def stub_at(lines: list[str], i: int):
    """The stub defined after the tag on line i: (first, last line, signature) or None."""
    j = i + 1
    while j < len(lines) and lines[j].strip().startswith("//"):
        j += 1
    sig = lines[j] if j < len(lines) else ""
    m = re.match(r"^(?:static\s+)?([\w:<>\s\*&]+?)\s+([\w:~]+)\(([^)]*)\)\s*(const)?\s*\{\s*$", sig)
    if not m:
        return None
    depth, k = 0, j
    body = []
    while k < len(lines):
        depth += lines[k].count("{") - lines[k].count("}")
        body.append(lines[k])
        if depth == 0:
            break
        k += 1
    inner = [l.strip() for l in body[1:-1] if l.strip() and not l.strip().startswith("//")]
    if "FROST_UNIMPLEMENTED();" not in inner:
        return None
    rest = [l for l in inner if l != "FROST_UNIMPLEMENTED();" and not re.fullmatch(r"\(void\)\w+;", l)
            and not re.fullmatch(r"return [^;]*;", l)]
    if rest:
        return None   # a stub with behaviour of its own around the marker: left for a hand port
    return j, k, m.group(1).strip(), m.group(2), m.group(3).strip()


def plan(log: Path | None):
    # (every tagged stub with --all, not only those a run reached)
    hits = None if log is None else set(re.findall(r"\[unimplemented\] (.+)$", log.read_text(encoding="utf-8", errors="replace"), re.M))
    out, skipped = [], []
    translated = set(json.loads((ROOT / "src/gen/manifest.json").read_text(encoding="utf-8"))["translated"])
    # each translated definition's parameter count (translated.h declares them unprototyped)
    arity = {}
    for f in (ROOT / "src/gen").glob("translated_*.c"):
        for d in re.finditer(r"^/\* ref: FUN_([0-9a-f]{9}) \*/\n[^\n]*?\bFUN_\1\(([^\n]*)\)\s*$",
                             f.read_text(encoding="utf-8"), re.M):
            arity[d.group(1)] = len([p for p in d.group(2).split(",") if p.strip() and p.strip() != "void"])
    for path in (ROOT / "src").rglob("*.cpp"):
        rel = path.relative_to(ROOT).as_posix()
        if rel.startswith("src/gen/"):
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            m = re.match(r"^// ref: FUN_([0-9a-f]{9})\s*$", line)
            if not m:
                continue
            s = stub_at(lines, i)
            if s is None:
                continue
            first, last, ret, name, params = s
            if hits is not None and name not in hits and name.split("::")[-1] not in hits:
                continue
            why = None
            if rel.startswith(KEEP_FILES) or name in KEEP_NAMES or m.group(1) in DELIBERATE \
                    or OFFLINE_NAMES.search(name):
                why = "kept offline / the port's own"
            elif TEARDOWN.search(name):
                why = "teardown (the quit path is the port's)"
            elif "::" in name:
                why = "a member function (a port object at the reference's offsets)"
            elif ret not in DEFAULT_RESULT and not VALUE_TYPE.match(ret):
                why = f"returns {ret}"
            else:
                for p in [p.strip() for p in params.split(",") if p.strip() and p.strip() != "void"]:
                    ptype = re.sub(r"\s*\w+$", "", p) if " " in p else p
                    if not VALUE_TYPE.match(ptype) and not DATA_POINTER.match(ptype.strip()):
                        why = f"takes {ptype}"
                        break
            if why is None and m.group(1) not in translated:
                # (the forwarder calls the translated definition: none, and it does not link)
                why = "not translated"
            if why is None:
                # a stub whose signature is not the reference's ("Signature not yet known", a
                # callback declared `()`): forwarded, the translated body reads the parameters it
                # was never given (15 of the first 77, 2026-10-18)
                n = len([p for p in params.split(",") if p.strip() and p.strip() != "void"])
                if arity.get(m.group(1)) != n:
                    why = f"signature differs (the port's {n} parameters, the reference's {arity.get(m.group(1))})"
            entry = {"file": rel, "line": i + 1, "addr": m.group(1), "name": name, "ret": ret, "params": params,
                     "first": first, "last": last}
            (skipped if why else out).append(entry | ({"why": why} if why else {}))
    return out, skipped


def forwarder(e: dict) -> list[str]:
    params = [p.strip() for p in e["params"].split(",") if p.strip() and p.strip() != "void"]
    # an unnamed parameter (`void FfxInitialize(bool)`) gets a name to pass along, not its type
    # as one (`(unsigned long long)bool`, C2062)
    params = [p if re.search(r"[\w\]]\s+\**\s*\w+$|\*\s*\w+$", p) and not VALUE_TYPE.match(p) and not DATA_POINTER.match(p)
              else f"{p} p{k}" for k, p in enumerate(params)]
    e = e | {"params": ", ".join(params) if params else e["params"]}
    names = [re.search(r"(\w+)$", p).group(1) for p in params]
    types = [re.sub(r"\s*\w+$", "", p) for p in params]
    # the translated definitions take a float promoted to double (their callers are unprototyped)
    ctypes = ["double" if t.strip().endswith(("float", "double")) else "unsigned long long" for t in types]
    args = ", ".join(f"({c}){n}" for c, n in zip(ctypes, names))
    decl = f"extern \"C\" {'double' if e['ret'] in ('float', 'double') else 'unsigned long long'} FUN_{e['addr']}({', '.join(ctypes)});"
    call = f"FUN_{e['addr']}({args})"
    ret = e["ret"]
    if ret == "void":
        do, fail = f"{call};", ""
    elif ret == "bool":
        do, fail = f"return ({call} & 0xff) != 0;", "return false;"
    else:
        do, fail = f"return static_cast<{ret}>({call});", f"return {DEFAULT_RESULT.get(ret, f'static_cast<{ret}>(0)')};"
    return [decl,
            f"// translated: FUN_{e['addr']}",
            f"{ret} {e['name']}({e['params']}) {{",
            "    __try {",
            f"        {do}",
            f"    }} __except (GenFault(0x{e['addr']}ull, GetExceptionInformation())) {{",
            *([f"        {fail}"] if fail else []),
            "    }",
            "}"]


def apply(entries: list[dict]):
    by_file = {}
    for e in entries:
        by_file.setdefault(e["file"], []).append(e)
    for rel, es in by_file.items():
        path = ROOT / rel
        lines = path.read_text(encoding="utf-8").splitlines()
        for e in sorted(es, key=lambda e: -e["line"]):
            tag = e["line"] - 1
            lines[tag:e["last"] + 1] = forwarder(e)
        if not any("int GenFault(" in l for l in lines):
            # the guard every forwarder runs under (src/gen/RefImage.cpp), after the includes
            last = 0
            for k, l in enumerate(lines):
                s = l.strip()
                if s.startswith("#include"):
                    last = k
                elif s and not s.startswith(("#", "//", "/*", "*")) and k > last:
                    break
            lines[last + 1:last + 1] = ["#include <windows.h>", "",
                                        "extern \"C\" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);"]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


FALLBACK_START = "// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----"
FALLBACK_END = "// ---- end of link fallbacks ----"


def link_fallbacks() -> int:
    """At the end of each port file that calls translated code: the alternates the linker takes
    when nothing defines the translated function or GenFault (src/util/Unimplemented.cpp).

    A forwarder names them directly, and a test executable that links the library without
    src/gen did not link: ObjectAllocTest pulled os's Path.cpp in for its own reasons and failed on
    FUN_140214150 and GenFault (LNK2019, 2026-10-18). The client links src/gen, which defines both,
    and an alternate name is only used for a symbol nothing defines."""
    changed = 0
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        if path.relative_to(ROOT).as_posix().startswith("src/gen/"):
            continue
        text = path.read_text(encoding="utf-8")
        if FALLBACK_START in text:
            text = text[:text.index(FALLBACK_START)].rstrip("\n") + "\n" + text[text.index(FALLBACK_END) + len(FALLBACK_END):].lstrip("\n")
        code = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//"))
        funs = sorted(set(re.findall(r"\b(FUN_[0-9a-f]{9})\b", code)))
        if not funs and "GenFault(" not in code:
            if text != path.read_text(encoding="utf-8"):
                path.write_text(text, encoding="utf-8", newline="\n")
                changed += 1
            continue
        block = [FALLBACK_START, '#pragma comment(linker, "/include:GenForwardMissing")']
        if "GenFault(" in code:
            block.append('#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")')
        block += [f'#pragma comment(linker, "/alternatename:{f}=GenForwardMissing")' for f in funs]
        block.append(FALLBACK_END)
        new = text.rstrip("\n") + "\n\n" + "\n".join(block) + "\n"
        if new != path.read_text(encoding="utf-8"):
            path.write_text(new, encoding="utf-8", newline="\n")
            changed += 1
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log")
    ap.add_argument("--all", action="store_true", help="every tagged stub, reached or not")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--link-fallbacks", action="store_true",
                    help="only refresh the link fallbacks of the files that call translated code")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.link_fallbacks:
        print(f"link fallbacks refreshed in {link_fallbacks()} files")
        return
    if not args.all and not args.log:
        ap.error("--log or --all")
    out, skipped = plan(None if args.all else Path(args.log))
    print(f"{len(out)} stubs to forward, {len(skipped)} left as they are")
    reasons = {}
    for s in skipped:
        reasons.setdefault(s["why"].split(" (")[0] if not s["why"].startswith(("takes", "returns")) else s["why"].split()[0], []).append(s["name"])
    for k, v in sorted(reasons.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):3} {k}: {', '.join(v[:6])}{' ...' if len(v) > 6 else ''}")
    if args.apply:
        apply(out)
        link_fallbacks()
        print("applied")


if __name__ == "__main__":
    main()
