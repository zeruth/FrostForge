"""Ghidra's 16-byte values in C: `undefined1 auVar3 [16]` as gen_v16 (src/gen/Ghidra.h), a union
of __m128i and its bytes, which C can assign, pass and return where it cannot an array.

Ghidra spells an SSE register as a 16-byte array and the instructions on it as integer operators
on it (`auVar2 = ZEXT816(0) << 0x40 | ZEXT816(x)`, `pand` as `&`) or as pseudo-functions named
after the instruction (`pshufb(auVar47,_DAT_1426348d0)`). As C the first is an assignment to an
array and an operator on one (C2106, C2296: 30 functions demoted on 2026-10-16), and the second
called nothing. Each statement that involves a 16-byte value is parsed here, its terms typed
(16 bytes or a scalar), and the 16-byte operations written as Ghidra.h's helpers and the
pseudo-ops as their intrinsics; a statement without one is left exactly as it was.

    rewrite(body) -> body        (raises V16Unsupported for what it cannot take)
"""
from __future__ import annotations

import re


class V16Unsupported(Exception):
    pass


# ---- tokens --------------------------------------------------------------------------------

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<str>"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')
  | (?P<num>0x[0-9a-fA-F]+[uUlL]*|\d+\.\d*(?:[eE][+-]?\d+)?[fF]?|\d+[eE][+-]?\d+[fF]?|\d+[uUlL]*)
  | (?P<id>[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*)
  | (?P<op><<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!<>=?:,.()\[\]{};])
""", re.X)


def tokens(s: str) -> list[str]:
    out, i = [], 0
    while i < len(s):
        m = TOKEN.match(s, i)
        if not m:
            raise V16Unsupported(f"token at {s[i:i + 20]!r}")
        if m.lastgroup != "ws":
            out.append(m.group(0))
        i = m.end()
    return out


# ---- the expression tree -------------------------------------------------------------------

BINARY = {
    "*": 13, "/": 13, "%": 13, "+": 12, "-": 12, "<<": 11, ">>": 11,
    "<": 10, ">": 10, "<=": 10, ">=": 10, "==": 9, "!=": 9, "&": 8, "^": 7, "|": 6,
    "&&": 5, "||": 4,
}
TYPE_WORDS = {"undefined", "undefined1", "undefined2", "undefined4", "undefined8", "uint", "int", "char", "uchar",
              "ushort", "short", "long", "ulong", "longlong", "ulonglong", "float", "double", "bool", "byte",
              "code", "void", "unsigned", "signed", "float10", "dword", "qword", "word", "uint3", "int3",
              "undefined3", "undefined5", "undefined6", "undefined7", "uint5", "uint6", "uint7", "int5", "int6",
              "int7", "wchar_t", "wchar16", "sbyte", "gen_v16"}


class Node:
    __slots__ = ("kind", "val", "kids", "ty")

    def __init__(self, kind, val=None, kids=None):
        self.kind, self.val, self.kids, self.ty = kind, val, kids or [], None


class Parser:
    def __init__(self, toks: list[str]):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if want is not None and tok != want:
            raise V16Unsupported(f"expected {want!r}, got {tok!r}")
        self.i += 1
        return tok

    def cast_ahead(self) -> int | None:
        """The length of a `(type)` at the cursor, or None."""
        if self.peek() != "(":
            return None
        j, depth = self.i + 1, 0
        # (and Ghidra's sized ones, `unkuint10`, `undefined10`, `int3`)
        sized = re.compile(r"(?:unk(?:u?int|byte)|undefined|u?int|float)\d+")
        if self.t[j] not in TYPE_WORDS and not sized.fullmatch(self.t[j] or ""):
            return None
        while j < len(self.t):
            tok = self.t[j]
            if tok == "(":
                depth += 1
            elif tok == ")":
                if depth == 0:
                    return j + 1 - self.i
                depth -= 1
            elif not (tok in TYPE_WORDS or sized.fullmatch(tok) or tok in ("*", "[", "]") or re.fullmatch(r"\d+", tok)):
                return None
            j += 1
        return None

    def comma(self):
        """`a, b`: Ghidra's comma expressions (`while (x = f(), x != 0)`)."""
        e = self.expr(0)
        while self.peek() == ",":
            self.take()
            e = Node("comma", None, [e, self.expr(0)])
        return e

    def expr(self, minp=0):
        """Precedence climbing: assignment (1, right to left) < ?: (2) < the binary operators."""
        left = self.unary()
        while True:
            op = self.peek()
            if op in ("=", "|=", "&=", "^=", "+=", "-=") and minp <= 1:
                self.take()
                left = Node("assign", op, [left, self.expr(1)])
                continue
            if op == "?" and minp <= 2:
                self.take()
                a = self.expr(0)
                self.take(":")
                left = Node("cond", None, [left, a, self.expr(2)])
                continue
            p = BINARY.get(op)
            if p is None or p < minp:
                return left
            self.take()
            left = Node("bin", op, [left, self.expr(p + 1)])

    def unary(self):
        tok = self.peek()
        n = self.cast_ahead()
        if n is not None:
            ty = " ".join(self.t[self.i + 1:self.i + n - 1])
            self.i += n
            return Node("cast", ty, [self.unary()])
        if tok in ("-", "~", "!", "*", "&"):
            self.take()
            return Node("unary", tok, [self.unary()])
        return self.postfix(self.primary())

    def primary(self):
        tok = self.take()
        if tok == "(":
            e = self.comma()
            self.take(")")
            return Node("paren", None, [e])
        if tok is None:
            raise V16Unsupported("expression ends early")
        if re.fullmatch(r"[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*", tok):
            return Node("id", tok)
        if tok[0].isdigit() or tok[0] in "\"'":
            return Node("num", tok)
        raise V16Unsupported(f"unexpected {tok!r}")

    def postfix(self, n):
        while True:
            tok = self.peek()
            if tok == "(":
                self.take()
                args = []
                if self.peek() != ")":
                    while True:
                        args.append(self.expr(0))
                        if self.peek() == ",":
                            self.take()
                            continue
                        break
                self.take(")")
                n = Node("call", None, [n] + args)
            elif tok == "[":
                self.take()
                i = self.expr(0)
                self.take("]")
                n = Node("index", None, [n, i])
            elif tok in (".", "->"):
                self.take()
                n = Node("member", tok + self.take(), [n])
            else:
                return n


