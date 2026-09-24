#!/usr/bin/env python3
"""Check shared-workflow log completeness. Does not estimate a success probability."""
import argparse
import csv
import json
from pathlib import Path
import re
import sys

ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')

def analyze(text):
    runs, run = [], None
    for line in ANSI.sub('', text).splitlines():
        offset = line.find('BASE,')
        if offset < 0:
            continue
        try:
            row = next(csv.reader([line[offset:]]))
            seq, ms, kind = int(row[1]), int(row[2]), row[3]
            fields = row[4:]
        except (ValueError, IndexError, csv.Error):
            if run is not None:
                run['issues'].append('malformed_record')
            continue
        if kind == 'begin':
            if run is not None:
                run['issues'].append('missing_end')
            run = dict(tag=fields[0], records=[], issues=[], complete=False)
            runs.append(run)
        if run is None:
            continue
        if seq != len(run['records']) + 1:
            run['issues'].append('sequence_gap_or_duplicate')
        if run['records'] and ms < run['records'][-1]['ms']:
            run['issues'].append('clock_went_backwards')
        run['records'].append(dict(seq=seq, ms=ms, kind=kind, fields=fields))
        if kind in ['event_loss', 'extra_event', 'missing_original']:
            run['issues'].append(kind)
        if kind == 'end':
            run['complete'] = True
            run['reported_result'] = fields[0]
            if fields != ['PASS', '0']:
                run['issues'].append('firmware_reported_failure')
            recs = run['records']
            required = {
                'baseline_inventory': any(r['kind'] == 'inventory' and r['fields'][:2] == ['baseline', '0'] for r in recs),
                'recovery': any(r['kind'] == 'recovery' and r['fields'] == ['0'] for r in recs),
                'enrollment': any(r['kind'] == 'enrollment' and r['fields'][0] == '0' for r in recs),
                'identification': any(r['kind'] == 'identify' and r['fields'][0] == '0' for r in recs),
                'restoration': any(r['kind'] == 'restoration' and r['fields'] == ['final_cleanup', '0'] for r in recs),
            }
            run['issues'].extend('missing_' + k for k, v in required.items() if not v)
            run = None
    if run is not None:
        run['issues'].append('missing_end')
    for run in runs:
        run['valid_pass'] = run['complete'] and not run['issues']
        run['issues'] = sorted(set(run['issues']))
    return runs

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', type=Path, nargs='+')
    a = p.parse_args()
    results = [{'log': str(path), 'runs': analyze(path.read_text(errors='replace'))} for path in a.logs]
    print(json.dumps(results, indent=2))
    return 0 if all(x['runs'] and all(r['valid_pass'] for r in x['runs']) for x in results) else 1

if __name__ == '__main__':
    raise SystemExit(main())
