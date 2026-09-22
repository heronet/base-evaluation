#!/usr/bin/env python3
"""Validate ZFM Test 4 stack profiles; values are cumulative observed watermarks."""
import argparse
import csv
import json
from pathlib import Path
import re

PHASES = ("sync_timeout",)
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def parse_profile(text):
    sessions = []
    current = None
    errors = []
    for line_no, original in enumerate(text.splitlines(), 1):
        line = ANSI.sub("", original)
        pos = line.find("Z4,")
        if pos < 0:
            continue
        fields = next(csv.reader([line[pos:]]))
        if fields[:3] == ["Z4", "M", "version"]:
            current = {"index": len(sessions) + 1, "version": fields[3:], "operations": [],
                       "stacks": [], "verdicts": [], "metadata": {}, "errors": []}
            sessions.append(current)
            continue
        if current is None:
            errors.append(f"line {line_no}: record before version/start")
            continue
        try:
            kind = fields[1]
            if current["verdicts"]:
                raise ValueError("record after suite verdict")
            if kind == "M":
                if fields[2] in current["metadata"]:
                    raise ValueError("duplicate metadata")
                current["metadata"][fields[2]] = fields[3:]
            elif kind == "S" and len(fields) == 8:
                rc, usable, unused, used = map(int, fields[4:])
                current["stacks"].append({"session": current["index"], "phase": fields[2],
                    "role": fields[3], "rc": rc, "usable_bytes": usable,
                    "unused_bytes": unused, "observed_used_bytes": used})
            elif kind == "O" and len(fields) == 6:
                current["operations"].append((fields[2], int(fields[3]), fields[4], int(fields[5])))
            elif kind == "V" and len(fields) == 5:
                current["verdicts"].append(fields[2:])
            else:
                raise ValueError("unknown/malformed record")
        except (ValueError, IndexError) as error:
            current["errors"].append(f"line {line_no}: {error}")
    if not sessions:
        errors.append("No profile session found")
    for session in sessions:
        problems = session["errors"]
        problems.extend(errors)
        if session["version"] != ["1", "0"] or session["metadata"].get("variant") != ["profile"]:
            problems.append("wrong version or variant")
        if session["metadata"].get("stack_config") != ["4096", "0"]:
            problems.append("unexpected configured stack sizes")
        if session["metadata"].get("caps") != ["1", "0"]:
            problems.append("unexpected ZFM capabilities")
        expected_ops = [(p, n, "PASS", 0) for p in PHASES for n in range(1, 4)]
        if session["operations"] != expected_ops:
            problems.append("missing, failed, duplicate, or out-of-order operations")
        if session["verdicts"] != [["suite", "PASS", "0"]]:
            problems.append("suite did not complete successfully")
        stack_keys = [(s["phase"], s["role"]) for s in session["stacks"]]
        expected_order = [("before_operations", "app_main"), ("sync_timeout", "app_main")]
        if stack_keys != expected_order:
            problems.append("missing, duplicate, or out-of-order stack observations")
        previous = {}
        for s in session["stacks"]:
            if s["rc"] != 0 or not 0 <= s["unused_bytes"] <= s["usable_bytes"] or s["usable_bytes"] <= 0:
                problems.append("failed or invalid stack measurement")
            if s["observed_used_bytes"] != s["usable_bytes"] - s["unused_bytes"]:
                problems.append("inconsistent stack arithmetic")
            prev = previous.get(s["role"])
            if prev and (s["usable_bytes"] != prev[0] or s["observed_used_bytes"] < prev[1]):
                problems.append("stack size changed or cumulative watermark decreased")
            previous[s["role"]] = (s["usable_bytes"], s["observed_used_bytes"])
        session["valid"] = not problems
        for s in session["stacks"]:
            s["session_valid"] = session["valid"]
    return sessions, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--out", required=True, type=Path, help="NEW output directory")
    args = ap.parse_args()
    sessions, errors = parse_profile(args.log.read_text(errors="replace"))
    try:
        args.out.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        ap.error("Output exists; choose a fresh directory")
    report = {"source": str(args.log), "errors": errors, "sessions": sessions}
    (args.out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    rows = [s for session in sessions for s in session["stacks"]]
    with (args.out / "stacks.csv").open("x", newline="") as f:
        if rows:
            w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    for session in sessions:
        print(f"Session {session['index']}: {'VALID' if session['valid'] else 'INVALID'}")
        for error in session["errors"]:
            print(f"  {error}")
    return 0 if sessions and not errors and all(s["valid"] for s in sessions) else 1


if __name__ == "__main__":
    raise SystemExit(main())