# ---- types ---------------------------------------------------------------------------------

V = "v16"
PIECE = re.compile(r"\._(\d+)_(\d+)_$")
# CONCATab with a + b == 16, ZEXTa16 / SEXTa16, SUB16a
CONCAT = re.compile(r"^CONCAT(\d)(\d+)$|^CONCAT(\d\d)(\d)$")
EXT16 = re.compile(r"^([ZS])EXT(\d{1,2})16$")
SUB16 = re.compile(r"^SUB16(\d{1,2})$")
# the 9- to 15-byte integers Ghidra makes of SSE byte work (`unkuint9 Var35 = CONCAT81(...)`,
# 102 functions): 16-byte values whose bytes above their width stay zero
DECLWIDE_INT = re.compile(r"^([ \t]*)unk(?:u?int|byte)(9|1[0-5]) (\w+);$", re.M)


def concat_sizes(name: str) -> tuple[int, int] | None:
    """CONCATab's (a, b), the high and low parts' sizes, when together they are wider than a
    scalar and fit 16 bytes (`CONCAT81`, `CONCAT19`, `CONCAT124`); None otherwise."""
    m = re.fullmatch(r"CONCAT(\d+)", name)
    if not m:
        return None
    d = m.group(1)
    for k in range(1, len(d)):
        a, b = int(d[:k]), int(d[k:])
        if d[k] != "0" and 1 <= a <= 16 and 1 <= b <= 16 and 8 < a + b <= 16:
            return a, b
    return None


def sub_sizes(name: str) -> tuple[int, int] | None:
    """SUBab's (a, b): b bytes out of an a-byte value, a wider than a scalar (`SUB148`, `SUB1610`)."""
    m = re.fullmatch(r"SUB(\d+)", name)
    if not m:
        return None
    d = m.group(1)
    for a in (16, 15, 14, 13, 12, 11, 10, 9):
        s = str(a)
        if d.startswith(s) and d[len(s):] and d[len(s)] != "0" and 1 <= int(d[len(s):]) < a:
            return a, int(d[len(s):])
    return None

