"""Each import slot's parameters, from the Windows SDK headers: data/import_protos.json.

The import table is stripped, so tools/iat_dump.py recovers the slots' names from a live process
(data/imports.json), but nothing says what each API takes. protos.py needs that: a wrapper that
only hands its registers on to an API (`OsMutexInit`, FUN_140262450: `call [InitializeSRWLock]`)
reads none of them itself, came out taking nothing, and its callers stopped passing the lock
(FUN_1401ca520 initialized one over its own code, the first crash of 2026-10-17).

This writes a C++ file that, for every imported name the SDK declares (`__if_exists`), prints
the parameters' kinds and sizes off the declaration's own type, compiles it with MSVC and runs
it. Names the SDK does not declare (none, as of writing) are left out and reported. Run it again
only when data/imports.json changes; the result is committed.

    python tools/import_protos.py
"""
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

HEADERS = ["winsock2.h", "ws2tcpip.h", "windows.h", "bcrypt.h", "wincrypt.h", "cfgmgr32.h", "setupapi.h",
           "shellapi.h", "shlobj.h", "objbase.h", "combaseapi.h", "roapi.h", "winstring.h", "mmsystem.h",
           "mmreg.h", "msacm.h", "imm.h", "winhttp.h", "commctrl.h", "iphlpapi.h", "dwmapi.h", "winver.h", "dbghelp.h",
           "sddl.h", "tlhelp32.h", "evntprov.h", "psapi.h", "werapi.h"]
LIBS = ["ws2_32", "bcrypt", "crypt32", "cfgmgr32", "setupapi", "shell32", "ole32", "winmm", "msacm32", "imm32",
        "winhttp", "comctl32", "iphlpapi", "dwmapi", "version", "user32", "gdi32", "advapi32", "runtimeobject", "kernel32", "wer"]

PROBE = r"""
#include <cstdio>
#include <type_traits>
template <class T> void Arg() {
    std::printf(" %c%zu", std::is_floating_point<T>::value ? 'f' : 'i', sizeof(T));
}
template <class R, class... A> void Probe(const char* name, R (*)(A...)) {
    std::printf("%s", name);
    int unused[] = {0, (Arg<A>(), 0)...};
    (void)unused;
    std::printf("\n");
}
template <class R, class... A> void Probe(const char* name, R (*)(A..., ...)) {
    std::printf("%s", name);
    int unused[] = {0, (Arg<A>(), 0)...};
    (void)unused;
    std::printf(" ...\n");
}
"""


def vcvars() -> str:
    path = subprocess.run([VSWHERE, "-latest", "-property", "installationPath"], capture_output=True,
                          text=True, check=True).stdout.strip()
    return os.path.join(path, r"VC\Auxiliary\Build\vcvars64.bat")


def names() -> list[str]:
    imports = json.load(open(os.path.join(ROOT, "data", "imports.json")))["imports"]
    out = set()
    for info in imports.values():
        n = info.get("imported_name")
        if n is None:
            continue
        # folded identical code (htons/ntohs): the candidates share a prototype, as ApplyFacts takes it
        out.add(n if isinstance(n, str) else n[0])
    return sorted(out)


def build(work: str, wanted: list[str]) -> tuple[subprocess.CompletedProcess, str]:
    src = os.path.join(work, "probe.cpp")
    body = ["#define WIN32_LEAN_AND_MEAN", "#define NOMINMAX"]
    body += [f"#include <{h}>" for h in HEADERS]
    body.append(PROBE)
    body.append("int main() {")
    for n in wanted:
        body.append(f"    __if_exists(::{n}) {{ Probe(\"{n}\", &::{n}); }}")
        body.append(f"    __if_not_exists(::{n}) {{ std::printf(\"? {n}\\n\"); }}")
    body.append("    return 0;\n}")
    with open(src, "w") as f:
        f.write("\n".join(body) + "\n")
    exe = os.path.join(work, "probe.exe")
    cmd = f'"{vcvars()}" >nul && cl /nologo /EHsc /std:c++17 /Fe"{exe}" /Fo"{work}\\\\" "{src}" ' + \
          " ".join(f"{l}.lib" for l in LIBS)
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=work), exe


def main() -> None:
    wanted = names()
    dropped = []
    with tempfile.TemporaryDirectory() as work:
        for _ in range(8):
            done, exe = build(work, wanted)
            if done.returncode == 0:
                break
            # a name the probe cannot take the address of as one function (an overload set, a
            # macro to an inline): leave it out, by the source line the compiler names
            lines = open(os.path.join(work, "probe.cpp")).read().splitlines()
            bad = set()
            for m in re.finditer(r"probe\.cpp\((\d+)\)", done.stdout):
                hit = re.search(r'Probe\("(\w+)"', lines[int(m.group(1)) - 1])
                if hit:
                    bad.add(hit.group(1))
            if not bad:
                sys.exit(done.stdout[-4000:])
            dropped += sorted(bad)
            wanted = [n for n in wanted if n not in bad]
        else:
            sys.exit("probe did not build")
        run = subprocess.run([exe], capture_output=True, text=True, check=True).stdout
    protos, missing = {}, []
    for line in run.splitlines():
        parts = line.split()
        if parts[0] == "?":
            missing.append(parts[1])
            continue
        variadic = parts[-1] == "..."
        params = [[p[0], int(p[1:])] for p in parts[1:len(parts) - variadic]]
        protos[parts[0]] = {"params": params, **({"variadic": True} if variadic else {})}
    out = os.path.join(ROOT, "data", "import_protos.json")
    with open(out, "w") as f:
        json.dump({"protos": protos}, f, indent=1, sort_keys=True)
    print(f"{len(protos)} prototypes -> {out}; not declared {len(missing)} {missing}; "
          f"not probeable {len(dropped)} {dropped}")


if __name__ == "__main__":
    main()
