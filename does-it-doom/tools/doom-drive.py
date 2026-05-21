#!/usr/bin/env python2
# coding: utf-8
"""Drive the emery emulator with scripted button input via the QEMU
transport. Intended to run *inside* the pebble-sdk Docker container
(uses libpebble2 from /opt/pebble-sdk-4.5-linux64/.env/...).

Usage:
  python2 doom-drive.py --press select --hold-ms 100
  python2 doom-drive.py --script tests/inputs/title_to_play.txt
  python2 doom-drive.py --press select up --hold-ms 50 --wait-ms 200

Script file format (one event per line):
  <button>[,<button>...]  <hold-ms>  <wait-ms>
e.g.:
  select  100  500           # tap SELECT, wait 500ms after release
  up      500  200           # hold UP 500ms, wait 200ms
  up,back 200  100           # hold UP+BACK together (strafe), 200ms

Buttons: back, up, select, down.
"""
from __future__ import print_function
import argparse
import sys
import time

# Inside the container the SDK's python2 site-packages aren't on default path.
sys.path.insert(
    0, "/opt/pebble-sdk-4.5-linux64/.env/local/lib/python2.7/site-packages"
)

import json
import os

from libpebble2.communication.transports.qemu import (
    QemuTransport, MessageTargetQemu,
)
from libpebble2.communication.transports.qemu.protocol import QemuButton


def discover_qemu_port(default_port):
    """pebble-tool writes the live emulator's port to /tmp/pb-emulator.json.
    Read it so doom-drive doesn't depend on a hard-coded port that's
    actually ephemeral. Structure is nested:
      {"<platform>": {"<sdk_version>": {"qemu": {"port": N, ...}}}}
    Walk the tree for any qemu.port we can find."""
    path = "/tmp/pb-emulator.json"
    if not os.path.exists(path):
        return default_port
    try:
        with open(path) as f:
            data = json.load(f)
        # platform -> sdk_version -> qemu -> port
        for _platform, by_version in data.items():
            if not isinstance(by_version, dict):
                continue
            for _ver, info in by_version.items():
                if not isinstance(info, dict):
                    continue
                qemu = info.get("qemu")
                if isinstance(qemu, dict) and "port" in qemu:
                    return int(qemu["port"])
    except (ValueError, IOError, KeyError):
        pass
    return default_port

BUTTON_BITS = {
    "back":   1,
    "up":     2,
    "select": 4,
    "down":   8,
}


def press(transport, names, hold_ms):
    state = 0
    for n in names:
        if n not in BUTTON_BITS:
            raise SystemExit("unknown button: %s" % n)
        state |= BUTTON_BITS[n]
    transport.send_packet(QemuButton(state=state), target=MessageTargetQemu())
    time.sleep(hold_ms / 1000.0)
    transport.send_packet(QemuButton(state=0), target=MessageTargetQemu())


def run_script(transport, path):
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            btns = parts[0].split(",")
            hold = int(parts[1])
            wait = int(parts[2])
            press(transport, btns, hold)
            time.sleep(wait / 1000.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", default=None, type=int,
                    help="QEMU port; defaults to the one in /tmp/pb-emulator.json")
    ap.add_argument("--press", nargs="+",
                    help="one or more buttons to press together")
    ap.add_argument("--hold-ms", default=100, type=int)
    ap.add_argument("--script", help="path to a script file")
    args = ap.parse_args()

    port = args.port if args.port is not None else discover_qemu_port(12344)
    print("connecting to qemu %s:%d" % (args.host, port))
    transport = QemuTransport(args.host, port)
    transport.connect()
    time.sleep(0.2)
    try:
        if args.press:
            press(transport, [b.lower() for b in args.press], args.hold_ms)
        if args.script:
            run_script(transport, args.script)
    finally:
        time.sleep(0.2)
        try:
            transport.socket.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
