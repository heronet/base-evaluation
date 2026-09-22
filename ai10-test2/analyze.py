#!/usr/bin/env python3
"""Convert T2A raw logs to CSV and report complete/failed/incomplete suites.

No synthetic results are inserted. Run numbers are scoped by boot/session.
"""
import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path

HEADER = ["session", "tag", "run", "case", "op", "kind", "name", "outcome", "rc",
          "value1", "value2", "uptime_ms"]
CASES = {"enroll_timeout", "identify_once_no_sample", "cancel_enroll_0ms",
         "cancel_enroll_1000ms", "cancel_identify_0ms", "cancel_identify_1000ms",
         "busy_identify"}
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def read_rows(path):
    session = 0
    rows = []
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        line = ANSI.sub("", line)
        if "*** Booting Zephyr OS" in line:
            session += 1
        position = line.find("T2,")
        if position < 0:
            continue
        fields = next(csv.reader([line[position:]]))
        if len(fields) != 11:
            raise ValueError(f"Malformed T2 row at line {line_number}: {line!r}")
        for index in (1, 3, 7, 8, 9, 10):
            int(fields[index])
        rows.append([session, *fields])
    if not rows:
        raise ValueError("No T2 rows found; this parser does not parse T1 logs.")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    rows = read_rows(args.log)
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("x", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(HEADER)
        writer.writerows(rows)
    suites = defaultdict(list)
    for values in rows:
        row = dict(zip(HEADER, values))
        suites[(row["session"], row["run"])].append(row)
    all_passed = True
    for (session, run), entries in suites.items():
        failures = [e for e in entries if e["outcome"] == "FAIL"]
        summaries = [e for e in entries if e["kind"] == "summary" and e["name"] == "suite"]
        case_rows = [e for e in entries if e["kind"] == "summary" and e["name"] == "case"]
        finished_cases = {e["case"] for e in case_rows if e["outcome"] == "PASS"}
        accepted = [e for e in entries if e["kind"] == "check" and e["name"] == "accepted"
                    and e["outcome"] == "PASS"]
        terminals = [e for e in entries if e["kind"] == "event" and e["name"] == "STOPPED"]
        accepted_ops = [e["op"] for e in accepted]
        terminal_ops = [e["op"] for e in terminals]
        complete = (len(summaries) == 1 and summaries[0]["outcome"] == "PASS"
                    and len(case_rows) == 7 and finished_cases == CASES
                    and len(accepted_ops) == 14 and len(set(accepted_ops)) == 14
                    and sorted(accepted_ops) == sorted(terminal_ops))
        result = "FAIL" if failures else "PASS" if complete else "INCOMPLETE"
        all_passed &= result == "PASS"
        print(f"session={session}, run={run}: {result}; "
              f"cases={len(finished_cases)}/7, accepted={len(accepted)}, "
              f"STOPPED={len(terminals)}, failed_rows={len(failures)}")
        for entry in failures:
            print(f"  {entry['case']} op={entry['op']} {entry['name']}: rc={entry['rc']}")
    print(f"Wrote {len(rows)} raw records to {args.csv}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