# pseudo-op -> (C expression template over the arguments as __m128i a0, a1, ...; result kind)
#   kinds: "i" __m128i, "f" __m128, "d" __m128d, "s" a scalar (int)
# Ghidra passes the destination's old value first where the instruction is dest-op-src
PSEUDO = {
    "pshufb": ("_mm_shuffle_epi8({0}, {1})", "i"),
    "paddusw": ("_mm_adds_epu16({0}, {1})", "i"),
    "paddsw": ("_mm_adds_epi16({0}, {1})", "i"),
    "paddusb": ("_mm_adds_epu8({0}, {1})", "i"),
    "pmulhuw": ("_mm_mulhi_epu16({0}, {1})", "i"),
    "pmulld": ("_mm_mullo_epi32({0}, {1})", "i"),
    "phaddw": ("_mm_hadd_epi16({0}, {1})", "i"),
    "pmovzxdq": ("_mm_cvtepu32_epi64({1})", "i"),
    "pmovzxbd": ("_mm_cvtepu8_epi32({1})", "i"),
    "pmovzxwd": ("_mm_cvtepu16_epi32({1})", "i"),
    "vpsubd_avx": ("_mm_sub_epi32({0}, {1})", "i"),
    "vpsrld_avx": ("_mm_srli_epi32({0}, {1:imm})", "i"),
    "vcvtdq2ps_avx": ("_mm_cvtepi32_ps({0})", "f"),
    "vpslld_avx": ("_mm_slli_epi32({0}, {1:imm})", "i"),
    "pmovsxwd": ("_mm_cvtepi16_epi32({1})", "i"),
    # the codecs' (the --all run linked none of these, 2026-10-17: ~600 functions)
    "pshufhw": ("_mm_shufflehi_epi16({1}, {2:imm})", "i"),
    "packssdw": ("_mm_packs_epi32({0}, {1})", "i"),
    "packsswb": ("_mm_packs_epi16({0}, {1})", "i"),
    "pmaddwd": ("_mm_madd_epi16({0}, {1})", "i"),
    "pmaddubsw": ("_mm_maddubs_epi16({0}, {1})", "i"),
    "pmulhrsw": ("_mm_mulhrs_epi16({0}, {1})", "i"),
    "psubsw": ("_mm_subs_epi16({0}, {1})", "i"),
    "psubsb": ("_mm_subs_epi8({0}, {1})", "i"),
    "paddsb": ("_mm_adds_epi8({0}, {1})", "i"),
    "psubusb": ("_mm_subs_epu8({0}, {1})", "i"),
    "phaddsw": ("_mm_hadds_epi16({0}, {1})", "i"),
    "pmovsxdq": ("_mm_cvtepi32_epi64({1})", "i"),
    "pblendvb": ("_mm_blendv_epi8({0}, {1}, {2})", "i"),
    "sqrtps": ("_mm_sqrt_ps(_mm_castsi128_ps({1}))", "f"),
    "sqrtpd": ("_mm_sqrt_pd(_mm_castsi128_pd({1}))", "d"),
    "rcpss": ("_mm_move_ss(_mm_castsi128_ps({0}), _mm_rcp_ss(_mm_castsi128_ps({1})))", "f"),
    "blendvpd": ("_mm_blendv_pd(_mm_castsi128_pd({0}), _mm_castsi128_pd({1}), _mm_castsi128_pd({2}))", "d"),
    "vpackuswb_avx": ("_mm_packus_epi16({0}, {1})", "i"),
    "vpacksswb_avx": ("_mm_packs_epi16({0}, {1})", "i"),
    "vpavgb_avx": ("_mm_avg_epu8({0}, {1})", "i"),
    "vpcmpgtb_avx": ("_mm_cmpgt_epi8({0}, {1})", "i"),
    "vphaddsw_avx": ("_mm_hadds_epi16({0}, {1})", "i"),
    "vpmaddubsw_avx": ("_mm_maddubs_epi16({0}, {1})", "i"),
    "vpaddsw_avx": ("_mm_adds_epi16({0}, {1})", "i"),
    "vpshufb_avx": ("_mm_shuffle_epi8({0}, {1})", "i"),
    "vpsraw_avx": ("_mm_srai_epi16({0}, {1:imm})", "i"),
    "vpunpcklbw_avx": ("_mm_unpacklo_epi8({0}, {1})", "i"),
    "vpunpckhdq_avx": ("_mm_unpackhi_epi32({0}, {1})", "i"),
    "vpmovsxwd_avx": ("_mm_cvtepi16_epi32({0})", "i"),
    "vpmovzxbd_avx": ("_mm_cvtepu8_epi32({0})", "i"),
    "vcvtph2ps_f16c": ("_mm_cvtph_ps({0})", "f"),
    "vcmpsd_avx": ("_mm_cmp_sd(_mm_castsi128_pd({0}), _mm_castsi128_pd({1}), {2:imm})", "d"),
    # AVX-512VL on 128 bits: each 64-bit lane multiplied, kept low (Ghidra.h, in plain C)
    "vpmullq_avx512vl": ("gen_mullo_epi64({0}, {1})", "i"),
    "lddqu": ("({1})", "i"),
    "psllw": ("_mm_slli_epi16({0}, {1:imm})", "i"),
    "psraw": ("_mm_srai_epi16({0}, {1:imm})", "i"),
    "pshuflw": ("_mm_shufflelo_epi16({1}, {2:imm})", "i"),
    "pblendw": ("_mm_blend_epi16({0}, {1}, {2:imm})", "i"),
    "pcmpistri": ("_mm_cmpistri({0}, {1}, {2:imm})", "s"),
    "aesenc": ("_mm_aesenc_si128({0}, {1})", "i"),
    "aesenclast": ("_mm_aesenclast_si128({0}, {1})", "i"),
    "aesdec": ("_mm_aesdec_si128({0}, {1})", "i"),
    "aesdeclast": ("_mm_aesdeclast_si128({0}, {1})", "i"),
    "aesimc": ("_mm_aesimc_si128({0})", "i"),
    "aeskeygenassist": ("_mm_aeskeygenassist_si128({0}, {1:imm})", "i"),
    "sha256rnds2_sha": ("_mm_sha256rnds2_epu32({0}, {1}, {2})", "i"),
    "sha256msg1_sha": ("_mm_sha256msg1_epu32({0}, {1})", "i"),
    "sha256msg2_sha": ("_mm_sha256msg2_epu32({0}, {1})", "i"),
    "vpsrldq_avx": ("_mm_srli_si128({0}, {1:imm})", "i"),
    "vpshufd_avx": ("_mm_shuffle_epi32({0}, {1:imm})", "i"),
    "vpunpckldq_avx": ("_mm_unpacklo_epi32({0}, {1})", "i"),
    "vpand_avx": ("_mm_and_si128({0}, {1})", "i"),
    "vpor_avx": ("_mm_or_si128({0}, {1})", "i"),
    "vpsrlq_avx": ("_mm_srli_epi64({0}, {1:imm})", "i"),
    "vpsllq_avx": ("_mm_slli_epi64({0}, {1:imm})", "i"),
    "vpsrad_avx": ("_mm_srai_epi32({0}, {1:imm})", "i"),
    "vpsubq_avx": ("_mm_sub_epi64({0}, {1})", "i"),
    "vpaddq_avx": ("_mm_add_epi64({0}, {1})", "i"),
    "vpmovsxdq_avx": ("_mm_cvtepi32_epi64({0})", "i"),
    "vcvtdq2pd_avx": ("_mm_cvtepi32_pd({0})", "d"),
    "vcvtps2pd_avx": ("_mm_cvtps_pd(_mm_castsi128_ps({0}))", "d"),
    "vcvtpd2dq_avx": ("_mm_cvtpd_epi32(_mm_castsi128_pd({0}))", "i"),
    "vshufps_avx": ("_mm_shuffle_ps(_mm_castsi128_ps({0}), _mm_castsi128_ps({1}), {2:imm})", "f"),
    "vunpcklpd_avx": ("_mm_unpacklo_pd(_mm_castsi128_pd({0}), _mm_castsi128_pd({1}))", "d"),
    "rcpps": ("_mm_rcp_ps(_mm_castsi128_ps({1}))", "f"),
    "rsqrtps": ("_mm_rsqrt_ps(_mm_castsi128_ps({1}))", "f"),
    "rsqrtss": ("_mm_move_ss(_mm_castsi128_ps({0}), _mm_rsqrt_ss(_mm_castsi128_ps({1})))", "f"),
    "minps": ("_mm_min_ps(_mm_castsi128_ps({0}), _mm_castsi128_ps({1}))", "f"),
    "maxps": ("_mm_max_ps(_mm_castsi128_ps({0}), _mm_castsi128_ps({1}))", "f"),
    "divps": ("_mm_div_ps(_mm_castsi128_ps({0}), _mm_castsi128_ps({1}))", "f"),
    "blendvps": ("_mm_blendv_ps(_mm_castsi128_ps({0}), _mm_castsi128_ps({1}), _mm_castsi128_ps({2}))", "f"),
}
# pseudo-ops on 32 or 64 bytes (AVX2 / AVX-512 paths): no gen_v16 holds them
WIDE_PSEUDO = re.compile(r"_avx2$|_avx512(?!vl$)|^vp\w+_avx512(?!vl$)")
# the ones Ghidra.h has as functions over a 16-byte value (movmskps(dest, v) and friends)
SCALAR_PSEUDO = {"movmskps": "movmskps", "movmskpd": "movmskpd"}


