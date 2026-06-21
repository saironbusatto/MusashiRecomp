#!/usr/bin/env python3
"""Phantom-input A/B instrument driver: get/set the pad config-SM mode live.

Usage:
  python padcfg.py            # report current mode
  python padcfg.py 1          # legacy pre-98aa688 "always 0xF3" config
  python padcfg.py 0          # new config state machine (shipping)
Optional: PORT as 2nd arg (default 4470, Tomba dev build).
"""
import socket, json, sys

port = 4470
setv = None
if len(sys.argv) > 1 and sys.argv[1] != "":
    setv = int(sys.argv[1])
if len(sys.argv) > 2:
    port = int(sys.argv[2])

cmd = {"cmd": "pad_cfg"}
if setv is not None:
    cmd["set"] = setv

try:
    s = socket.create_connection(("127.0.0.1", port), 3)
    s.sendall((json.dumps(cmd) + "\n").encode())
    print(s.recv(65536).decode().strip())
except Exception as e:
    print("debug server not up yet (expected until in-game):", e)
