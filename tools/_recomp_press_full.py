"""Full recomp-side capture during CROSS press at YES/NO modal.

Captures:
  - wtrace over a broad range covering the modal/shell state area
  - fn_entry ring filtered to 0x00030000..0x00050000 (kernel RAM image)

Then prints both rings, summarized by frequency.
"""
import socket, json, time, collections

s = socket.create_connection(('127.0.0.1', 4370), timeout=10)

def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1 << 20)
        if not c:
            break
        buf += c
        if buf.strip().endswith(b'}'):
            try:
                return json.loads(buf.decode())
            except Exception:
                continue

# --- recomp wtrace ---
print('-- recomp wtrace setup --')
print('clear     :', send({'id': 1, 'cmd': 'wtrace_clear'}))
print('reset rng :', send({'id': 2, 'cmd': 'wtrace_range',
                            'lo': '0x00000000', 'hi': '0x00000000'}))
ranges = [
    (0x00066900, 0x00066D00),  # state_block + cursor
    (0x00078300, 0x00078400),  # state_idx + widget area
    (0x00079F00, 0x00079FF0),  # cursor mirror
    (0x0007A180, 0x0007A184),  # modal gate
]
for i, (lo, hi) in enumerate(ranges):
    print(f'add [{lo:08X}..{hi:08X}]:',
          send({'id': 10+i, 'cmd': 'wtrace_add',
                'lo': f'0x{lo:08X}', 'hi': f'0x{hi:08X}'}))

# --- recomp fn_entry filter (kernel RAM image area) ---
print('\n-- recomp fn_entry filter --')
print('fn_filter :', send({'id': 20, 'cmd': 'fn_filter',
                            'lo': '0x00030000', 'hi': '0x00050000'}))
print('fn_clear  :', send({'id': 21, 'cmd': 'fn_clear'}))

print('\n-- press --')
print('press     :', send({'id': 30, 'cmd': 'press', 'buttons': 0xBFFF, 'frames': 4}))
time.sleep(0.6)

print('\n=== RECOMP wtrace ===')
r = send({'id': 40, 'cmd': 'wtrace_dump', 'count': 200})
entries = r.get('entries', [])
print(f"total: {r.get('total')}, captured: {len(entries)}")
for e in entries[:80]:
    print(f"  seq={e.get('seq'):>5}  addr=0x{int(e.get('addr','0x0'),16):08X}"
          f"  old=0x{int(e.get('old','0x0'),16):08X}"
          f"  new=0x{int(e.get('new','0x0'),16):08X}"
          f"  pc=0x{int(e.get('pc','0x0'),16):08X}"
          f"  ra=0x{int(e.get('ra','0x0'),16):08X}  sz={e.get('size')}")

print('\n=== RECOMP fn_entry (kernel RAM 0x00030000..0x00050000) ===')
fn = send({'id': 41, 'cmd': 'fn_entry_dump', 'max_count': 1000})
fn_entries = fn.get('entries', [])
print(f"total: {fn.get('total')}, captured: {len(fn_entries)}")

# count by addr
counts = collections.Counter()
for e in fn_entries:
    counts[e.get('addr', '?')] += 1
print('\ntop 20 addresses by hit count:')
for addr, n in counts.most_common(20):
    print(f"  {addr}  hits={n}")

# print a tail (last 40 entries to see chain near modal)
print('\nfn_entry tail (last 40 entries):')
for e in fn_entries[-40:]:
    print(f"  seq={e.get('seq'):>6}  addr={e.get('addr')}  ra={e.get('ra','?')}")
s.close()