def is_place16(n: Node, typer) -> bool:
    """The 16 bytes a pointer to 16 bytes points at (`*pauVar`, `pauVar[i]`)."""
    while n.kind == "paren":
        n = n.kids[0]
    if n.kind == "unary" and n.val == "*":
        return typer.ty(n.kids[0]) == "p16"
    if n.kind == "index":
        return typer.ty(n.kids[0]) == "p16"
    return False


class Typer:
    def __init__(self, v16: set[str], v16ptr: set[str], wide_globals: set[str]):
        self.v16, self.v16ptr, self.globals = v16, v16ptr, wide_globals
        self.v16ptrs: set[str] = set()
        self.short: dict[str, int] = {}   # byte arrays of 9..15, by name -> size
        self.vfuncs: set[str] = set()     # callees returning 16 bytes
        self.vparams: dict[str, set[int]] = {}   # callees taking 16 bytes, by argument position
        self.pointers: set[str] = set()           # variables declared as pointers

    def ty(self, n: Node) -> str:
        if n.ty is not None:
            return n.ty
        k = n.kind
        t = "s"
        if k == "id":
            t = V if n.val in self.v16 or n.val in self.globals or n.val in self.short else \
                ("p16" if n.val in self.v16ptr else "ap16" if n.val in self.v16ptrs else "s")
        elif k == "paren":
            t = self.ty(n.kids[0])
        elif k == "member":
            m = PIECE.search(n.val)
            t = V if m and int(m.group(2)) > 8 else "s"
            self.ty(n.kids[0])
        elif k == "cast":
            self.ty(n.kids[0])
            t = "p16" if re.sub(r"\s", "", n.val) in ("undefined1(*)[16]", "undefined(*)[16]") else \
                (V if re.sub(r"\s", "", n.val) in ("undefined1[16]",) or WIDE_INT_CAST.fullmatch(n.val.strip()) else "s")
        elif k == "unary":
            sub = self.ty(n.kids[0])
            if n.val == "*":
                t = V if sub == "p16" else ("p16" if sub == "ap16" else "s")
            elif n.val == "&":
                t = "s"
            elif n.val == "!":
                t = "s"
            else:
                t = sub if sub == V else "s"
        elif k == "index":
            base = self.ty(n.kids[0])
            self.ty(n.kids[1])
            t = V if base == "p16" else ("p16" if base == "ap16" else "s")
        elif k == "bin":
            a, b = self.ty(n.kids[0]), self.ty(n.kids[1])
            if n.val in "+-" and (is_place16(n.kids[0], self) or is_place16(n.kids[1], self)):
                # `*pauVar26 + uVar32`: the 16 bytes at a pointer decay to their first byte's
                # address in Ghidra's C, so this is byte arithmetic, not a 128-bit sum
                n.ty = "bytes"
                return n.ty
            if n.val in ("==", "!=", "<", ">", "<=", ">=", "&&", "||"):
                t = "s"
            elif n.val in ("<<", ">>"):
                t = a if a == V else "s"
            else:
                t = V if V in (a, b) else ("p16" if "p16" in (a, b) and n.val in "+-" else "s")
        elif k == "cond":
            ts = [self.ty(x) for x in n.kids]
            t = V if V in ts[1:] else "s"
        elif k == "comma":
            t = [self.ty(x) for x in n.kids][-1]
        elif k == "assign":
            a, _b = self.ty(n.kids[0]), self.ty(n.kids[1])
            t = a
        elif k == "call":
            for x in n.kids[1:]:
                self.ty(x)
            name = n.kids[0].val if n.kids[0].kind == "id" else None
            t = "s"
            if name:
                if CONCAT.match(name):
                    t = V if concat_sizes(name) else "s"
                elif EXT16.match(name):
                    t = V
                elif sub_sizes(name) and sub_sizes(name)[1] > 8:
                    t = V
                elif name in PSEUDO:
                    t = V if PSEUDO[name][1] != "s" else "s"
                elif name in self.vfuncs:
                    t = V
        n.ty = t
        return t


# ---- emission ------------------------------------------------------------------------------

# a reference global by Ghidra's name for it, ending in its address
GLOBAL_NAME = re.compile(r"_?(?:PTR_)*(?:DAT|UNK|s|u|ws)_\w*?_?(1[0-9a-f]{8})")

V16_BIN = {"&": "gen_v16_and", "|": "gen_v16_or", "^": "gen_v16_xor", "+": "gen_v16_add", "-": "gen_v16_sub",
           "*": "gen_v16_mul", "/": "gen_v16_div", "%": "gen_v16_mod", "<<": "gen_v16_shl", ">>": "gen_v16_shr"}
V16_CMP = {"==": "gen_v16_eq", "!=": "!gen_v16_eq", "<": "gen_v16_lt", ">": "gen_v16_gt", "<=": "!gen_v16_gt",
           ">=": "!gen_v16_lt"}


