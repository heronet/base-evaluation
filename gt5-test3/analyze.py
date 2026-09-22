#!/usr/bin/env python3
"""BASE GT5 T3 v1.0. Preserve invalid/incomplete runs; never overwrite output.

Standard library for CSV/JSON summaries; matplotlib only with --plots.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import sys

ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
MASK = (1 << 32) - 1
MODES = ("idle", "sync")
RUN_META = {"caps", "version", "clock", "period_ticks", "jobs_period_ms",
            "victim_calibration", "load_calibration", "priorities"}
CELL_META = {"condition", "load_percent", "window_cycles", "job_counts",
             "background_counts", "bio_counts", "bio_window_completed", "processed_jobs"}


def percentile(values, q):
    """Linear interpolation, same convention throughout the artifact."""
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    lo, hi = math.floor(position), math.ceil(position)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def parse(text):
    runs, errors, raw = {}, [], []
    session = 0
    for line_no, original in enumerate(text.splitlines(), 1):
        line = ANSI.sub("", original)
        if "*** Booting Zephyr OS build" in line:
            session += 1
        pos = line.find("G3,")
        if pos < 0:
            continue
        fields = next(csv.reader([line[pos:]]))
        raw.append({"line": line_no, "session": session, "fields": fields})
        try:
            kind = fields[1]
            if kind not in ("M", "V", "J") or len(fields) != (8 if kind == "J" else 7):
                raise ValueError("unknown record type or field count")
            run, cell = int(fields[2]), int(fields[3])
            if run == 0:  # t3 info; authoritative configuration also appears per run.
                continue
            if run < 1 or cell < 0 or cell > 8:
                raise ValueError("invalid run/cell")
            key = (session, run)
            item = runs.setdefault(key, {"meta": {}, "verdicts": {}, "cells": {}, "errors": []})
            if "suite" in item["verdicts"]:
                raise ValueError("records after suite end or duplicate run ID within boot")
            target = item if cell == 0 else item["cells"].setdefault(
                cell, {"meta": {}, "verdicts": {}, "jobs": []})
            if kind == "M":
                if fields[4] in target["meta"]:
                    raise ValueError("duplicate metadata")
                target["meta"][fields[4]] = [int(fields[5]), int(fields[6])]
            elif kind == "V":
                if fields[4] in target["verdicts"] or fields[5] not in ("PASS", "FAIL"):
                    raise ValueError("invalid or duplicate verdict")
                target["verdicts"][fields[4]] = [fields[5], int(fields[6])]
            elif cell == 0:
                raise ValueError("job outside a cell")
            else:
                target["jobs"].append([int(v) for v in fields[4:]])
        except (ValueError, IndexError) as exc:
            errors.append(f"line {line_no}: {exc}")
    if not runs:
        errors.append("No G3 runs found")
    return runs, errors, raw


def calculate(runs, parse_errors):
    summaries, jobs, statuses = [], [], []
    for (session, run), data in runs.items():
        errors = list(parse_errors)
        meta = data["meta"]
        missing = RUN_META - meta.keys()
        if missing:
            errors.append(f"missing run metadata: {sorted(missing)}")
        for name in ("setup", "suite"):
            if data["verdicts"].get(name) != ["PASS", 0]:
                errors.append(f"{name} missing or failed")
        if set(data["cells"]) != set(range(1, 9)):
            errors.append("expected exactly cells 1..8")
        run_summaries, run_jobs = [], []
        conditions = set()
        for cell, entry in sorted(data["cells"].items()):
            try:
                cm = entry["meta"]
                if CELL_META - cm.keys():
                    raise ValueError("missing cell metadata")
                if entry["verdicts"].get("cell") != ["PASS", 0]:
                    raise ValueError("cell failed or incomplete")
                hz, ticks_hz = meta["clock"]
                if not (0 < hz < MASK // 10 and ticks_hz > 0):
                    raise ValueError("unsupported clock configuration")
                if meta["version"] != [1, 0] or meta["caps"] != [1, 0] or meta["jobs_period_ms"] != [300, 10]:
                    raise ValueError("unexpected harness version/job configuration")
                if meta["priorities"] != [5, 4]:
                    raise ValueError("unexpected worker priorities")
                pticks, lticks = meta["period_ticks"]
                if pticks != math.ceil(ticks_hz * .010) or lticks != math.ceil(ticks_hz * .007):
                    raise ValueError("inconsistent timer configuration")
                for name, target in (("victim_calibration", 200), ("load_calibration", 3500)):
                    iterations, actual = meta[name]
                    if iterations <= 0 or not target * .8 <= actual <= target * 1.2:
                        raise ValueError("invalid calibration")
                mode, priority = cm["condition"]
                load, lp = cm["load_percent"]
                if mode not in range(2) or priority not in (2, 8) or load not in (0, 50) or lp != 4:
                    raise ValueError("invalid condition")
                condition = (mode, priority, load)
                if condition in conditions:
                    raise ValueError("duplicate condition")
                conditions.add(condition)
                group = (cell - 1) // 2
                expected = (((cell - 1) % 2 + run - 1 + group) % 2,
                            2 if group < 2 else 8, 50 if group % 2 else 0)
                if condition != expected:
                    raise ValueError("condition order differs from harness")
                if cm["job_counts"] != [300, 0] or len(entry["jobs"]) != 300:
                    raise ValueError("missing/extra/dropped jobs")
                if cm.get("processed_jobs") != [300, 0]:
                    raise ValueError("incomplete processing or receive errors")
                started, completed = cm["bio_counts"]
                in_window, bio_error = cm["bio_window_completed"]
                if bio_error or started != completed or started < 0:
                    raise ValueError("biometric progress check failed")
                if (mode == 0 and (started or in_window)) or (mode != 0 and not 0 < in_window <= completed):
                    raise ValueError("biometric workload absent/unexpected")
                load_releases, load_executed = cm["background_counts"]
                if not 0 <= load_executed <= load_releases:
                    raise ValueError("inconsistent background counters")
                if load == 0 and load_releases or load != 0 and load_executed == 0:
                    raise ValueError("background workload absent/unexpected")
                window, whz = cm["window_cycles"]
                if whz != hz or not 0 < window < hz * 9:
                    raise ValueError("invalid/beyond-watchdog window")
                first_release = entry["jobs"][0][1]
                responses, dispatches, spans, intervals, phases = [], [], [], [], []
                previous_release, previous_finish = None, 0
                local_jobs = []
                effective_period_us = pticks * 1e6 / ticks_hz
                for expected_seq, (seq, release, start, finish) in enumerate(entry["jobs"]):
                    if seq != expected_seq or any(not 0 <= x <= MASK for x in (release, start, finish)):
                        raise ValueError("invalid job index/counter")
                    r, s, f = [(x - first_release) & MASK for x in (release, start, finish)]
                    if not 0 <= r <= s <= f <= window or s < previous_finish:
                        raise ValueError("noncausal timestamps/counter wrap ambiguity")
                    if previous_release is not None and r <= previous_release:
                        raise ValueError("non-increasing releases")
                    dispatch = (s - r) * 1e6 / hz
                    response = (f - r) * 1e6 / hz
                    span = (f - s) * 1e6 / hz
                    interval = None if previous_release is None else (r - previous_release) * 1e6 / hz
                    phase = r * 1e6 / hz - seq * effective_period_us
                    dispatches.append(dispatch); responses.append(response); spans.append(span)
                    phases.append(phase)
                    if interval is not None:
                        intervals.append(interval - effective_period_us)
                    local_jobs.append({"session": session, "run": run, "cell": cell,
                        "mode": MODES[mode], "task_priority": priority, "requested_load_pct": load,
                        "job": seq, "release_cycle32": release, "start_cycle32": start,
                        "finish_cycle32": finish, "dispatch_us": dispatch, "response_us": response,
                        "execution_span_us": span, "release_interval_us": interval,
                        "release_phase_error_us": phase, "deadline_miss": int((f-r)*1000 > hz*10)})
                    previous_release, previous_finish = r, f
                result = {"session": session, "run": run, "cell": cell, "mode": MODES[mode],
                    "task_priority": priority, "requested_load_pct": load, "jobs": 300,
                    "effective_period_us": effective_period_us,
                    "calibrated_offered_load_pct": meta["load_calibration"][1] / (lticks*1e6/ticks_hz)*100 if load else 0,
                    "background_releases": load_releases, "background_executed": load_executed,
                    "bio_started": started, "bio_completed": completed, "bio_completed_in_window": in_window,
                    "deadline_misses": sum(j["deadline_miss"] for j in local_jobs),
                    "release_interval_error_min_us": min(intervals),
                    "release_interval_error_max_us": max(intervals),
                    "release_phase_error_min_us": min(phases), "release_phase_error_max_us": max(phases)}
                for name, values in (("dispatch", dispatches), ("response", responses), ("execution_span", spans)):
                    for label, q in (("p50", .5), ("p95", .95), ("p99", .99), ("max", 1)):
                        result[f"{name}_{label}_us"] = percentile(values, q)
                run_summaries.append(result)
                run_jobs.extend(local_jobs)
            except (ValueError, KeyError, IndexError, ZeroDivisionError) as exc:
                errors.append(f"cell {cell}: {exc}")
        if len(conditions) != 8:
            errors.append("incomplete condition matrix")
        valid = not errors
        for row in run_summaries + run_jobs:
            row["run_valid"] = valid
        summaries.extend(run_summaries); jobs.extend(run_jobs)
        statuses.append({"session": session, "run": run, "valid": valid,
                         "errors": errors, "metadata": meta})
    return summaries, jobs, statuses


def write_csv(path, rows):
    with path.open("x", newline="") as stream:
        if rows:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader(); writer.writerows(rows)


def make_plots(directory, summaries, jobs):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    colors = {"idle": "#555555", "sync": "#0072B2"}
    valid_jobs = [j for j in jobs if j["run_valid"]]
    valid_summaries = [s for s in summaries if s["run_valid"]]
    if not valid_jobs:
        return
    # Individual run ECDFs; jobs are not treated as independent run replicates.
    for metric, xlabel in (("dispatch_us", "Release-to-start latency (µs)"),
                           ("response_us", "Release-to-completion response (µs)")):
        fig, axes = plt.subplots(2, 2, figsize=(9, 6), constrained_layout=True)
        for ax, (priority, load) in zip(axes.flat, ((2, 0), (2, 50), (8, 0), (8, 50))):
            used = set()
            groups = {}
            for j in valid_jobs:
                if (j["task_priority"], j["requested_load_pct"]) == (priority, load):
                    groups.setdefault((j["session"], j["run"], j["mode"]), []).append(j[metric])
            for (_, _, mode), values in groups.items():
                values.sort()
                ax.step(values, [(i+1)/len(values) for i in range(len(values))], where="post",
                        color=colors[mode], alpha=.7, linewidth=1,
                        label=mode if mode not in used else None)
                used.add(mode)
            ax.set(title=f"Task priority {priority}; requested load {load}%", xlabel=xlabel, ylabel="ECDF")
            ax.grid(alpha=.2); ax.legend(fontsize=8)
        fig.savefig(directory / f"{metric}_ecdf.pdf")
        fig.savefig(directory / f"{metric}_ecdf.png", dpi=180)
        plt.close(fig)
    fig, axes = plt.subplots(2, 2, figsize=(9, 6), constrained_layout=True)
    for ax, (priority, load) in zip(axes.flat, ((2, 0), (2, 50), (8, 0), (8, 50))):
        for i, mode in enumerate(MODES):
            values = [s["response_p99_us"] for s in valid_summaries
                      if (s["mode"], s["task_priority"], s["requested_load_pct"]) == (mode, priority, load)]
            if values:
                offsets = [0] if len(values) == 1 else [-.12 + .24*j/(len(values)-1) for j in range(len(values))]
                ax.scatter([i+o for o in offsets], values, color=colors[mode], s=24)
        ax.set_xticks(range(2), MODES)
        ax.set(title=f"Task priority {priority}; requested load {load}%", ylabel="Per-run p99 response (µs)")
        ax.grid(axis="y", alpha=.2)
    fig.savefig(directory / "per_run_p99_response.pdf")
    fig.savefig(directory / "per_run_p99_response.png", dpi=180)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--out", required=True, type=Path, help="NEW output directory (never overwritten)")
    ap.add_argument("--plots", action="store_true", help="also produce PDF/PNG with matplotlib")
    args = ap.parse_args()
    text = args.log.read_text(errors="replace")
    runs, errors, raw = parse(text)
    summaries, jobs, statuses = calculate(runs, errors)
    try:
        args.out.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        ap.error("Output directory already exists; choose a new name")
    write_csv(args.out / "cell_summary.csv", summaries)
    write_csv(args.out / "jobs.csv", jobs)
    report = {"source": str(args.log), "parse_errors": errors, "runs": statuses, "raw_records": raw}
    (args.out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    if args.plots:
        make_plots(args.out, summaries, jobs)
    for status in statuses:
        print(f"Session {status['session']}, run {status['run']}: {'VALID' if status['valid'] else 'INVALID'}")
        for error in status["errors"]:
            print(f"  {error}")
    for error in errors:
        print(error, file=sys.stderr)
    print(f"Saved {args.out}")
    return 0 if statuses and not errors and all(s["valid"] for s in statuses) else 1


if __name__ == "__main__":
    raise SystemExit(main())
