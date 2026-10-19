# Fidelity history

One row per `python tools/fidelity.py --record` (see tools/fidelity.py for what the columns mean).
From 2026-10-19 `stubs` counts only stubs with nothing behind them: a hand stub whose reference
function runs translated (or natively, or through an adapter) is counted under `stubs translated`.

| date | commit | tied | ported | partial | stubs | live stubs | mean | size-weighted | >= 0.9 | < 0.5 | same | stubs translated |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-10-10 | a82645c | 8124 | 6863 | 173 | 1088 | 876 | 0.707 | 0.567 | 2772 | 1919 | 340 |
