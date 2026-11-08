#!/usr/bin/env python3
"""Incremental translation helper for localization/translations/<code>.lang.

`tools/localization.py new <code>` writes every declared string with an empty
value. This helper lets you fill those in batches and always tells you what is
left, so a long translation cannot drift and miss a key.

Commands:

    status    <code>            translated / remaining counts
    remaining <code> [count]    print next N untranslated entries with English
    apply     <code> <batch>    merge a partial .lang batch into the main file

`remaining` prints `ident =` followed by `# EN: <english>`, which is the batch
skeleton: copy it, replace the empty value with the translation, and feed it to
`apply`. Comment lines in a batch are ignored, so the `# EN:` hints are harmless.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
TRANS = ROOT / "localization" / "translations"

# Reuse the scanner from localization.py instead of re-implementing it.
spec = importlib.util.spec_from_file_location("loc_tool", TOOLS / "localization.py")
loc_tool = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = loc_tool
spec.loader.exec_module(loc_tool)


def english_map() -> dict[str, str]:
    result = loc_tool.scan(ROOT)
    return {entry.ident: entry.text for entry in result.entries}


def read_values(path: Path) -> tuple[dict[str, str], list[str]]:
    values: dict[str, str] = {}
    order: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        ident, value = line.split("=", 1)
        ident = ident.strip()
        if ident:
            values[ident] = value.strip()
            order.append(ident)
    return values, order


def cmd_status(code: str) -> int:
    values, _ = read_values(TRANS / f"{code}.lang")
    empty = [ident for ident, value in values.items() if not value]
    print(f"{code}: {len(values) - len(empty)}/{len(values)} translated, "
          f"{len(empty)} remaining")
    return 0


def cmd_remaining(code: str, count: int) -> int:
    en = english_map()
    values, _ = read_values(TRANS / f"{code}.lang")
    empties = [ident for ident in values if not values[ident]]
    shown = empties[:count]
    for ident in shown:
        print(f"{ident} =")
        print(f"# EN: {en.get(ident, '')}")
    print(f"# --- {len(empties)} remaining total; showing {len(shown)} ---")
    return 0


def cmd_apply(code: str, batch: str) -> int:
    main = TRANS / f"{code}.lang"
    values, _ = read_values(main)
    bvals, _ = read_values(Path(batch))
    applied = 0
    for ident, value in bvals.items():
        if ident not in values:
            print(f"warning: {ident} not in {code}.lang, skipped")
            continue
        if value:
            values[ident] = value
            applied += 1

    out: list[str] = []
    for raw in main.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            out.append(raw)
            continue
        ident = line.split("=", 1)[0].strip()
        out.append(f"{ident} = {values[ident]}")
    main.write_text("\n".join(out) + "\n", encoding="utf-8")

    empty = [ident for ident, value in values.items() if not value]
    print(f"applied {applied} entries; {code}: "
          f"{len(values) - len(empty)}/{len(values)} translated, "
          f"{len(empty)} remaining")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status")
    s.add_argument("code")
    r = sub.add_parser("remaining")
    r.add_argument("code")
    r.add_argument("count", type=int, nargs="?", default=10)
    a = sub.add_parser("apply")
    a.add_argument("code")
    a.add_argument("batch")
    args = parser.parse_args(argv)

    if args.cmd == "status":
        return cmd_status(args.code)
    if args.cmd == "remaining":
        return cmd_remaining(args.code, args.count)
    if args.cmd == "apply":
        return cmd_apply(args.code, args.batch)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
