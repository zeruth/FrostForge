"""Puts Rhipper (the Ghidra-to-C tooling, its own repository) on the import path for FrostLite's
own tools, which build on its modules (pebin, corpus, oracle, translate, ...).

Rhipper's checkout is the "rhipper" setting in ../rhipper.json, or RHIPPER_HOME. Import this
first in a FrostLite tool:

    import _rhipper  # noqa: F401  (Rhipper's modules on the path)
"""
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME = os.environ.get("RHIPPER_HOME") or json.loads((ROOT / "rhipper.json").read_text(encoding="utf-8")).get("rhipper")
if not HOME or not (Path(HOME) / "project.py").is_file():
    sys.exit(f"Rhipper not found ({HOME!r}): set \"rhipper\" in {ROOT / 'rhipper.json'} or RHIPPER_HOME")
HOME = str(Path(HOME).resolve())
if HOME not in sys.path:
    # (after this tool's own directory: FrostLite's modules win a name clash)
    sys.path.insert(1, HOME)
# Rhipper's tools find the project by rhipper.json from the working directory; a FrostLite tool
# may be run from anywhere
os.environ.setdefault("RHIPPER_PROJECT", str(ROOT))
