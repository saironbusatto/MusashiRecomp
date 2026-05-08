"""Find the caller of the loop functions by looking at the very latest fn_entry."""
import socket, json
PORT = 4370

s = socket.create_connection(('127.0.0.1', PORT), timeout=20)
def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue
    return json.loads(buf.decode())

stats = send({'id':1,'cmd':'fn_stats'})
total = stats['entry_total']
print(f"fn_entry total = {total}")
N = 60
seq_lo = max(0, total - N)
r = send({'id':1,'cmd':'fn_entry_dump',
          'addr_lo':'0x1FC00000','addr_hi':'0x1FC50000',
          'seq_lo':str(seq_lo),'count':N})

# Group by (func, ra) pair
from collections import Counter
pairs = Counter()
for e in r.get('entries', []):
    pairs[(e['func'], e['ra'])] += 1

print(f"\nCaller pairs over last {N} entries:")
for (fn, ra), c in pairs.most_common():
    print(f"  func={fn} ra={ra} count={c}")

print(f"\nLast 30 entries (chronological):")
for e in r.get('entries', [])[-30:]:
    print(f"  seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} "
          f"a0={e['a0']} a1={e.get('a1')}")

# Read mem[0x80079E24] — used by BFC2E65C as substate index
addr = 0x80079E24
rr = send({'id':1,'cmd':'read_ram','addr':f'0x{addr:08X}','len':4})
import struct
b = bytes.fromhex(rr.get('hex',''))
print(f"\nmem[0x{addr:08X}] = 0x{struct.unpack('<I',b)[0]:08X}")

# Read state-block + cursor block
for (a, ln, lbl) in [(0x80066940,32,'state'), (0x80066BB8,16,'cursor'),
                     (0x80078320,16,'state_idx/widget'),
                     (0x80079E24,16,'substate?')]:
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
    b = bytes.fromhex(rr.get('hex',''))
    print(f"\n{lbl} @ 0x{a:08X}:")
    for i in range(0, len(b), 4):
        if i+4 > len(b): break
        w = struct.unpack('<I', b[i:i+4])[0]
        print(f"  0x{a+i:08X} = 0x{w:08X}")

s.close()
