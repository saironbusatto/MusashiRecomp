"""Query Beetle's always-on fn_trace ring."""
import socket, json, sys

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(20.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        buf += c
        depth = 0; in_str = False; esc = False
        for b in buf:
            if esc: esc = False; continue
            if b == 0x5C: esc = True; continue
            if b == 0x22: in_str = not in_str; continue
            if in_str: continue
            if b == 0x7B: depth += 1
            elif b == 0x7D: depth -= 1
        if depth == 0 and buf.strip(): break
    s.close()
    return json.loads(buf.decode())

def cmd_arms():
    r = call({'id': 1, 'cmd': 'beetle_fntrace_arms'})
    print(f"armed targets ({r.get('count',0)}):")
    for a in r.get('arms', []):
        print(f"  {a}")

def cmd_arm(target):
    r = call({'id': 1, 'cmd': 'beetle_fntrace_arm', 'target': target})
    print(json.dumps(r, indent=2))

def cmd_disarm():
    r = call({'id': 1, 'cmd': 'beetle_fntrace_disarm'})
    print(json.dumps(r, indent=2))

def cmd_unfiltered(on):
    r = call({'id': 1, 'cmd': 'beetle_fntrace_unfiltered', 'on': int(on)})
    print(json.dumps(r, indent=2))

def cmd_reset():
    r = call({'id': 1, 'cmd': 'beetle_fntrace_reset'})
    print(json.dumps(r, indent=2))

def cmd_dump(count=64):
    r = call({'id': 1, 'cmd': 'beetle_fntrace_dump', 'count': str(count)})
    total = r.get('total', 0)
    entries = r.get('entries', [])
    print(f"total={total} returned={len(entries)}")
    for e in entries:
        print(f"  seq={e['seq']:>10} fr={e['frame']:>6} {e['kind']:<4} "
              f"caller={e['caller']} -> target={e['target']}  "
              f"ra={e['ra']} a0={e['a0']} a1={e['a1']}")

def cmd_callers(target, count=64):
    r = call({'id': 1, 'cmd': 'beetle_fntrace_callers', 'target': target, 'count': str(count)})
    entries = r.get('entries', [])
    print(f"callers of {target} ({len(entries)} hits):")
    for e in entries:
        print(f"  seq={e['seq']:>10} fr={e['frame']:>6} {e['kind']:<4} "
              f"caller={e['caller']}  ra={e['ra']} a0={e['a0']} a1={e['a1']}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('usage:')
        print('  beetle_fn_trace.py arms')
        print('  beetle_fn_trace.py arm <0xPC>')
        print('  beetle_fn_trace.py disarm')
        print('  beetle_fn_trace.py unfiltered 0|1')
        print('  beetle_fn_trace.py reset')
        print('  beetle_fn_trace.py dump [count]')
        print('  beetle_fn_trace.py callers <0xPC> [count]')
        sys.exit(1)
    cmd = sys.argv[1]
    if   cmd == 'arms':       cmd_arms()
    elif cmd == 'arm':        cmd_arm(sys.argv[2])
    elif cmd == 'disarm':     cmd_disarm()
    elif cmd == 'unfiltered': cmd_unfiltered(int(sys.argv[2]))
    elif cmd == 'reset':      cmd_reset()
    elif cmd == 'dump':       cmd_dump(int(sys.argv[2]) if len(sys.argv) > 2 else 64)
    elif cmd == 'callers':    cmd_callers(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 64)
    else: print(f'unknown command: {cmd}')
