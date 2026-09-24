# BASE evaluation

Test applications and recorded evidence for **BASE: A Capability-Driven Biometric Subsystem for Embedded Real-Time Systems**. Each application directory contains its sources, Zephyr configuration, overlay, and results. Run host commands from the repository root unless indicated otherwise.

## Tests and reported logs

Log names below are relative to each application's `results/` directory.

| Application   | Experiment                       | Console command                              | Recorded log               |
| ------------- | -------------------------------- | -------------------------------------------- | -------------------------- |
| `ai10-test1`  | E1: face and palm workflows      | `t1 run <n> face no` or `t1 run <n> palm no` | `ai10-t1-recorded-01.log`  |
| `gt5-test1`   | E1: staged fingerprint workflows | `g1 run <n>`                                 | `gt5-t1-recorded-03.log`   |
| `zfm-test1`   | E1: staged fingerprint workflows | `z1 run <n>`                                 | `zfm-t1-recorded-01.log`   |
| `ai10-test2`  | E2: lifecycle and recovery       | `t2 run <n>`                                 | `ai10-t2a-recorded-01.log` |
| `ai10-test2b` | E2: held terminal callback       | `t2b run <n>`                                | `ai10-t2b-recorded-01.log` |
| `ai10-test3`  | E3: scheduling                   | `t3 run <n>`                                 | `ai10-t3-recorded-01.log`  |
| `gt5-test3`   | E3: scheduling                   | `t3 run <n>`                                 | `gt5-t3-recorded-01.log`   |
| `zfm-test3`   | E3: scheduling                   | `t3 run <n>`                                 | `zfm-t3-recorded-01.log`   |
| `ai10-test4`  | E4: resources                    | Runs automatically at boot                   | `ai10-t4-profile-01.log`   |
| `gt5-test4`   | E4: resources                    | Runs automatically at boot                   | `gt5-t4-profile-01.log`    |
| `zfm-test4`   | E4: resources                    | Runs automatically at boot                   | `zfm-t4-profile-01.log`    |

The shared-application experiment (E1) is in [`base-shared-workflow/`](base-shared-workflow/). Four recorded runs cover ZFM-X0 fingerprint, GT-5X fingerprint, and AI10 face and palm, exercising interrupted enrollment, subsequent enrollment and identification, and inventory restoration. Its README provides build and run instructions.

Manual fingerprint attribute-check transcripts are retained as `manual-attribute-checks.log` in the `gt5-test1/results/` and `zfm-test1/results/` directories.

The device-specific E1 tests contain three completed workflows per fingerprint device and three each for AI10 face and palm. GT-5X's recorded workflow IDs are 3, 4, and 5. E2 contains three suites per application: 42 accepted operations in Test 2A and 12 in Test 2B, including recovery. E3 contains three suites per device: 10,800 AI10 jobs and 7,200 jobs for each fingerprint device, totaling 25,200 jobs.

All listed logs are preserved as complete files. E4 uses AI10 session 1 and GT-5X/ZFM-X0 session 2. The latter two files retain their incomplete first sessions.

## Setup

Experiments target the WeAct ESP32-S3-B (`weact_esp32s3_b/esp32s3/procpu`). Build metadata, resolved configurations, biometric API/driver/binding snapshots, and source hashes are retained with the recorded results. The corresponding build records identify the software used for each archived build. To reproduce an archived experiment, use the biometric sources under `sources/` in that experiment's build archive.

Connect one module at a time using the application's `app.overlay`. The overlays select UART1 with the board's `uart1_default` pins: connect module TX to board RX and module RX to board TX, with a common ground and the module's specified supply. Sensor baud rates are 115200 for AI10, 9600 for GT-5X, and 57600 for ZFM-X0. The console uses 115200 baud for all devices. Resolved pin assignments are recorded in the archived `zephyr.dts` files.

For firmware builds, activate an existing Zephyr Python environment with west, CMake 3.28+, Ninja, and the required toolchain. For log analysis alone, Python 3 is sufficient; Matplotlib is used for optional plots. Install the supporting capture, ELF-inspection, and plotting packages:

```sh
python -m pip install -r requirements.txt
BASE_REPO="$(pwd)"
BASE_ZEPHYR=/absolute/path/to/zephyr
BASE_WEST=/absolute/path/to/west-workspace
BASE_BOARD=weact_esp32s3_b/esp32s3/procpu
BASE_PORT=/dev/your-console-port
```

Set the paths for your machine. `BASE_WEST` contains `.west/config`; `BASE_ZEPHYR` is the source checkout to build. The repository can be outside that checkout.

## Build and run

Choose one of these build procedures, then use the shared flash/capture command below.

**AI10 Tests 1, 2, 2B, and 3:** select the application and build directly:

```sh
BASE_APP=ai10-test2
BASE_BUILD="$BASE_REPO/$BASE_APP/build/manual"
(
  cd "$BASE_WEST"
  west build -p always -b "$BASE_BOARD" -d "$BASE_BUILD" "$BASE_REPO/$BASE_APP" -- \
    -DZEPHYR_BASE="$BASE_ZEPHYR" -DDTC_OVERLAY_FILE="$BASE_REPO/$BASE_APP/app.overlay"
)
```

**Fingerprint Tests 1 and 3:** `gt5-test1`, `gt5-test3`, `zfm-test1`, and `zfm-test3` provide `build.py`. The helper builds the application and saves its build evidence as `results/<tag>.zip`:

