"""Tiny TCP-debug-server client used by the OPTIONS-black session.

Single source of truth for talking to:
  - psx-runtime (TombaRecomp build) on port 4470
  - psx-beetle                       on port 4380

No game-specific knowledge here. Just request/response and command-name
adapters so the rest of the session scripts read the same way against
either backend, even though the wire verbs differ
(e.g. wtrace_add vs wtrace_arm).
"""
from __future__ import annotations
import json, socket, sys, time

RUNTIME_PORT = 4470
BEETLE_PORT  = 4380


def call(port: int, cmd: str, timeout: float = 30.0, **kw):
    payload = {"id": 1, "cmd": cmd, **kw}
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall((json.dumps(payload) + "\n").encode())
        buf = bytearray()
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf.extend(chunk)
    finally:
        s.close()
    return json.loads(buf.decode("utf-8", "replace").strip() or "{}")


def hx(v):
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        return int(v, 16) if v.lower().startswith("0x") else int(v)
    return v


def read_word(port: int, addr: int) -> int:
    """Little-endian word read at addr via read_ram."""
    r = call(port, "read_ram", addr=f"0x{addr:08X}", len=4)
    if not r.get("ok"):
        raise RuntimeError(f"read_ram failed at 0x{addr:08X}: {r}")
    b = bytes.fromhex(r["hex"])
    return int.from_bytes(b, "little")


def read_block(port: int, addr: int, n: int) -> bytes:
    out = bytearray()
    off = 0
    while off < n:
        chunk = min(0x40000, n - off)
        r = call(port, "read_ram", addr=f"0x{addr+off:08X}", len=chunk)
        if not r.get("ok"):
            raise RuntimeError(f"read_ram failed at 0x{addr+off:08X}: {r}")
        out.extend(bytes.fromhex(r["hex"]))
        off += chunk
    return bytes(out)


def ping_both():
    out = {}
    for name, port in [("runtime", RUNTIME_PORT), ("beetle", BEETLE_PORT)]:
        try:
            out[name] = call(port, "ping", timeout=3)
        except Exception as e:
            out[name] = {"error": str(e)}
    return out


if __name__ == "__main__":
    if len(sys.argv) == 1 or sys.argv[1] == "ping":
        for k, v in ping_both().items():
            print(f"{k:8} {v}")
        sys.exit(0)
    cmd = sys.argv[1]
    port = int(sys.argv[2])
    args = {}
    for a in sys.argv[3:]:
        k, _, v = a.partition("=")
        args[k] = v
    print(json.dumps(call(port, cmd, **args), indent=2))
