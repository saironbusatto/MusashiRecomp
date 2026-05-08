"""Press CROSS, then dump dirty-RAM block-entry log to find:
  1. Did 0x800321BC EVER get dispatched?
  2. What other RAM stubs got dispatched, with what RA?"""
import socket, json, time
s = socket.create_connection(('127.0.0.1', 4370), timeout=20)
def send(p):
    s.sendall((json.dumps(p)+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue
def to_int(v):
    if isinstance(v,str): return int(v,16) if v.startswith('0x') else int(v)
    return v

# Press CROSS for 4 frames
print("=== pressing CROSS for 4 frames ===")
print(send({'id':1,'cmd':'press','buttons':0xBFFF,'frames':4}))
time.sleep(0.5)

# Dump entire log (most recent 16K entries)
print("\n=== querying dirty_block_log (no filter, last 4096) ===")
r = send({'id':1,'cmd':'dirty_block_log','count':4096})
print(f"  total seq={r.get('total')} available={r.get('available')} emitted={r.get('emitted')}")

# Group by target — how many distinct stub PCs were entered?
groups = {}
for e in r.get('entries', []):
    t = to_int(e['target'])
    if t not in groups: groups[t] = []
    groups[t].append(e)
print(f"\n  unique targets: {len(groups)}")
for t in sorted(groups, key=lambda x: -len(groups[x])):
    es = groups[t]
    ras = set(to_int(e['ra']) for e in es)
    fr_lo = min(int(e['frame']) for e in es)
    fr_hi = max(int(e['frame']) for e in es)
    print(f"    0x{t:08X}: {len(es):>5} hits, "
          f"frames {fr_lo}..{fr_hi}, "
          f"unique RAs={len(ras)}: {sorted(f'0x{r:08X}' for r in list(ras)[:6])}")

# Specifically check 0x800321BC
print("\n=== filter target=0x800321BC (the modal stub) ===")
r2 = send({'id':1,'cmd':'dirty_block_log',
           'target_lo':'0x800321BC','target_hi':'0x800321C0','count':50})
print(f"  emitted={r2.get('emitted')}")
for e in r2.get('entries', []):
    print(f"    seq={e['seq']} target=0x{to_int(e['target']):08X} "
          f"ra=0x{to_int(e['ra']):08X} frame={e['frame']}")

# Also check the cursor stubs 0x8003215C..0x800321C0
print("\n=== filter target=0x8003215C..0x800321C0 (cursor + modal stubs) ===")
r3 = send({'id':1,'cmd':'dirty_block_log',
           'target_lo':'0x8003215C','target_hi':'0x800321E0','count':100})
print(f"  emitted={r3.get('emitted')}")
seen_targets = {}
for e in r3.get('entries', []):
    t = to_int(e['target'])
    seen_targets[t] = seen_targets.get(t, 0) + 1
for t in sorted(seen_targets):
    print(f"    0x{t:08X}: {seen_targets[t]} hits")

s.close()
