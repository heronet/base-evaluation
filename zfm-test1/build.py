#!/usr/bin/env python3
"""Build ZFM Test 1 and retain source/configuration evidence; never flash."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zephyr-base', type=Path, required=True)
    p.add_argument('--west-workspace', type=Path, required=True)
    p.add_argument('--board', default='weact_esp32s3_b/esp32s3/procpu')
    p.add_argument('--tag', default='zfm-t1-01')
    a = p.parse_args()
    if not a.tag or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in a.tag):
        p.error('Use letters, numbers, hyphens or underscores for the tag')
    app = Path(__file__).resolve().parent
    source, workspace = a.zephyr_base.resolve(), a.west_workspace.resolve()
    if not (workspace / '.west/config').is_file():
        p.error('--west-workspace must contain the existing .west/config')
    if not (source / 'include/zephyr/drivers/biometrics.h').is_file():
        p.error('--zephyr-base must point to your BASE checkout')
    build, out = app / 'build' / a.tag, app / 'results' / a.tag
    archive = out.with_suffix('.zip')
    if build.exists() or out.exists() or archive.exists():
        p.error('Tag already exists. Keep old evidence and choose a new tag.')
    out.mkdir(parents=True)
    metadata = {'test': 'ZFM Test 1', 'version': '1.0', 'board': a.board,
                'tag': a.tag, 'status': 'INCOMPLETE', 'zephyr_base': str(source),
                'west_workspace': str(workspace), 'python': sys.version,
                'discovery_zephyr_base': os.environ.get('ZEPHYR_BASE')}
    command = ['west', 'build', '-p', 'always', '-b', a.board, '-d', str(build), str(app), '--',
               f'-DZEPHYR_BASE={source}', f'-DDTC_OVERLAY_FILE={app / "app.overlay"}']
    metadata['command'] = command
    try:
        # Snapshot the input files before compilation, including uncommitted code.
        for rel in ('CMakeLists.txt', 'prj.conf', 'app.overlay', 'src/main.c', 'README.md', 'build.py'):
            dest = out / 'harness' / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            archive_source = app / rel
            if rel == 'README.md' and not archive_source.is_file():
                archive_source = app.parent / rel
            shutil.copy2(archive_source, dest)
        relevant = ('drivers/biometrics', 'include/zephyr/drivers/biometrics.h', 'dts/bindings/biometrics')
        for rel in relevant:
            src = source / rel
            dest = out / 'sources' / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            if src.is_dir():
                shutil.copytree(src, dest)
            else:
                shutil.copy2(src, dest)
        for key, cmd, cwd in (
            ('source_commit', ['git', 'rev-parse', 'HEAD'], source),
            ('source_status', ['git', 'status', '--short'], source),
            ('west_version', ['west', '--version'], workspace),
            ('manifest_freeze', ['west', 'manifest', '--freeze'], workspace),
            ('manifest_resolve', ['west', 'manifest', '--resolve'], workspace),
            ('module_revisions', ['west', 'list', '-f', '{name}\t{abspath}\t{sha}'], workspace)):
            try:
                r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=30)
                metadata[key] = {'returncode': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr}
            except (OSError, subprocess.TimeoutExpired) as e:
                metadata[key] = {'error': str(e)}
        with (out / 'build.log').open('x') as log:
            proc = subprocess.Popen(command, cwd=workspace, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, errors='replace')
            for line in proc.stdout:
                print(line, end='', flush=True); log.write(line); log.flush()
            rc = proc.wait()
        if rc:
            raise RuntimeError(f'Build failed ({rc}); inspect saved build.log')
        for name in ('zephyr.elf', 'zephyr.map', 'zephyr.dts', '.config'):
            shutil.copy2(build / 'zephyr' / name, out / ('zephyr.config' if name == '.config' else name))
        shutil.copy2(build / 'CMakeCache.txt', out / 'CMakeCache.txt')
        config = (out / 'zephyr.config').read_text()
        for flag in ('CONFIG_BIOMETRICS=y', 'CONFIG_BIOMETRICS_ZFM_X0=y', 'CONFIG_SHELL=y'):
            if flag not in config.splitlines():
                raise RuntimeError(f'Missing resolved setting: {flag}')
        metadata['status'] = 'BUILD_COMPLETE'
    except Exception as e:
        metadata['error'] = str(e)
        print('STOPPED:', e, file=sys.stderr)
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    hashes = {str(f.relative_to(out)): hashlib.sha256(f.read_bytes()).hexdigest()
              for f in sorted(out.rglob('*')) if f.is_file()}
    (out / 'sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(out.rglob('*')):
            if f.is_file(): z.write(f, f.relative_to(out.parent))
    print('Saved evidence:', archive)
    if metadata['status'] != 'BUILD_COMPLETE':
        return 1
    print('Build complete. From', workspace, 'run:')
    print('west flash -d', build)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