class Emitter:
    def __init__(self, typer: Typer):
        self.T = typer

    def v(self, n: Node) -> str:
        """n as a gen_v16 value."""
        return self.e(n) if self.T.ty(n) == V else f"gen_v16_u({self.e(n)})"

    def m(self, n: Node) -> str:
        """n as an __m128i (for an intrinsic)."""
        if self.T.ty(n) == V:
            return f"({self.e(n)}).m"
        g = GLOBAL_NAME.fullmatch(n.val) if n.kind == "id" else None
        if g:
            # a global handed to a vector instruction whole: the 16 bytes at its address, whatever
            # Ghidra named it (`_s_VUVUVU_142634c00`, a constant it took for a string)
            return f"(*(gen_v16 *)REFP(0x{g.group(1)})).m"
        return f"gen_v16_u({self.e(n)}).m"

    def place(self, n: Node) -> str:
        """n as the object a piece of it is at: a short byte array is itself (`auVar9._2_10_ =`
        on an `undefined1 auVar9 [12]`, FUN_14030acc0), not the value e() loads from it."""
        if n.kind == "id" and n.val in self.T.short:
            return n.val
        return self.e(n)

    def e(self, n: Node) -> str:
        T, k = self.T, n.kind
        if k == "id" and n.val in T.short:
            return f"gen_v16_load({n.val}, {T.short[n.val]})"
        if k == "id" and n.val in T.globals:
            # a 16-byte global (the data export types it as bytes): its 16 bytes in place
            g = GLOBAL_NAME.fullmatch(n.val)
            return f"(*(gen_v16 *)REFP(0x{g.group(1)}))" if g else f"(*(gen_v16 *)&{n.val})"
        if k in ("id", "num"):
            return n.val
        if k == "paren":
            return f"({self.e(n.kids[0])})"
        if k == "member":
            m = PIECE.search(n.val)
            if m and int(m.group(2)) > 8:
                off, size = int(m.group(1)), int(m.group(2))
                return f"gen_v16_load((const char *)&{self.place(n.kids[0])} + {off}, {size})"
            return f"{self.e(n.kids[0])}{n.val}"
        if k == "cast":
            inner = n.kids[0]
            ty = re.sub(r"\s+", " ", n.val)
            if T.ty(n) == "p16":
                core = inner.kids[0] if inner.kind == "paren" else inner
                if core.kind == "bin" and core.val in "+-":
                    a_, b_ = core.kids
                    # a 16-byte local offset under a pointer cast (`(undefined1 (*) [16])(local + 0xc)`):
                    # the array decays to its address in Ghidra's C, so this is an address
                    if a_.kind == "id" and T.ty(a_) == V and T.ty(b_) != V:
                        return f"((gen_v16 *)((unsigned char *)&{a_.val} {core.val} {self.e(b_)}))"
                return f"((gen_v16 *){self.e(inner)})"
            if T.ty(n) == V and (re.sub(r"\s", "", n.val) in ("undefined1[16]", "undefined[16]")
                                 or WIDE_INT_CAST.fullmatch(n.val.strip())):
                # to 16 bytes (`(undefined1 [16])0x0`, or a 16-byte value already): the value
                return self.v(inner)
            if T.ty(inner) == V:
                # the low lane by its bits: `(float)auVar1` is the float movss would store
                if ty == "float":
                    return f"gen_v16_f0({self.e(inner)})"
                if ty in ("double", "float10"):
                    return f"gen_v16_d0({self.e(inner)})"
                return f"(({ty})gen_v16_q0({self.e(inner)}))"
            if T.ty(n) == V:
                # a scalar cast to 16 bytes (`(undefined1 [16])0x0`): C casts to no array type
                return f"gen_v16_u({self.e(inner)})"
            return f"({ty}){self.e(inner)}"
        if k == "unary":
            sub = n.kids[0]
            if n.val == "-" and T.ty(n) == V:
                return f"gen_v16_neg({self.e(sub)})"
            if n.val == "~" and T.ty(n) == V:
                return f"gen_v16_not({self.e(sub)})"
            if n.val == "!" and T.ty(sub) == V:
                return f"gen_v16_zero({self.e(sub)})"
            if n.val == "&" and sub.kind == "id" and sub.val in T.short:
                # (the address of a short byte array is the array's, not of a copy of it)
                return f"&{sub.val}"
            return f"{n.val}{self.e(sub)}"
        if k == "index":
            base = n.kids[0]
            if base.kind == "id" and base.val in T.short:
                return f"{base.val}[{self.e(n.kids[1])}]"
            # a byte of a 16-byte value (`(*pauVar19)[1]`): the union's bytes
            if T.ty(n.kids[0]) == V:
                return f"{self.e(n.kids[0])}.b[{self.e(n.kids[1])}]"
            return f"{self.e(n.kids[0])}[{self.e(n.kids[1])}]"
        if k == "bin" and T.ty(n) == "bytes":
            a, b = n.kids
            if is_place16(a, T):
                return f"((unsigned char *)&{self.e(a)} {n.val} {self.e(b)})"
            return f"({self.e(a)} {n.val} (unsigned char *)&{self.e(b)})"
        if k == "bin":
            a, b = n.kids
            ta, tb = T.ty(a), T.ty(b)
            if V not in (ta, tb):
                return f"{self.e(a)} {n.val} {self.e(b)}"
            if n.val in ("<<", ">>") and ta == V:
                return f"{V16_BIN[n.val]}({self.e(a)}, (int)({self.e(b)}))"
            if n.val in V16_BIN:
                return f"{V16_BIN[n.val]}({self.v(a)}, {self.v(b)})"
            if n.val in V16_CMP:
                f = V16_CMP[n.val]
                neg = f.startswith("!")
                return f"{'!' if neg else ''}{f.lstrip('!')}({self.v(a)}, {self.v(b)})"
            if n.val in ("&&", "||"):
                return f"!gen_v16_zero({self.v(a)}) {n.val} !gen_v16_zero({self.v(b)})"
            raise V16Unsupported(f"operator {n.val} on 16 bytes")
        if k == "comma":
            return f"{self.e(n.kids[0])}, {self.e(n.kids[1])}"
        if k == "cond":
            c, a, b = n.kids
            cs = f"!gen_v16_zero({self.e(c)})" if T.ty(c) == V else self.e(c)
            if T.ty(n) == V:
                return f"({cs} ? {self.v(a)} : {self.v(b)})"
            return f"({cs} ? {self.e(a)} : {self.e(b)})"
        if k == "assign":
            lhs, rhs = n.kids
            tl, tr = T.ty(lhs), T.ty(rhs)
            if lhs.kind == "member" and PIECE.search(lhs.val) and int(PIECE.search(lhs.val).group(2)) > 8:
                m = PIECE.search(lhs.val)
                if n.val != "=":
                    raise V16Unsupported("compound assignment to a wide piece")
                return (f"gen_v16_store((char *)&{self.place(lhs.kids[0])} + {int(m.group(1))}, {int(m.group(2))}, "
                        f"{self.v(rhs)})")
            if lhs.kind == "id" and lhs.val in T.short:
                if n.val != "=":
                    raise V16Unsupported("compound assignment to a short byte array")
                return f"gen_v16_store({lhs.val}, {T.short[lhs.val]}, {self.v(rhs)})"
            if tl == V:
                if n.val == "=":
                    return f"{self.e(lhs)} = {self.v(rhs)}"
                op = n.val[0]
                return f"{self.e(lhs)} = {V16_BIN[op]}({self.e(lhs)}, {self.v(rhs)})"
            if tr == V:
                lv = self.e(lhs)
                if n.val == "=" and rhs.kind == "id" and lhs.kind == "id" and lhs.val in T.pointers \
                        and rhs.val in T.v16 and not re.fullmatch(r"auVar\d+", rhs.val):
                    # a 16-byte array in the frame assigned to a pointer (`local_1d8 = local_1c8;`,
                    # a string pointed at its own inline buffer, FUN_142586960) is its address,
                    # as C's array decay has it; a register's value (`auVarN`) is its bits
                    return f"{lv} = (void *)&{self.e(rhs)}"
                return f"{lv} {n.val} GEN_V16_AS({lv}, {self.e(rhs)})"
            return f"{self.e(lhs)} {n.val} {self.e(rhs)}"
        if k == "call":
            fn, args = n.kids[0], n.kids[1:]
            name = fn.val if fn.kind == "id" else None
            if name and CONCAT.match(name) and T.ty(n) == V:
                hi, lo = concat_sizes(name)
                return f"gen_v16_zext(gen_v16_concat({self.v(args[0])}, {self.v(args[1])}, {lo}), {hi + lo})"
            if name and EXT16.match(name):
                zs, w = EXT16.match(name).groups()
                fn_ = "gen_v16_zext" if zs == "Z" else "gen_v16_sext"
                a = args[0]
                if T.ty(a) == V:
                    return f"{fn_}({self.e(a)}, {int(w)})"
                return f"{fn_}(gen_v16_u({self.e(a)}), {int(w)})"
            if name and sub_sizes(name):
                w = sub_sizes(name)[1]
                if w > 8:
                    # (wider than a scalar: the bytes as a 16-byte value, from offset k)
                    return f"gen_v16_zext(gen_v16_shr({self.v(args[0])}, 8 * (int)({self.e(args[1])})), {w})"
                return f"gen_v16_bytes({self.v(args[0])}, (int)({self.e(args[1])}), {w})"
            if name in PSEUDO:
                tpl, kind = PSEUDO[name]
                # a shift by a register (`psraw(auVar24,ZEXT416(n))`, `psllw(a,auVar50)`): the
                # count is that register's low qword, the intrinsic's non-immediate form (C2440 on
                # gen_v16 to int, 5 functions, 2026-10-17)
                by_reg = re.match(r"_mm_s(ll|rl|ra)i_(epi16|epi32|epi64)\(", tpl) and \
                    any(sp == ":imm" and self.T.ty(args[int(ix)]) == V for ix, sp in re.findall(r"\{(\d)(:imm)?\}", tpl))
                if by_reg:
                    tpl = re.sub(r"^_mm_s(ll|rl|ra)i_", r"_mm_s\1_", tpl).replace(":imm}", "}")

                def fill(mm):
                    i, spec = int(mm.group(1)), mm.group(2)
                    if spec == ":imm":
                        return f"({self.e(args[i])})"
                    return self.m(args[i])

                c = re.sub(r"\{(\d)(:imm)?\}", fill, tpl)
                if kind == "s":
                    return c
                return {"i": f"gen_v16_m({c})", "f": f"gen_v16_f({c})", "d": f"gen_v16_d({c})"}[kind]
            if name in SCALAR_PSEUDO:
                # (the destination's old value is no operand of movmskps: Ghidra passes whatever the
                # register held, often a register it lost track of, `movmskps(in_EAX, v)`)
                return f"{SCALAR_PSEUDO[name]}(0, {self.v(args[1])})"
            if name and WIDE_PSEUDO.search(name):
                raise V16Unsupported(f"{name}: a 32- or 64-byte vector")
            # an ordinary call: a 16-byte argument goes by its first 8 bytes (the register's
            # low qword, what a scalar parameter in xmm or a general register holds)
            # (but whole to a callee that takes 16 bytes there, a vector parameter)
            whole = T.vparams.get(fn.val, ()) if fn.kind == "id" else ()
            # (and a 16-byte array in the frame is its address, as C's array decay has it, the same
            # as assigned to a pointer above: `FUN_1419ad790(&DAT_142d2c038,&local_28,local_10)`
            # hands on the end of a six-int initializer list, and its first 8 bytes crashed the
            # static initializer FUN_1401fd640, 2026-10-17; a register's value `auVarN` is its bits)
            def arg(k, a):
                if T.ty(a) != V or k in whole:
                    return self.e(a)
                if a.kind == "id" and a.val in T.v16 and not re.fullmatch(r"auVar\d+", a.val):
                    return f"(void *)&{self.e(a)}"
                return f"gen_v16_q0({self.e(a)})"
            out = [arg(k, a) for k, a in enumerate(args)]
            return f"{self.e(fn)}({', '.join(out)})"
        raise V16Unsupported(f"node {k}")


