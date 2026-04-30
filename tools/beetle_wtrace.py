"""beetle_wtrace.py — query Beetle's RAM-write trace ring.

Beetle's wtrace is registered at boot and default-armed on the card-gate
window [0x7568..0x756C). Add more ranges via `arm`. Query the ring with
`dump`. Reset with `reset`.

Usage:
    python tools/beetle_wtrace.py dump [count=256]
    python tools/beetle_wtrace.py arm <lo_hex> <hi_hex>
    python tools/beetle_wtrace.py disarm
    python tools/beetle_wtrace.py ranges
    python tools/beetle_wtrace.py reset
    python tools/beetle_wtrace.py find_lsb_clear   # show only writes that clear bit 0

The default port is the unified debug server (4370), which is the same
port psx-beetleoracle.exe listens on.
"""
import socket, json, sys


def call(cmd, **kwargs):
    payload = {'id': 1, 'cmd': cmd}
    payload.update(kwargs)
    s = socket.create_connection(('127.0.0.1', 4370))
    s.settimeout(10.0)
    s.sendall(json.dumps(payload).encode() + b'\n')
    buf = b''
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        # crude balanced-brace finish detect
        depth = 0; in_str = False; esc = False
        for c in buf:
            if esc:
                esc = False; continue
            if c == 0x5C:
                esc = True; continue
            if c == 0x22:
                in_str = not in_str; continue
            if in_str:
                continue
            if c == 0x7B:
                depth += 1
            elif c == 0x7D:
                depth -= 1
        if depth == 0 and buf.strip():
            break
    s.close()
    return json.loads(buf.decode())


def fmt_entry(e, prev_val_by_addr):
    addr = e['addr']
    new_val = e['val']
    new_int = int(new_val, 16) & 0xFF  # gate is byte
    old = prev_val_by_addr.get(addr)
    old_str = f"0x{old:02X}" if old is not None else " ?? "
    prev_val_by_addr[addr] = new_int
    delta = "OPEN" if (new_int & 1) == 0 else "BUSY"
    return (f"#{e['seq']:>5} f={e['frame']:>5} {addr} {old_str}->0x{new_int:02X} ({delta})  "
            f"pc={e['pc']} ({e['region']:>4}) ra={e['ra']} slot={e['slot']} sz={e['size']}")


def cmd_dump(args):
    count = int(args[0]) if args else 256
    r = call('beetle_wtrace', count=count)
    if not r.get('ok'):
        print(f"ERROR: {r.get('error')}")
        return
    print(f"# total={r['total']} returned={r['count']}")
    prev = {}
    for e in r.get('entries', []):
        print(fmt_entry(e, prev))


def cmd_find_lsb_clear(args):
    count = int(args[0]) if args else 65536
    r = call('beetle_wtrace', count=count)
    if not r.get('ok'):
        print(f"ERROR: {r.get('error')}")
        return
    print(f"# total={r['total']} scanned={r['count']}  (filter: writes with new_value LSB=0)")
    prev = {}
    hits = 0
    for e in r.get('entries', []):
        new_int = int(e['val'], 16) & 0xFF
        old = prev.get(e['addr'])
        prev[e['addr']] = new_int
        if (new_int & 1) == 0:
            hits += 1
            print(fmt_entry(e, {e['addr']: old} if old is not None else {}))
    print(f"# {hits} LSB-clear writes")


def cmd_arm(args):
    if len(args) < 2:
        print("usage: arm <lo_hex> <hi_hex>")
        return
    r = call('beetle_wtrace_arm', lo=args[0], hi=args[1])
    print(json.dumps(r, indent=2))


def cmd_disarm(args):
    print(json.dumps(call('beetle_wtrace_disarm'), indent=2))


def cmd_ranges(args):
    print(json.dumps(call('beetle_wtrace_ranges'), indent=2))


def cmd_reset(args):
    print(json.dumps(call('beetle_wtrace_reset'), indent=2))


CMDS = {
    'dump':            cmd_dump,
    'find_lsb_clear':  cmd_find_lsb_clear,
    'arm':             cmd_arm,
    'disarm':          cmd_disarm,
    'ranges':          cmd_ranges,
    'reset':           cmd_reset,
}


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS:
        print(__doc__)
        sys.exit(1)
    CMDS[sys.argv[1]](sys.argv[2:])
