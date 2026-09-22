#!/usr/bin/env python3
"""Build GT5 Test 3 and retain source/configuration evidence; never flash."""
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
    p.add_argument('--tag', default='gt5-t3-01')
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
    metadata = {'test': 'GT5 Test 3', 'version': '1.0', 'board': a.board,
                'tag': a.tag, 'status': 'INCOMPLETE', 'zephyr_base': str(source),
                'west_workspace': str(workspace), 'python': sys.version,
                'discovery_zephyr_base': os.environ.get('ZEPHYR_BASE')}
    command = ['west', 'build', '-p', 'always', '-b', a.board, '-d', str(build), str(app), '--',
               f'-DZEPHYR_BASE={source}', f'-DDTC_OVERLAY_FILE={app / "app.overlay"}']
    metadata['command'] = command
    try:
        expected = json.loads((app / 'input_sha256.json').read_text())['required_source_sha256']
        actual = {rel: hashlib.sha256((source / rel).read_bytes()).hexdigest() for rel in expected}
        metadata['source_hashes'] = actual
        differences = [rel for rel in expected if actual[rel] != expected[rel]]
        if differences:
            raise RuntimeError('Driver/API differs from the verified GT5 Test 1 build: '
                               + ', '.join(differences) + '. Send the changed files for review.')
        # Snapshot the input files before compilation, including uncommitted code.
        for rel in ('CMakeLists.txt', 'prj.conf', 'app.overlay', 'src/main.c', 'README.md', 'build.py', 'analyze.py', 'input_sha256.json'):
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
        cache = (out / 'CMakeCache.txt').read_text()
        import re
        base = re.search(r'^ZEPHYR_BASE:[^=]+=(.+)$', cache, re.MULTILINE)
        if base is None or Path(base.group(1).strip()).resolve() != source:
            raise RuntimeError('CMake cache does not match requested source tree')
        config = (out / 'zephyr.config').read_text()
        for flag in ('CONFIG_BIOMETRICS=y', 'CONFIG_BIOMETRICS_GT5X=y', 'CONFIG_SHELL=y', 'CONFIG_HEAP_MEM_POOL_SIZE=4096',
                     '# CONFIG_SMP is not set', '# CONFIG_PM is not set',
                     '# CONFIG_TIMESLICING is not set', '# CONFIG_LOG is not set'):
            if ((flag.startswith('CONFIG_') and flag not in config.splitlines()) or
                (flag.startswith('# ') and (flag.split()[1] + '=y') in config.splitlines())):
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
