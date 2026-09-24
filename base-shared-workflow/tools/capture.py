#!/usr/bin/env python3
"""Interactive serial console; retain raw received bytes without overwriting logs."""
import argparse
from pathlib import Path
import sys
import threading
import serial

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('port')
    p.add_argument('log', type=Path)
    p.add_argument('--baud', type=int, default=115200)
    a = p.parse_args()
    a.log.parent.mkdir(parents=True, exist_ok=True)
    with a.log.open('xb') as log:
        # Keep modem control deasserted to avoid an intentional reset on open.
        s = serial.Serial(port=None, baudrate=a.baud, timeout=0.1)
        s.dtr = False
        s.rts = False
        s.port = a.port
        s.open()
        def send():
            for line in sys.stdin:
                try:
                    s.write(line.rstrip('\r\n').encode() + b'\r')
                except (serial.SerialException, OSError):
                    return
        threading.Thread(target=send, daemon=True).start()
        print('Type shell commands. Ctrl-C closes capture; it does not cancel the device operation.',
              file=sys.stderr)
        try:
            while True:
                data = s.read(4096)
                if data:
                    log.write(data)
                    log.flush()
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
        except KeyboardInterrupt:
            pass
        finally:
            s.close()
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
