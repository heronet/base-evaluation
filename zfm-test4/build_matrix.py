#!/usr/bin/env python3
"""Build/archive Test 4 without flashing. Run inside the user's Zephyr workspace."""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import zipfile
from elf_details import inspect_elf

VARIANTS = ("baseline", "idle", "sync", "profile")


def config_read(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            result[line[2:-11]] = "n"
    return result


def validate_config(variant, config, baseline_config=None):
    """Validate experimental controls against resolved, architecture-aware values."""
    expected = {"CONFIG_T4_" + variant.upper(): "y", "CONFIG_SERIAL": "y",
                "CONFIG_UART_INTERRUPT_DRIVEN": "y", "CONFIG_MAIN_STACK_SIZE": "4096",
                "CONFIG_HEAP_MEM_POOL_SIZE": "0", "CONFIG_SMP": "n", "CONFIG_SHELL": "n",
                "CONFIG_LOG": "n", "CONFIG_PM": "n", "CONFIG_TIMESLICING": "n",
                "CONFIG_BIOMETRICS": "n" if variant == "baseline" else "y",
                "CONFIG_SIZE_OPTIMIZATIONS": "y",
                "CONFIG_INIT_STACKS": "y" if variant == "profile" else "n"}
    if variant == "profile":
        expected["CONFIG_THREAD_STACK_INFO"] = "y"
    elif baseline_config is not None:
        # Architecture-selected bookkeeping is allowed, but must be matched.
        expected["CONFIG_THREAD_STACK_INFO"] = baseline_config.get("CONFIG_THREAD_STACK_INFO", "n")
    if variant != "baseline":
        expected.update(CONFIG_BIOMETRICS_ZFM_X0="y")
    bad = {k: {"expected": value, "actual": config.get(k, "n")}
           for k, value in expected.items() if config.get(k, "n") != value}
    if bad:
        raise RuntimeError(f"Resolved configuration mismatch in {variant}: {bad}")


def run_logged(command, path, cwd, env=None):
    print("Running:", shlex.join(str(x) for x in command), flush=True)
    with Path(path).open("x") as log:
        proc = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace")
        for line in proc.stdout:
            log.write(line); log.flush()
            print(line, end="", flush=True)
        rc = proc.wait()
    if rc:
        raise RuntimeError(f"Command failed ({rc}); retain {path} and inspect it")


def capture(command, cwd):
    try:
        return subprocess.check_output(command, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"{command} failed ({error.returncode}):\n{error.output}") from error


def optional_command(command, cwd, env=None):
    """Read-only metadata collection must not prevent the actual build."""
    try:
        result = subprocess.run(command, cwd=cwd, env=env, text=True, errors="replace",
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        return {"command": command, "returncode": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": command, "returncode": None, "stdout": "", "stderr": str(error)}


def collect_manifest(results, app, env):
    """Preserve freeze errors and available revisions; never update the workspace."""
    freeze = optional_command(["west", "manifest", "--freeze"], app, env)
    (results / "west-manifest-freeze.json").write_text(json.dumps(freeze, indent=2) + "\n")
    metadata = {"freeze_status": "OK" if freeze["returncode"] == 0 else "FAILED"}
    if freeze["returncode"] == 0:
        (results / "west-manifest.yml").write_text(freeze["stdout"])
        return metadata

    print("WARNING: west manifest --freeze failed; continuing with the build matrix.", flush=True)
    print((freeze["stderr"] or freeze["stdout"] or "No error output returned").strip(), flush=True)
    print("The evidence archive will mark the frozen manifest as unavailable.", flush=True)
    resolved = optional_command(["west", "manifest", "--resolve"], app, env)
    (results / "west-manifest-resolve.json").write_text(json.dumps(resolved, indent=2) + "\n")
    metadata["resolve_status"] = "OK" if resolved["returncode"] == 0 else "FAILED"
    if resolved["returncode"] == 0:
        (results / "west-manifest-resolved.yml").write_text(resolved["stdout"])
    projects = optional_command(["west", "list", "-f", "{name}\t{abspath}"], app, env)
    (results / "west-project-list.json").write_text(json.dumps(projects, indent=2) + "\n")
    metadata["project_list_status"] = "OK" if projects["returncode"] == 0 else "FAILED"
    revisions = []
    if projects["returncode"] == 0:
        for line in projects["stdout"].splitlines():
            if "\t" not in line:
                continue
            name, directory = line.split("\t", 1)
            path = Path(directory)
            entry = {"name": name, "path": directory}
            # Do not let git walk up to a parent repository for an uncloned project.
            if not (path / ".git").exists():
                entry["status"] = "NO_LOCAL_GIT_CHECKOUT"
            else:
                head = optional_command(["git", "rev-parse", "HEAD"], path)
                entry["head"] = head
                entry["status"] = "OK" if head["returncode"] == 0 else "FAILED"
                entry["worktree_status"] = optional_command(["git", "status", "--porcelain"], path)
            revisions.append(entry)
    (results / "module-revisions.json").write_text(json.dumps(revisions, indent=2) + "\n")
    metadata["note"] = "Frozen manifest unavailable. Fallback metadata may be partial; " \
                       "review saved errors and module revisions before publishing reproducibility claims."
    return metadata


def archive_tree(folder):
    target = folder.with_suffix(".zip")
    with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED) as archive:
        for p in sorted(folder.rglob("*")):
            if p.is_file():
                archive.write(p, p.relative_to(folder.parent))
    return target


def workspace_context(app, requested_workspace):
    """Discover west separately from the external source checkout being built."""
    origin = requested_workspace.resolve() if requested_workspace else app
    if requested_workspace and not (origin / ".west/config").is_file():
        raise RuntimeError(f"{origin} does not contain .west/config; pass the existing west workspace root")
    workspace = Path(capture(["west", "topdir"], origin)).resolve()
    if not (workspace / ".west/config").is_file():
        raise RuntimeError(f"west returned an invalid workspace root: {workspace}")
    # Preserve the user's working discovery environment. The requested source
    # is passed separately as -DZEPHYR_BASE to CMake, just as in manual builds.
    return workspace, dict(os.environ)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--board", required=True, help="same full board target as Tests 2/3")
    ap.add_argument("--overlay", type=Path, default=Path("app.overlay"))
    ap.add_argument("--tag", default="zfm-t4-01", help="new identifier; prevents overwriting")
    ap.add_argument("--zephyr-base", type=Path, help="source checkout to build; may be outside the workspace")
    ap.add_argument("--west-workspace", type=Path, help="existing workspace root containing .west/config")
    args = ap.parse_args()
    app = Path(__file__).resolve().parent
    overlay = args.overlay.resolve()
    if not overlay.is_file():
        ap.error("Copy the working ZFM overlay to app.overlay first")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.tag):
        ap.error("tag must contain only letters, numbers, underscores or hyphens")
    if not shutil.which("west"):
        ap.error("west is unavailable; activate your Zephyr Python environment")
    builds = app / "build" / args.tag
    results = app / "results" / args.tag
    if builds.exists() or results.exists() or results.with_suffix(".zip").exists():
        ap.error("That tag already exists; choose a NEW tag (failed attempts are retained)")
    results.mkdir(parents=True)
    builds.mkdir(parents=True)
    metadata = {"sensor": "zfm_x0", "harness_version": "1.0", "collector_version": "1.3", "board": args.board, "tag": args.tag,
                "status": "INCOMPLETE", "python": sys.version,
                "pyelftools": importlib.metadata.version("pyelftools"), "variants": []}
    try:
        workspace, build_env = workspace_context(app, args.west_workspace)
        manifest_repo = (workspace / capture(["west", "config", "manifest.path"], workspace)).resolve()
        metadata["west_workspace"] = str(workspace)
        metadata["workspace_manifest_repository"] = str(manifest_repo)
        metadata["west_discovery_zephyr_base"] = build_env.get("ZEPHYR_BASE")
        if args.zephyr_base:
            zephyr = args.zephyr_base.resolve()
        else:
            zephyr = manifest_repo
        if not (zephyr / "include/zephyr/drivers/biometrics.h").is_file():
            raise RuntimeError("Cannot locate your BASE API; pass --zephyr-base /path/to/your/zephyr")
        metadata["zephyr_base"] = str(zephyr)
        metadata["source_is_manifest_repository"] = zephyr == manifest_repo
        metadata["zephyr_commit"] = capture(["git", "rev-parse", "HEAD"], zephyr)
        metadata["west_version"] = capture(["west", "--version"], workspace)
        (results / "west-build-help.txt").write_text(capture(["west", "build", "--help"], workspace) + "\n")
        metadata["manifest"] = collect_manifest(results, workspace, build_env)
        relevant = ["drivers/biometrics", "include/zephyr/drivers/biometrics.h", "dts/bindings/biometrics"]
        (results / "source-status.txt").write_text(capture(["git", "status", "--short", "--", *relevant], zephyr) + "\n")
        sources = results / "sources"
        source_files = [zephyr / "include/zephyr/drivers/biometrics.h"]
        for directory in (zephyr / "drivers/biometrics", zephyr / "dts/bindings/biometrics"):
            source_files.extend(p for p in directory.rglob("*") if p.is_file())
        for src in source_files:
            dest = sources / src.relative_to(zephyr)
            dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src, dest)
        for relative in ("prj.conf", "Kconfig", "CMakeLists.txt", "src/main.c", "README.md",
                         "build_matrix.py", "elf_details.py", "analyze_profile.py", "requirements.txt"):
            dest = results / "harness" / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            archive_source = app / relative
            if relative == "README.md" and not archive_source.is_file():
                archive_source = app.parent / relative
            shutil.copy2(archive_source, dest)
        shutil.copytree(app / "variants", results / "harness/variants")
        shutil.copy2(overlay, results / "app.overlay")
        configs = {}
        for variant in VARIANTS:
            build = builds / variant
            out = results / variant
            out.mkdir()
            command = ["west", "build", "-p", "always", "-b", args.board, "-d", str(build), str(app), "--",
                       f"-DZEPHYR_BASE={zephyr}",
                       f"-DCONF_FILE={app / 'prj.conf'};{app / 'variants' / (variant + '.conf')}",
                       f"-DDTC_OVERLAY_FILE={overlay}"]
            run_logged(command, out / "build.log", workspace, build_env)
            cache = (build / "CMakeCache.txt").read_text()
            actual_base = re.search(r"^ZEPHYR_BASE:[^=]+=(.+)$", cache, re.MULTILINE)
            if actual_base is None or Path(actual_base.group(1).strip()).resolve() != zephyr:
                raise RuntimeError("Build did not confirm the requested ZEPHYR_BASE in its CMake cache")
            config = config_read(build / "zephyr/.config")
            configs[variant] = config
            for filename in ("zephyr.elf", "zephyr.map", "zephyr.dts"):
                shutil.copy2(build / "zephyr" / filename, out / filename)
            shutil.copy2(build / "zephyr/.config", out / "zephyr.config")
            shutil.copy2(build / "CMakeCache.txt", out / "CMakeCache.txt")
            validate_config(variant, config, configs.get("baseline"))
            details = inspect_elf(out / "zephyr.elf", out / "driver_symbols.csv")
            (out / "driver_layout.json").write_text(json.dumps(details, indent=2) + "\n")
            if not details["has_dwarf_info"]:
                raise RuntimeError(f"Missing DWARF in {variant}; check that CMakeLists.txt retains zephyr_compile_options(-g)")
            if variant == "baseline" and details["driver_data_objects"]:
                raise RuntimeError("Baseline unexpectedly contains ZFM driver data")
            if variant != "baseline" and len(details["driver_data_objects"]) != 1:
                raise RuntimeError("Expected exactly one ZFM instance; inspect overlay/symbols")
            # Keep native totals and attribution; do not guess ESP32 RAM by ELF flags.
            for report in ("rom_report", "ram_report"):
                run_logged(["west", "build", "-d", str(build), "-t", report], out / (report + ".txt"), workspace, build_env)
            for json_file in ("rom.json", "ram.json"):
                for parent in (build, build / "zephyr"):
                    if (parent / json_file).is_file():
                        shutil.copy2(parent / json_file, out / json_file); break
            metadata["variants"].append({"name": variant, "build_dir": str(build),
                                         "instrumented": variant == "profile", "build_command": command})
        differences = {}
        base = configs["baseline"]
        for variant, config in configs.items():
            differences[variant] = {k: {"baseline": base.get(k, "n"), "variant": config.get(k, "n")}
                                    for k in sorted(base.keys() | config.keys())
                                    if base.get(k, "n") != config.get(k, "n")}
        (results / "config_differences.json").write_text(json.dumps(differences, indent=2) + "\n")
        metadata["status"] = "BUILDS_COMPLETE"
    except Exception as error:
        metadata["error"] = str(error)
        print(f"STOPPED: {error}", file=sys.stderr)
    (results / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    hashes = {str(p.relative_to(results)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(results.rglob("*")) if p.is_file()}
    (results / "sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    archive = archive_tree(results)
    print(f"\nSaved evidence: {archive}")
    if metadata["status"] != "BUILDS_COMPLETE":
        return 1
    print("All builds/reports complete. No board has been flashed.")
    print(f"Next, from workspace {workspace}: west flash -d {builds / 'profile'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
