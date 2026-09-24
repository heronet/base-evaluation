#!/usr/bin/env python3
"""Build one shared source and retain the exact inputs; does not flash."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

APP = Path(__file__).resolve().parent

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile', choices=['zfm', 'gt5', 'ai10'])
    p.add_argument('--zephyr-base', type=Path, required=True)
    p.add_argument('--west-workspace', type=Path, required=True)
    p.add_argument('--tag', required=True)
    p.add_argument('--board', default='weact_esp32s3_b/esp32s3/procpu')
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', a.tag):
        p.error('tag must contain only letters, digits, hyphens and underscores')
    z, workspace = a.zephyr_base.resolve(), a.west_workspace.resolve()
    if not (workspace / '.west/config').is_file():
        p.error('--west-workspace must contain .west/config')
    header = z / 'include/zephyr/drivers/biometrics.h'
    if not header.is_file():
        p.error('--zephyr-base must point to the checkout containing the supplied API')
    build, evidence = APP / 'build' / a.tag, APP / 'results' / a.tag
    if build.exists() or evidence.exists():
        p.error('tag already exists; use another tag to preserve earlier runs')
    evidence.mkdir(parents=True)
    metadata = dict(tag=a.tag, profile=a.profile, board=a.board, status='INCOMPLETE')
    try:
        # Snapshot actual local sources, including uncommitted modifications.
        inputs = [header, z / 'drivers/biometrics', z / 'dts/bindings/biometrics']
        for src in inputs:
            dest = evidence / 'sources' / src.relative_to(z)
            dest.parent.mkdir(parents=True, exist_ok=True)
            if src.is_dir():
                shutil.copytree(src, dest)
            else:
                shutil.copy2(src, dest)
        for rel in ['src/main.c', 'CMakeLists.txt', 'Kconfig', 'prj.conf',
                    f'profiles/{a.profile}.overlay', f'profiles/{a.profile}.conf', 'build.py']:
            dst = evidence / 'application' / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(APP / rel, dst)
        ref = json.loads((APP / 'reviewed-sources.json').read_text())
        metadata['differences_from_supplied_zip'] = []
        for name, expected in ref.items():
            path = header if name == 'biometrics.h' else z / 'drivers/biometrics' / name
            if not path.is_file() or digest(path) != expected:
                metadata['differences_from_supplied_zip'].append(name)
        if metadata['differences_from_supplied_zip']:
            print('Source differs from the inspected ZIP:',
                  ', '.join(metadata['differences_from_supplied_zip']))
            print('The actual build inputs are saved; review changes before interpreting results.')
        git = subprocess.run(['git', '-C', str(z), 'rev-parse', 'HEAD'],
                             capture_output=True, text=True)
        metadata['zephyr_commit'] = git.stdout.strip() if git.returncode == 0 else None
        command = ['west', 'build', '-p', 'always', '-b', a.board, '-d', str(build), str(APP), '--',
                   f'-DZEPHYR_BASE={z}', f'-DDTC_OVERLAY_FILE={APP / "profiles" / (a.profile + ".overlay")}',
                   f'-DEXTRA_CONF_FILE={APP / "profiles" / (a.profile + ".conf")}',
                   f'-DBASE_BUILD_TAG={a.tag}']
        metadata['command'] = command
        env = os.environ.copy()
        with (evidence / 'build.log').open('x') as log:
            proc = subprocess.Popen(command, cwd=workspace, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, errors='replace')
            for line in proc.stdout:
                print(line, end='', flush=True)
                log.write(line)
            if proc.wait() != 0:
                raise RuntimeError('Build failed; see build.log')
        for name in ['.config', 'zephyr.dts', 'zephyr.elf', 'zephyr.map']:
            shutil.copy2(build / 'zephyr' / name,
                         evidence / ('zephyr.config' if name == '.config' else name))
        config = (evidence / 'zephyr.config').read_text().splitlines()
        driver = {'zfm': 'ZFM_X0', 'gt5': 'GT5X', 'ai10': 'AI10'}[a.profile]
        if f'CONFIG_BIOMETRICS_{driver}=y' not in config:
            raise RuntimeError('Selected driver was not enabled; inspect overlay and bindings')
        # Refuse to associate the saved inputs with a build made while they changed.
        for saved in (evidence / 'sources').rglob('*'):
            if saved.is_file():
                live = z / saved.relative_to(evidence / 'sources')
                if not live.is_file() or digest(saved) != digest(live):
                    raise RuntimeError('Source changed during the build; use a new tag and rebuild')
        for saved in (evidence / 'application').rglob('*'):
            if saved.is_file():
                live = APP / saved.relative_to(evidence / 'application')
                if not live.is_file() or digest(saved) != digest(live):
                    raise RuntimeError('Application changed during the build; use a new tag and rebuild')
        metadata['status'] = 'BUILD_COMPLETE'
    except (OSError, RuntimeError) as e:
        metadata['error'] = str(e)
        print(e, file=sys.stderr)
    (evidence / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    hashes = {str(f.relative_to(evidence)): digest(f)
              for f in sorted(evidence.rglob('*')) if f.is_file()}
    (evidence / 'sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    if metadata['status'] != 'BUILD_COMPLETE':
        return 1
    print('Build complete. Flash from your west workspace:')
    print('west flash -d', build)
    print('Capture log:', evidence / 'session.log')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
