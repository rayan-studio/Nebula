#!/usr/bin/env python3
"""Summarize Nebula LSP trace events from nebula.log.

Usage:
  python scripts/lsp_trace_report.py [path-to-log]

Default log path:
  logs/nebula.log
"""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

TRACE_TAG = "[LSP-TRACE]"

def extract_event(line: str) -> str:
    idx = line.find(TRACE_TAG)
    if idx < 0:
        return ""
    payload = line[idx + len(TRACE_TAG):].strip()
    if not payload:
        return ""
    return payload.split(" ", 1)[0]


def main() -> int:
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("logs") / "nebula.log"
    if not log_path.exists():
        print(f"Log file not found: {log_path}")
        return 1

    raw = log_path.read_bytes()
    # Nebula logs are often UTF-16LE on Windows. Auto-detect and decode.
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff") or b"\x00" in raw[:64]:
        text = raw.decode("utf-16", errors="ignore")
    else:
        text = raw.decode("utf-8", errors="ignore")
    lines = text.splitlines()

    trace_lines = [ln for ln in lines if TRACE_TAG in ln]
    if not trace_lines:
        print("No LSP trace lines found. Set NEBULA_LSP_TRACE=1 before launching Nebula.")
        return 0

    counts = Counter(extract_event(ln) for ln in trace_lines)

    print(f"Trace file: {log_path}")
    print(f"Total trace lines: {len(trace_lines)}")
    print("\nEvent counts:")
    for name, n in sorted(counts.items(), key=lambda x: (-x[1], x[0])):
        if not name:
            continue
        print(f"  {name}: {n}")

    # Heuristic warnings for common races/mismatches.
    drops = [ln for ln in trace_lines if "DropDiagnostics" in ln]
    upgrades = [ln for ln in trace_lines if "DidChange->DidOpen upgrade" in ln]
    missing_ctx = [ln for ln in trace_lines if "ctx=missing" in ln]

    print("\nHeuristics:")
    print(f"  DropDiagnostics: {len(drops)}")
    print(f"  DidChange->DidOpen upgrade: {len(upgrades)}")
    print(f"  publishDiagnostics with missing context: {len(missing_ctx)}")

    if drops:
        print("\nRecent DropDiagnostics lines:")
        for ln in drops[-8:]:
            print(f"  {ln}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
