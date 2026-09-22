#!/usr/bin/env python3

import argparse
import pathlib
import sys
import threading

import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--baud", type=int, default=115200)
parser.add_argument("--out", required=True)
args = parser.parse_args()

path = pathlib.Path(args.out)
path.parent.mkdir(parents=True, exist_ok=True)

stop = threading.Event()
file_lock = threading.Lock()

# Exclusive creation prevents accidental replacement of an earlier run.
with path.open("xb") as logfile:
    port = serial.Serial(
        port=None,
        baudrate=args.baud,
        timeout=0.1,
    )
    port.port = args.port
    port.dtr = False
    port.rts = False
    port.open()

    def receive():
        try:
            while not stop.is_set():
                data = port.read(4096)
                if not data:
                    continue
                with file_lock:
                    logfile.write(data)
                    logfile.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
        except (serial.SerialException, OSError) as exc:
            print(f"\nSerial reader stopped: {exc}", file=sys.stderr)
            stop.set()

    worker = threading.Thread(target=receive, daemon=True)
    worker.start()

    print("Type shell commands and press Enter. Ctrl-C ends capture.")

    try:
        while not stop.is_set():
            line = input()
            with file_lock:
                logfile.write(
                    ("\n#HOST_COMMAND " + line + "\n").encode()
                )
                logfile.flush()
            port.write((line + "\r\n").encode())
            port.flush()
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        worker.join(timeout=1)
        port.close()

print(f"\nSaved {path}")