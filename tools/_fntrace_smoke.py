"""Smoke test the recomp-side fntrace ring.

Connects to psx-runtime on port 4370, dumps the most recent N entries,
and prints a summary. Use this to verify the ring is recording every
psx_dispatch entry.
"""
import socket
import json
import sys


def call(s: socket.socket, cmd: str, **kwargs):
    msg = {"cmd": cmd, "id": 1}
    msg.update(kwargs)
    s.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    return json.loads(buf.split(b"\n")[0].decode())


def main(port: int = 4370):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    r = call(s, "fntrace_dump", count=5)
    print(f"total dispatches recorded: {r.get('total')}")
    print(f"available in ring:         {r.get('available')}")
    print(f"emitted (this dump):       {r.get('emitted')}")
    print(f"armed targets:             {r.get('armed')}")
    print()
    print("most-recent 5 entries (newest first):")
    for e in r.get("entries", []):
        print(
            f"  seq={e['seq']:>10} target={e['target']} ra={e['ra']} "
            f"a0={e['a0']} a1={e['a1']} a2={e['a2']} a3={e['a3']} "
            f"s3={e['s3']} frame={e['frame']}"
        )
    s.close()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4370
    main(port)