# ---- statements ----------------------------------------------------------------------------

DECL16 = re.compile(r"^([ \t]*)undefined1? (\w+) \[16\];$", re.M)
DECLP16 = re.compile(r"^([ \t]*)undefined1? \(\*(\w+)\) \[16\];$", re.M)
# a byte array of 9 to 15 (`undefined1 auVar83 [12]`): moved to and from 16-byte values by its bytes
DECLN = re.compile(r"^[ \t]*undefined1? (\w+) \[(9|1[0-5])\];$", re.M)
# an array of them (`undefined1 (*local_148 [4]) [16]`)
DECLAP16 = re.compile(r"^([ \t]*)undefined1? \(\*(\w+) \[(\w+)\]\) \[16\];$", re.M)
# a cast to an integer of 9 to 15 bytes (`(unkuint10)auVar83._0_10_ >> 0x40`, the top two bytes of
# a 10-byte value, FUN_14030acc0): the value as 16 bytes, zero above, which the shift then reads
WIDE_INT_CAST = re.compile(r"unk(?:u?int|byte)(?:9|1[0-5])")
DECL2D16 = re.compile(r"^([ \t]*)undefined1? (\w+) \[(\w+)\] \[16\];$", re.M)
WIDE_DECL = re.compile(r"^[ \t]*undefined1? \(?\*?\w+\)? \[(?:32|64)\];$", re.M)
WIDE_TERM = re.compile(r"\b(?:ZEXT\d{1,2}16|SEXT\d{1,2}16|SUB(?:9|1[0-6])\d{1,2})\(|\._\d+_(?:9|1[0-6])_|\(\*\) \[16\]")


