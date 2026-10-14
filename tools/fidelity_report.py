"""A one-page visual of port fidelity by subsystem, from build/fidelity.json, with the code
tools/translate.py generated (src/gen, its manifest) shown apart from the hand ports.

    python tools/fidelity.py --summary        # refresh build/fidelity.json first
    python tools/fidelity_report.py           # writes build/fidelity.html
"""
from __future__ import annotations

import datetime
import html
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "build/fidelity.json"
MANIFEST = ROOT / "src/gen/manifest.json"
REACHED = ROOT / "build/reached.json"   # tools/reached.py --json, from a FROST_HITS run
OUT = ROOT / "build/fidelity.html"

# source folder -> bucket
BUCKETS = {
    "Rendering": ["gx", "gxu", "model2", "shadereffect", "texture", "images", "world", "daynight", "prism"],
    "UI & Lua": ["ui", "glue", "framescript", "console"],
    "Data & files": ["db", "cas", "casc", "tact", "fileservices", "aio"],
    "Networking & services": ["services"],
    "Audio": ["sound"],
    "Platform & core": ["os", "base", "blz", "storm", "util", "threadpool", "app", "client"],
}
COLORS = {"Rendering": "#7c6cf2", "UI & Lua": "#2fa8d8", "Data & files": "#3cb179",
          "Networking & services": "#e0913b", "Audio": "#d9577f", "Platform & core": "#8a94a6"}
BUCKET_OF = {d: b for b, ds in BUCKETS.items() for d in ds}


def reference_counts() -> tuple[int, int]:
    """Every function in the reference, and those that are not FID-named library code."""
    from corpus import load_corpus
    from fidelity import REF_KEEP
    cor = load_corpus()
    lib = sum(1 for e in cor.values() if not e["name"].startswith(REF_KEEP) and "::" not in e["name"])
    return len(cor), len(cor) - lib


def folder(path: str) -> str:
    parts = path.replace("\\", "/").split("/")
    return parts[1] if len(parts) > 2 and parts[0] == "src" else "other"


def generated(r: dict) -> bool:
    return r["file"].replace("\\", "/").startswith("src/gen/")


def reason_class(why: str) -> str:
    """A demotion reason without its particulars, so like ones count together."""
    if why.startswith("did not compile"):
        return "did not compile"
    k = re.sub(r"\s*\(.*", "", why)
    k = re.sub(r"\b(?:FUN|DAT|LAB|PTR)_[0-9a-f]+|0x[0-9a-f]+", "…", k)
    return k.strip()


