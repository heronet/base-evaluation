# Shared biometric workflow

This application evaluates reuse of one application source across ZFM-X0,
GT-5X, and AI10 biometric drivers. It supplements E1 in the BASE paper.

## Requirements

- A configured Zephyr workspace and toolchain.
- The BASE biometric API and drivers:
  https://github.com/zephyrproject-rtos/zephyr/pull/119773
- WeAct ESP32-S3-B and the selected biometric module.
- Python dependencies: `python -m pip install -r requirements.txt`

Evaluated source snapshots and resolved configurations are retained in
`results/`. The development PR may contain later changes.

## Build and run

From this directory:

```sh
python build.py zfm \
  --zephyr-base /path/to/zephyr \
  --west-workspace /path/to/workspace \
  --tag zfm-shared-01

west flash -d build/zfm-shared-01

python tools/capture.py SERIAL_PORT results/zfm-shared-01/session.log
```

Select the profile and device-shell command:

| Device    | Profile | Shell command          |
| --------- | ------- | ---------------------- |
| ZFM-X0    | `zfm`   | `base run fingerprint` |
| GT-5X     | `gt5`   | `base run fingerprint` |
| AI10 face | `ai10`  | `base run face`        |
| AI10 palm | `ai10`  | `base run palm`        |

Use a corresponding build tag for each profile. AI10 face and palm use
the same firmware; save their console sessions separately. UART settings
and device properties are defined in `profiles/`.

## Procedure

The application records the initial inventory, interrupts enrollment,
checks recovery, completes a fresh enrollment, identifies the new record,
and restores the initial template-ID set.

For staged devices, interruption follows one capture. For AI10, stop is
requested without presenting a sample. Keep face and palm out of view
until the subsequent enrollment prompt. Use a sample not already enrolled.

Run with exclusive device ownership. Cleanup deletes only IDs absent from
the initial inventory. Failed runs may require manual reconciliation.
Inventory restoration checks IDs, not template contents.

## Results

Retain complete logs, including failed attempts. Analyze a session with:

```sh
python tools/analyze.py results/zfm-shared-01/session.log
```

A passing run requires recovery, enrollment, correct identification,
inventory restoration, and complete record numbering. Missing end records
are incomplete runs.

`build.py` archives source and configuration inputs with each build.
These demonstrations establish the exercised shared workflows; they do
not measure recognition accuracy or validate protocol-failure races.