def concat16(text: str) -> bool:
    """Whether text calls a CONCATab wider than a scalar (CONCAT88, CONCAT412, CONCAT81, ...)."""
    return any(concat_sizes("CONCAT" + d) for d in re.findall(r"\bCONCAT(\d+)\(", text))


def head_of(body: str) -> str:
    """The signature: what comes before the body's `{`."""
    return body.split("{", 1)[0]


def rewrite(body: str, wide_globals: set[str] = frozenset(), vfuncs: set[str] = frozenset(),
            vparams: dict[str, set[int]] | None = None) -> str:
    """The body with every statement on 16-byte values in C. A statement this cannot take
    raises V16Unsupported; the caller keeps the body as it was."""
    v16 = {m.group(2) for m in DECL16.finditer(body)} | {m.group(3) for m in DECLWIDE_INT.finditer(body)}
    v16ptr = {m.group(2) for m in DECLP16.finditer(body)}
    # (and parameters of that type, in the header: `undefined1 (*param_2) [16]`)
    v16ptr |= set(re.findall(r"undefined1? \(\*(param_\d+)\) \[16\]", body.split("{", 1)[0]))
    # (and a 16-byte value parameter, a vector in an xmm register: `undefined1 param_1 [16]`)
    v16 |= set(re.findall(r"undefined1? (param_\d+) \[16\]", body.split("{", 1)[0]))
    v16ptrs = {m.group(2) for m in DECLAP16.finditer(body)}
    # (and an array of 16-byte values, `undefined1 local_5a0 [3] [16]`: its rows are assigned
    # whole, FUN_141723600, so it is gen_v16 [3], and its name the pointer it decays to)
    v16ptr |= {m.group(2) for m in DECL2D16.finditer(body)}
    pseudo = [p for p in list(PSEUDO) + list(SCALAR_PSEUDO) if re.search(rf"\b{p}\(", body)]
    globs = {g for g in wide_globals if re.search(rf"\b{re.escape(g)}\b", body)}
    calls16 = {f for f in vfuncs if re.search(rf"\b{f}\s*\(", body)}
    if not (v16 or v16ptr or v16ptrs or pseudo or globs or calls16 or WIDE_TERM.search(body) or concat16(body)):
        return body
    # (a 32- or 64-byte variable is left an array: its pieces are what Ghidra reads of it, and a
    # 16-byte one, `auVar5._0_16_`, is a load like any wide piece)
    if any(WIDE_PSEUDO.search(n) for n in re.findall(r"\b([a-z]\w*)\(", body)):
        raise V16Unsupported("a 32- or 64-byte vector (an AVX2 or AVX-512 path)")
    short = {m.group(1) for m in DECLN.finditer(body)}
    words = v16 | v16ptr | v16ptrs | set(pseudo) | globs | short | calls16
    mention = re.compile(r"\b(?:" + "|".join(map(re.escape, sorted(words))) + r")\b") if words else None
    typer = Typer(v16, v16ptr, globs)
    typer.v16ptrs = v16ptrs
    typer.short = {m.group(1): int(m.group(2)) for m in DECLN.finditer(body)}
    typer.vfuncs = calls16
    typer.vparams = vparams or {}
    # (the variables Ghidra declares as pointers, locals and parameters)
    typer.pointers = set(re.findall(r"^[ \t]*[\w ]+\*+\s*(\w+);$", body, re.M)) | \
        set(re.findall(r"[\w ]+\*+\s*(\w+)\s*[,)]", head_of(body)))
    em = Emitter(typer)
    head, rest = body.split("{", 1)
    rest = DECL16.sub(lambda m: f"{m.group(1)}gen_v16 {m.group(2)};", rest)
    rest = DECLWIDE_INT.sub(lambda m: f"{m.group(1)}gen_v16 {m.group(3)};", rest)
    rest = DECLP16.sub(lambda m: f"{m.group(1)}gen_v16 *{m.group(2)};", rest)
    rest = DECLAP16.sub(lambda m: f"{m.group(1)}gen_v16 *{m.group(2)}[{m.group(3)}];", rest)
    rest = DECL2D16.sub(lambda m: f"{m.group(1)}gen_v16 {m.group(2)}[{m.group(3)}];", rest)
    out = []
    for a, b in spans(rest):
        text = rest[a:b]
        if not ((mention and mention.search(text)) or WIDE_TERM.search(text) or concat16(text)):
            continue
        toks = tokens(text)
        p = Parser(toks)
        tree = p.comma()
        if p.i != len(toks):
            raise V16Unsupported(f"trailing tokens in {text[:60]!r}")
        lead = text[:len(text) - len(text.lstrip())]
        out.append((a, b, lead + em.e(tree)))
    for a, b, new in reversed(out):
        rest = rest[:a] + new + rest[b:]
    # a 16-byte local Ghidra also indexes as bytes (`auStack_48[0] = 0`): its bytes in the union
    # (221 functions demoted with C2088 when it became a gen_v16 without this, 2026-10-16)
    for name in v16:
        rest = re.sub(rf"(?<![\w.>]){re.escape(name)}(\s*)\[", rf"{name}.b\1[", rest)
    # a register Ghidra reads as an input only for an instruction that overwrites it whole
    # (`pshuflw(in_XMM1, x, 0)`: the old value is no operand of the intrinsic): gone with it
    for m in list(re.finditer(r"^[ \t]*[\w ]+?\*? ?(in_\w+)(?: \[\d+\])?;\n", rest, re.M)):
        if not re.search(rf"\b{m.group(1)}\b", rest[:m.start()] + rest[m.end():]):
            rest = rest.replace(m.group(0), "", 1)
    return head + "{" + rest


