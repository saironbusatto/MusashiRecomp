"""Capture full state after reproduction of CROSS-on-COPY-modal bug."""
import socket, json, struct

s = socket.create_connection(('127.0.0.1', 4370), timeout=30)

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
    return json.loads(buf.decode())

def read_words(addr, length, label=''):
    r = send({'id':1,'cmd':'read_ram','addr':f'0x{addr:08X}','len':length})
    hx = r.get('hex', '')
    b = bytes.fromhex(hx) if hx else b''
    print(f"\n--- {label or hex(addr)} (0x{addr:08X}, {length}B) ---")
    for i in range(0, len(b), 4):
        if i+4 > len(b): break
        w = struct.unpack("<I", b[i:i+4])[0]
        print(f"  0x{addr+i:08X}: 0x{w:08X}")
    return b

# 1. Core state block
read_words(0x80066940, 32, 'state block (substate, +0x6944, main, +0x694C, +0x6950, ...)')

# 2. Modal/cursor/widget bookkeeping per handoff
read_words(0x80066BB8, 16,  'modal selection / cursor counter')
read_words(0x80078320, 16,  'state index / widget id')
read_words(0x80079E40, 32,  'staging block (incl. mem[0x80079E4C])')

# 3. State-7 entry from the state table (0x80110EC8 + 7*0x688 = 0x80113D58)
ENTRY_BASE = 0x80110EC8
ENTRY_SIZE = 0x688
state_idx = 7
read_words(ENTRY_BASE + state_idx * ENTRY_SIZE, 0x40,
           f'state-{state_idx} entry head (0x80110EC8 + {state_idx}*0x688)')

# Also capture the "current state index" if the global lives somewhere
# we can read.  Try mem[0x80078320] (already dumped) — first word is state idx.

# 4. Recent wtrace on the state block
print("\n--- wtrace state-block writes (last 60 hits in 0x66940..0x6695C) ---")
w = send({'id':1,'cmd':'wtrace_dump','start':0,'count':100000})
all_entries = w.get('entries', [])
def to_int(v):
    if isinstance(v,str): return int(v,16) if v.startswith('0x') else int(v)
    return v
state_writes = [e for e in all_entries
                if 0x66940 <= to_int(e.get('addr',0)) < 0x6695C]
for e in state_writes[-60:]:
    print(f"  addr=0x{to_int(e['addr']):08X} old=0x{to_int(e.get('old',0)):08X} "
          f"new=0x{to_int(e.get('new',0)):08X} func=0x{to_int(e.get('func',0)):08X} "
          f"ra=0x{to_int(e.get('ra',0)):08X} frame={e.get('frame','?')}")
print(f"  (total state-block writes captured: {len(state_writes)})")

# 5. fn_entry dump for the armed range
print("\n--- fn_entry hits in 0x1FC1D000..0x1FC1F000 ---")
r = send({'id':1,'cmd':'fn_entry_dump',
         'addr_lo':'0x1FC1D000','addr_hi':'0x1FC1F000',
         'seq_lo':'0','count':200})
print(f"  total={r.get('total')} returned={len(r.get('entries',[]))}")
for e in r.get('entries', []):
    print(f"  seq={e['seq']:>4} func={e['func']} ra={e['ra']} "
          f"a0={e['a0']} a1={e.get('a1','?')} a2={e.get('a2','?')} "
          f"frame={e['frame']} depth={e.get('depth','?')}")

s.close()
