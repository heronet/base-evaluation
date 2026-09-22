#!/usr/bin/env python3

import argparse
import csv
import pathlib
import re

parser = argparse.ArgumentParser()
parser.add_argument("log")
parser.add_argument("csv")
args = parser.parse_args()

ansi = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
records = []

for number, raw in enumerate(
    pathlib.Path(args.log).read_text(errors="replace").splitlines(),
    start=1,
):
    line = ansi.sub("", raw)
    start = line.find("T1,")
    if start < 0:
        continue

    fields = next(csv.reader([line[start:]]))
    if len(fields) != 11:
        raise SystemExit(f"Malformed T1 record at log line {number}: {line}")

    records.append(fields)

if not records:
    raise SystemExit("No T1 records found.")

with open(args.csv, "x", newline="") as output:
    writer = csv.writer(output)
    writer.writerow([
        "record", "run", "case", "step", "outcome", "rc",
        "id", "modality", "value1", "value2", "uptime_ms",
    ])
    writer.writerows(records)

print(f"Extracted {len(records)} records.")