def spans(rest: str):
    """(start, end) of each expression: an expression statement up to its `;`, a `return`'s
    operand, and the parenthesized condition of if / while / switch / for's parts."""
    out = []
    i, n = 0, len(rest)
    stmt_start = True
    while i < n:
        ch = rest[i]
        if ch in " \t\n":
            i += 1
            continue
        if ch == '"' or ch == "'":
            j = i + 1
            while j < n and rest[j] != ch:
                j += 2 if rest[j] == "\\" else 1
            i = j + 1
            stmt_start = False
            continue
        if rest.startswith("/*", i):
            i = rest.find("*/", i) + 2
            continue
        if ch in "{};":
            stmt_start = True
            i += 1
            continue
        m = re.match(r"(if|while|switch|for)\s*\(", rest[i:])
        if stmt_start and m:
            j = i + m.end()
            depth, k = 1, j
            while k < n and depth:
                if rest[k] in "'\"":
                    # (a quoted `'('` is no parenthesis: FUN_1421c9900's `== '('` ran the
                    # condition on into the statement after it)
                    q, k = rest[k], k + 1
                    while k < n and rest[k] != q:
                        k += 2 if rest[k] == "\\" else 1
                    k += 1
                    continue
                depth += {"(": 1, ")": -1}.get(rest[k], 0)
                k += 1
            inner = (j, k - 1)
            if m.group(1) == "for":
                parts, s0, d = [], inner[0], 0
                for q in range(inner[0], inner[1]):
                    d += {"(": 1, ")": -1}.get(rest[q], 0)
                    if rest[q] == ";" and d == 0:
                        parts.append((s0, q))
                        s0 = q + 1
                parts.append((s0, inner[1]))
                out += [p for p in parts if rest[p[0]:p[1]].strip()]
            else:
                out.append(inner)
            i = k
            stmt_start = True
            continue
        m = re.match(r"(?:else|do|default)\b", rest[i:])
        if stmt_start and m:
            i += m.end()
            continue
        m = re.match(r"case\b[^:]*:", rest[i:])
        if stmt_start and m:
            i += m.end()
            continue
        m = re.match(r"(?:goto\s+\w+|break|continue|return)\s*;", rest[i:])
        if stmt_start and m:
            i += m.end()
            continue
        m = re.match(r"[A-Za-z_]\w*\s*:(?!:)", rest[i:])
        if stmt_start and m and not re.match(r"default\b", rest[i:]):
            i += m.end()
            continue
        if stmt_start and re.match(r"(?:[\w ]+?\**)\s+\w+(?:\s*\[[^\]]*\])?\s*;", rest[i:]) and \
                re.match(r"[A-Za-z_]", ch) and not re.match(r"return\b", rest[i:]):
            # a declaration
            i = rest.find(";", i) + 1
            continue
        if stmt_start:
            start = i
            if rest.startswith("return", i) and re.match(r"return\b", rest[i:]):
                start = i + len("return")
            depth, k = 0, start
            while k < n:
                c = rest[k]
                if c in "\"'":
                    q = k + 1
                    while q < n and rest[q] != c:
                        q += 2 if rest[q] == "\\" else 1
                    k = q + 1
                    continue
                if c in "([":
                    depth += 1
                elif c in ")]":
                    depth -= 1
                elif c == ";" and depth == 0:
                    break
                k += 1
            out.append((start, k))
            i = k
            stmt_start = False
            continue
        i += 1
    return out
