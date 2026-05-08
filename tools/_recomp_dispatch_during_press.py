"""During CROSS press, capture recomp's dirty_block_log and check
whether the JAL-site function (around 0x80030500..0x80030600 or
0x80008A00..0x80008B00) is dispatched.

Also check mem[0x80079E2C] on recomp before the press — that's the gate
the JAL-into-coordinator depends on."""
import socket, json, time

s = socket.create_connection(('127.0.0.1', 4370), timeout=10)

def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1 << 20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

# Read the gate cell on recomp
print('-- pre-press recomp state --')
print('gate 0x80079E2C:', send({'id': 1, 'cmd': 'read_word', 'addr': '0x80079E2C'}))
print('gate 0x80079E1C:', send({'id': 2, 'cmd': 'read_word', 'addr': '0x80079E1C'}))
print('mem 0x8005AE2C :', send({'id': 3, 'cmd': 'read_word', 'addr': '0x8005AE2C'}))

# Reset dirty_block_log if a reset cmd exists, otherwise just record current seq
state_pre = send({'id': 4, 'cmd': 'dirty_block_log', 'count': 1})
print('seq pre :', state_pre.get('total_seq', state_pre.get('total')))

# Press
print('\n-- press --')
print('press   :', send({'id': 5, 'cmd': 'press', 'buttons': 0xBFFF, 'frames': 4}))
time.sleep(0.6)

# Pull dirty_block_log entries since pre
print('\n-- post-press dirty_block_log --')
state_post = send({'id': 6, 'cmd': 'dirty_block_log', 'count': 2000})
total = state_post.get('total_seq', state_post.get('total'))
entries = state_post.get('entries', [])
print(f"total seq now: {total}, captured: {len(entries)}")

# Find entries that fall in the ranges we care about
WANT_RANGES = [
    (0x80030500, 0x80030600),  # caller copy 1 (kernel image)
    (0x80008A00, 0x80008B00),  # caller copy 2 (low RAM)
    (0x800394B0, 0x800398C4),  # the coordinator function body
    (0x80039000, 0x8003A000),  # broader range covering coord + helpers
]
print('\nentries hitting target/caller ranges:')
hit_any = False
for e in entries:
    tgt = int(e.get('target', '0x0'), 16)
    ra  = int(e.get('ra', '0x0'), 16)
    for lo, hi in WANT_RANGES:
        if lo <= tgt < hi or lo <= ra < hi:
            hit_any = True
            print(f"  seq={e.get('seq'):>5}  target=0x{tgt:08X}  ra=0x{ra:08X}  frame={e.get('frame')}")
            break
if not hit_any:
    print('  NONE')

# Also: count of unique targets in the press window
import collections
counter = collections.Counter()
for e in entries:
    counter[int(e.get('target','0x0'),16)] += 1
print('\ntop 30 dirty-RAM targets dispatched in window:')
for tgt, n in counter.most_common(30):
    print(f'  0x{tgt:08X}  hits={n}')
s.close()