```sh
BASE_APP=zfm-test1
BASE_TAG=zfm-t1-new-01
BASE_BUILD="$BASE_REPO/$BASE_APP/build/$BASE_TAG"
python "$BASE_REPO/$BASE_APP/build.py" --board "$BASE_BOARD" \
  --zephyr-base "$BASE_ZEPHYR" --west-workspace "$BASE_WEST" --tag "$BASE_TAG"
```

**Resource tests:** choose `ai10-test4`, `gt5-test4`, or `zfm-test4`. `build_matrix.py` builds `baseline`, `idle`, `sync`, and `profile`; AI10 also has `async`. It saves reports and build evidence as `results/<tag>.zip`. Flash the profile for stack measurements:

```sh
BASE_APP=ai10-test4
BASE_TAG=ai10-t4-new-01
BASE_BUILD="$BASE_REPO/$BASE_APP/build/$BASE_TAG/profile"
(
  cd "$BASE_REPO/$BASE_APP"
  python build_matrix.py --board "$BASE_BOARD" \
    --zephyr-base "$BASE_ZEPHYR" --west-workspace "$BASE_WEST" --tag "$BASE_TAG"
)
```

Use a fresh build tag and log filename for each new batch. After a successful build, flash and capture:

```sh
(
  cd "$BASE_WEST"
  west flash -d "$BASE_BUILD"
)
python "$BASE_REPO/ai10-test1/capture.py" --port "$BASE_PORT" --baud 115200 \
  --out "$BASE_REPO/$BASE_APP/results/new-session-01.log"
```

Close other serial monitors first. Press reset once after capture starts to record the boot; Ctrl-C ends capture. For interactive tests, enter the command from the table, replacing `<n>` with 1, 2, and 3 in succession. Wait for each summary before starting the next run. Use the matching `info` command (`t1 info`, `g1 info`, `z1 info`, `t2 info`, `t2b info`, or `t3 info`) to inspect the configuration.

- **E1:** follow the presentation prompts. Use a previously unenrolled finger for the fingerprint tests. AI10 requires three face trials and three palm trials, keeping other modalities outside its view. Successful trials remove their test template and check the initial ID inventory. After a failed trial, inspect the log and inventory before retrying.
- **E2:** keep faces and palms outside the module's view. Test 2A checks timeout, cancellation, conflicting operations, and recovery. Test 2B deliberately holds `STOPPED` and checks exclusion and stop completion using added 100 ms and 500 ms observation windows. Interpret its stop timings separately from Test 2A.
- **E3:** present no biometric sample and avoid console input during measurement. Each suite runs 12 conditions for AI10 or eight for a fingerprint device, with 300 jobs per condition. Keep CPU frequency and the recorded calibration consistent across a batch.
- **E4:** no shell commands are used. Keep samples away and wait for `T4,V,suite,PASS,0`, `G4,V,suite,PASS,0`, or `Z4,V,suite,PASS,0` before ending capture.

## Analyze the recorded data

These commands run from the repository root and require no connected hardware. Use new output names if rerunning them.

```sh
mkdir -p analysis
python ai10-test1/to_csv.py ai10-test1/results/ai10-t1-recorded-01.log analysis/ai10-e1.csv
python ai10-test2/analyze.py ai10-test2/results/ai10-t2a-recorded-01.log analysis/ai10-e2a.csv
python ai10-test2b/analyze.py ai10-test2b/results/ai10-t2b-recorded-01.log analysis/ai10-e2b.csv

for sensor in ai10 gt5 zfm; do
  python "$sensor-test3/analyze.py" "$sensor-test3/results/$sensor-t3-recorded-01.log" \
    --out "analysis/$sensor-e3"
done
```

The fingerprint E1 logs contain `G1` and `Z1` check and summary records; they use a different format from AI10's CSV converter. E2 analyzers report validation and accepted-operation/terminal-event counts. E3 produces `validation.json`, `jobs.csv`, and `cell_summary.csv`; add `--plots` to produce per-device plots. Use valid complete runs, and keep suite repetitions separate. Response time is measured from the recorded timer-callback release to job completion; p99 is calculated per 300-job condition.

Run the resource analyzers individually:

```sh
python ai10-test4/analyze_profile.py ai10-test4/results/ai10-t4-profile-01.log --out analysis/ai10-e4
python gt5-test4/analyze_profile.py gt5-test4/results/gt5-t4-profile-01.log --out analysis/gt5-e4
python zfm-test4/analyze_profile.py zfm-test4/results/zfm-t4-profile-01.log --out analysis/zfm-e4
```

These produce `validation.json` and `stacks.csv`. The GT-5X and ZFM-X0 commands return status 1 because their logs include incomplete first sessions. Their second sessions validate successfully. Use rows with `session_valid=True`; do not combine partial sessions.

## Build records and resource accounting

The seven ZIPs under `results/` retain selected records from the evaluated builds: native resource reports where available, configurations, devicetrees, build/toolchain metadata, and biometric API/driver/binding snapshots. Bulky ELF/map files and duplicate application copies are omitted. `sha256.json` checks the retained files; `original_sha256.json` preserves the original archive's file hashes. Newly collected build ZIPs may contain additional files.

To inspect E4 reports:

```sh
mkdir -p analysis/builds
for sensor in ai10 gt5 zfm; do
  python -m zipfile -e "$sensor-test4/results/$sensor-t4-01.zip" analysis/builds
done
```

Read `rom_report.txt` and `ram_report.txt` in each variant directory and subtract the matching `baseline` totals. Use the `profile` variant for stack measurements separately from the linked-image comparison. `driver_layout.json` and `driver_symbols.csv` preserve object-layout and symbol attribution. AI10's worker storage is already present in idle and synchronous builds; GT-5X allocations consume heap storage reserved in the image. Stack watermarks are cumulative across startup and the exercised paths.