def main() -> None:
    every = json.loads(SRC.read_text())
    rows = [r for r in every if not generated(r)]
    gen_rows = [r for r in every if generated(r) and r["kind"] != "stub"]
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8")) if MANIFEST.exists() else {}
    translated = manifest.get("translated", {})
    not_translated = manifest.get("stubbed", {})
    groups = defaultdict(list)
    for r in rows:
        groups[BUCKET_OF.get(folder(r["file"]), "Platform & core")].append(r)

    def stats(rs):
        scored = [r for r in rs if r["kind"] != "stub"]
        size = sum(r["ref_size"] for r in scored) or 1
        return {
            "total": len(rs),
            "high": sum(r["kind"] == "ported" and r["score"] >= 0.9 for r in rs),
            "mid": sum(r["kind"] == "ported" and r["score"] < 0.9 for r in rs),
            "partial": sum(r["kind"] == "partial" for r in rs),
            "stub": sum(r["kind"] == "stub" for r in rs),
            "live": sum(r["kind"] == "stub" and r.get("live", False) for r in rs),
            "same": sum(r.get("same", False) for r in rs),
            "mean": sum(r["score"] for r in scored) / len(scored) if scored else 0.0,
            "weighted": sum(r["score"] * r["ref_size"] for r in scored) / size,
            "hist": [sum(1 for r in scored if i / 10 <= r["score"] < (i + 1) / 10 or (i == 9 and r["score"] >= 1))
                     for i in range(10)],
            "folders": sorted({folder(r["file"]) for r in rs}),
            "worst": sorted((r for r in scored if r["ref_size"] >= 200), key=lambda r: r["score"])[:5],
        }

    overall = stats(rows)
    ref_total, ref_game = reference_counts()
    tagged = len(rows)
    order = sorted(groups, key=lambda b: -len(groups[b]))

    def bar(s):
        t = s["total"] or 1
        segs = [("high", "seg-high", "0.9+"), ("mid", "seg-mid", "below 0.9"), ("partial", "seg-partial", "partial"),
                ("stub", "seg-stub", "stub")]
        return "".join(f'<span class="{c}" style="width:{100 * s[k] / t:.2f}%" title="{s[k]} {label}"></span>'
                       for k, c, label in segs if s[k])

    def hist(s, color):
        peak = max(s["hist"]) or 1
        return "".join(f'<span style="height:{max(4, 100 * v / peak):.0f}%;background:{color};opacity:{0.35 + 0.65 * i / 9:.2f}" '
                       f'title="{i / 10:.1f}–{(i + 1) / 10:.1f}: {v}"></span>' for i, v in enumerate(s["hist"]))

    cards = []
    for b in order:
        s = stats(groups[b])
        color = COLORS[b]
        worst = "".join(f'<li><span class="score">{r["score"]:.2f}</span>{html.escape(r["name"].replace(chr(96) + "anonymous namespace" + chr(39) + "::", ""))[:60]}</li>'
                        for r in s["worst"])
        cards.append(f"""
<section class="card" style="--accent:{color}">
  <header><h2>{html.escape(b)}</h2><span class="count">{s['total']:,} functions</span></header>
  <div class="top">
  <dl>
    <div><dt>Mean</dt><dd>{s['mean']:.2f}</dd></div>
    <div><dt>0.9+</dt><dd>{s['high']:,}</dd></div>
    <div><dt>Partial</dt><dd>{s['partial']:,}</dd></div>
    <div><dt>Stubs</dt><dd>{s['stub']:,} <em>{s['live']} live</em></dd></div>
  </dl>
  <div class="ring" style="--p:{s['weighted'] * 100:.1f}"><div><b>{s['weighted'] * 100:.0f}</b><small>size-weighted</small></div></div>
  </div>
  <div class="bar">{bar(s)}</div>
  <div class="hist">{hist(s, color)}</div>
  <div class="axis"><span>0</span><span>score</span><span>1</span></div>
  <h3>Largest gaps</h3><ol>{worst or '<li>none</li>'}</ol>
  <p class="folders">{' · '.join(html.escape(f) for f in s['folders'])}</p>
</section>""")

    # the translated code: what it covers beyond the hand ports, how close its compile comes to the
    # reference's, and why the rest of what it reached stayed a stub
    hand = {r["addr"] for r in rows if r["kind"] != "stub"}
    gen_only = set(translated) - hand
    g = stats(gen_rows) if gen_rows else None
    reasons = Counter(reason_class(w) for w in not_translated.values())
    peak = max(reasons.values(), default=1)
    reason_list = "".join(
        f'<li><span class="rbar" style="width:{100 * v / peak:.1f}%"></span><b>{v:,}</b> {html.escape(k)}</li>'
        for k, v in reasons.most_common(10))
    gen_section = ""
    if g:
        gworst = "".join(f'<li><span class="score">{r["score"]:.2f}</span>FUN_{html.escape(r["addr"])}'
                         f' <span class="dim">{r["ref_size"]:,} bytes</span></li>' for r in g["worst"])
        gen_section = f"""
<h3>Translated from the decompilation</h3>
<p class="sub small">Reference functions with no hand port, run as C that tools/translate.py writes from the
Ghidra decompilation (src/gen). The score compares the compiled translation with the reference function itself.</p>
<div class="grid">
<section class="card" style="--accent:var(--gen)">
  <header><h2>Translated</h2><span class="count">{len(translated):,} functions</span></header>
  <div class="top">
  <dl>
    <div><dt>Mean</dt><dd>{g['mean']:.2f}</dd></div>
    <div><dt>0.9+</dt><dd>{g['high']:,}</dd></div>
    <div><dt>Scored</dt><dd>{g['total']:,}</dd></div>
    <div><dt>Lua handlers</dt><dd>{len(manifest.get('handlers', {})):,}</dd></div>
    <div><dt>Adapted to ports</dt><dd>{len(manifest.get('adapted', [])):,}</dd></div>
    <div><dt>Left as stubs</dt><dd>{len(not_translated):,}</dd></div>
  </dl>
  <div class="ring" style="--p:{g['weighted'] * 100:.1f}"><div><b>{g['weighted'] * 100:.0f}</b><small>size-weighted</small></div></div>
  </div>
  <div class="hist">{hist(g, 'var(--gen)')}</div>
  <div class="axis"><span>0</span><span>score</span><span>1</span></div>
  <h3>Largest gaps</h3><ol>{gworst or '<li>none</li>'}</ol>
</section>
<section class="card" style="--accent:var(--stub)">
  <header><h2>Why the rest stayed stubs</h2><span class="count">{len(not_translated):,} functions</span></header>
  <p class="folders">Reached from translated code but refused by the translator; each class is one fix in tools/translate.py.</p>
  <ol class="reasons">{reason_list or '<li>none</li>'}</ol>
</section>
</div>"""

    # what the last FROST_HITS run reached of the translated code: the runtime part, by calls, is
    # the hand-port queue (tools/reached.py)
    reached_section = ""
    if REACHED.exists():
        rd = json.loads(REACHED.read_text(encoding="utf-8"))
        funcs = [f for f in rd.get("functions", []) if f["addr"] in translated]
        runtime = [f for f in funcs if f["runtime"]]
        startup = [f for f in funcs if not f["runtime"]]
        stamp = datetime.datetime.fromtimestamp(REACHED.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
        peak_calls = max((f["calls"] for f in runtime), default=1)
        queue = "".join(
            f'<li><span class="rbar gen" style="width:{100 * f["calls"] / peak_calls:.1f}%"></span>'
            f'<b>{f["calls"]:,}</b> {html.escape(f["name"] or "FUN_" + f["addr"])}'
            f'{" <span class=dim>root</span>" if f["root"] else ""}'
            f' <span class="dim">{f["size"]:,} bytes</span></li>'
            for f in runtime[:15])
        reached_section = f"""
<section class="card" style="--accent:var(--gen)">
  <header><h2>Reached by the last run</h2><span class="count">{len(funcs):,} functions · {stamp}</span></header>
  <div class="top">
  <dl>
    <div><dt>Runtime</dt><dd>{len(runtime):,}</dd></div>
    <div><dt>Startup only</dt><dd>{len(startup):,}</dd></div>
    <div><dt>Runtime roots</dt><dd>{sum(f['root'] for f in runtime):,}</dd></div>
    <div><dt>Initializers</dt><dd>{sum(f['initializer'] for f in funcs):,}</dd></div>
  </dl>
  </div>
  <p class="folders">Translated code a FROST_HITS run entered, still without a hand port. The runtime part,
  most-called first, is the queue for porting by hand (tools/reached.py, tools/promote.py for a home file).</p>
  <ol class="reasons">{queue or '<li>none</li>'}</ol>
</section>"""
    if reached_section:
        # a third card in the translated section's grid, which its last </div> closes
        if gen_section:
            cut = gen_section.rfind("</div>")
            gen_section = gen_section[:cut] + reached_section + gen_section[cut:]
        else:
            gen_section = f'<div class="grid">{reached_section}</div>'

    o = overall
    covered = len(hand) + len(gen_only)
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FrostLite Fidelity</title>
<style>
:root {{ --bg:#f6f7fb; --card:#fff; --ink:#1c2030; --muted:#6b7285; --line:#e4e7ef;
  --high:#3cb179; --mid:#9fd3b6; --partial:#f0b44c; --stub:#e5677a; --gen:#5b8def; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --bg:#11131a; --card:#1a1d27; --ink:#e8eaf2;
  --muted:#9097ab; --line:#2a2e3b; --mid:#2f6b4d; }} }}
* {{ box-sizing:border-box }}
body {{ margin:0; background:var(--bg); color:var(--ink); font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif; }}
main {{ max-width:1200px; margin:0 auto; padding:32px 16px 48px }}
h1 {{ font-size:28px; margin:0 0 4px; letter-spacing:-.02em }}
.sub {{ color:var(--muted); margin:0 0 24px }}
.hero {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:12px; margin-bottom:16px }}
.stat {{ background:var(--card); border:1px solid var(--line); border-radius:14px; padding:14px 16px }}
.stat b {{ display:block; font-size:26px; letter-spacing:-.02em }} .stat span {{ color:var(--muted); font-size:13px }}
.cover {{ background:var(--card); border:1px solid var(--line); border-radius:14px; padding:14px 16px; margin-bottom:8px }}
.cover span {{ color:var(--muted) }} .cover .note {{ color:var(--muted); font-size:13px; margin-top:8px }}
.bar.thin {{ height:10px; margin-top:10px }}
.legend {{ display:flex; flex-wrap:wrap; gap:16px; color:var(--muted); font-size:13px; margin:16px 0 20px }}
.legend i {{ display:inline-block; width:10px; height:10px; border-radius:3px; margin-right:6px; vertical-align:-1px }}
.grid {{ display:grid; grid-template-columns:repeat(auto-fill,minmax(min(340px,100%),1fr)); gap:16px }}
.card {{ background:var(--card); border:1px solid var(--line); border-top:4px solid var(--accent); border-radius:16px; padding:18px; min-width:0 }}
.top {{ display:flex; align-items:center; justify-content:space-between; gap:16px; margin:14px 0 }}
.card header {{ display:flex; justify-content:space-between; align-items:baseline; gap:8px }}
.card h2 {{ font-size:18px; margin:0 }} .count {{ color:var(--muted); font-size:13px }}
.ring {{ --p:0; flex:none; width:96px; height:96px; border-radius:50%; position:relative;
  background:conic-gradient(var(--accent) calc(var(--p)*1%), var(--line) 0) }}
.ring div {{ position:absolute; inset:9px; border-radius:50%; background:var(--card); display:flex; flex-direction:column;
  align-items:center; justify-content:center }}
.ring b {{ font-size:24px; line-height:1 }} .ring small {{ font-size:10px; color:var(--muted) }}
dl {{ display:grid; grid-template-columns:1fr 1fr; gap:6px 12px; margin:0; flex:1 }}
dt {{ color:var(--muted); font-size:12px }} dd {{ margin:0; font-weight:600 }} dd em {{ font-weight:400; color:var(--stub); font-style:normal; font-size:12px }}
.bar {{ display:flex; height:12px; border-radius:6px; overflow:hidden; background:var(--line) }}
.seg-high {{ background:var(--high) }} .seg-mid {{ background:var(--mid) }} .seg-partial {{ background:var(--partial) }} .seg-stub {{ background:var(--stub) }}
.hist {{ display:flex; align-items:flex-end; gap:3px; height:54px; margin-top:16px }}
.hist span {{ flex:1; border-radius:3px 3px 0 0 }}
.axis {{ display:flex; justify-content:space-between; color:var(--muted); font-size:11px; margin-top:4px }}
h3 {{ font-size:13px; color:var(--muted); font-weight:600; margin:16px 0 6px; text-transform:uppercase; letter-spacing:.04em }}
ol {{ margin:0; padding:0; list-style:none; font-size:13px }} ol li {{ padding:3px 0; border-bottom:1px solid var(--line);
  white-space:nowrap; overflow:hidden; text-overflow:ellipsis }}
.score {{ display:inline-block; min-width:38px; color:var(--stub); font-variant-numeric:tabular-nums }}
.folders {{ color:var(--muted); font-size:12px; margin:12px 0 0 }}
.seg-gen {{ background:var(--gen) }} .dim {{ color:var(--muted); font-size:12px }} .sub.small {{ font-size:13px; margin:-2px 0 14px }}
ol.reasons li {{ position:relative; isolation:isolate; padding:5px 6px; white-space:normal }}
ol.reasons b {{ display:inline-block; min-width:44px; font-variant-numeric:tabular-nums }}
.rbar {{ position:absolute; left:0; top:3px; bottom:3px; background:var(--stub); opacity:.16; border-radius:4px; z-index:-1 }}
.rbar.gen {{ background:var(--gen) }}
@media (max-width:420px) {{ .grid {{ grid-template-columns:1fr }} }}
</style></head><body><main>
<h1>FrostLite fidelity</h1>
<p class="sub">Hand-ported and translated functions compared against the reference 3.4.3 client · generated {datetime.date.today().isoformat()}</p>
<div class="hero">
  <div class="stat"><b>{o['total']:,}</b><span>tagged · {100 * o['total'] / ref_game:.1f}% of {ref_game:,}</span></div>
  <div class="stat"><b>{o['mean']:.2f}</b><span>mean fidelity</span></div>
  <div class="stat"><b>{o['weighted']:.2f}</b><span>size-weighted fidelity</span></div>
  <div class="stat"><b>{o['high']:,}</b><span>at 0.9 or better</span></div>
  <div class="stat"><b>{o['same']:,}</b><span>instruction-identical</span></div>
  <div class="stat"><b>{o['stub']:,}</b><span>stubs · {o['live']} live</span></div>
  <div class="stat"><b>{len(gen_only):,}</b><span>translated · {g['mean'] if g else 0:.2f} mean</span></div>
</div>
<div class="cover">
  <div><b>{covered:,}</b> of <b>{ref_game:,}</b> game functions covered <span>({100 * covered / ref_game:.1f}%):
    {len(hand):,} hand-ported, {len(gen_only):,} translated</span></div>
  <div class="bar thin"><span class="seg-high" style="width:{100 * (o['high'] + o['mid']) / ref_game:.2f}%" title="ported"></span><span class="seg-partial" style="width:{100 * o['partial'] / ref_game:.2f}%" title="partial"></span><span class="seg-gen" style="width:{100 * len(gen_only) / ref_game:.2f}%" title="translated"></span><span class="seg-stub" style="width:{100 * o['stub'] / ref_game:.2f}%" title="stub"></span></div>
  <div class="note">{ref_total:,} functions in the reference; {ref_total - ref_game:,} are library code (CRT, std) and left out. Ported {100 * (o['high'] + o['mid']) / ref_game:.1f}% · partial {100 * o['partial'] / ref_game:.1f}% · translated {100 * len(gen_only) / ref_game:.1f}% · stub {100 * o['stub'] / ref_game:.1f}%</div>
</div>
<h3>Fidelity of what is tagged</h3>
<div class="bar" style="height:16px">{bar(o)}</div>
<div class="legend"><span><i style="background:var(--high)"></i>ported, 0.9+</span><span><i style="background:var(--mid)"></i>ported, below 0.9</span>
<span><i style="background:var(--partial)"></i>partial</span><span><i style="background:var(--gen)"></i>translated</span><span><i style="background:var(--stub)"></i>stub</span></div>
<h3>Hand ports by subsystem</h3>
<div class="grid">{''.join(cards)}</div>
{gen_section}
</main></body></html>
"""
    OUT.write_text(page, encoding="utf-8", newline="\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
