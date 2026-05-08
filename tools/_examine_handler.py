"""Dump bytes around 0xBFC19538 (last_store) + 0x800321BC (primary_handler)
+ key fn_entry hits in 0xBFC19500..0xBFC19A60 to see if commit area runs."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
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

print("=== 0x800321BC primary handler RAM (32 bytes) ===")
rr = send({'id':1,'cmd':'read_ram','addr':'0x800321BC','len':0x40})
b = bytes.fromhex(rr.get('hex',''))
for i in range(0, len(b), 4):
    w = struct.unpack('<I', b[i:i+4])[0]
    print(f"  0x{0x800321BC+i:08X}: 0x{w:08X}")

print("\n=== 0xBFC19500..0xBFC195C0 (around last_store) ===")
rr = send({'id':1,'cmd':'read_ram','addr':'0xBFC19500','len':0xC0})
b = bytes.fromhex(rr.get('hex',''))
for i in range(0, len(b), 4):
    w = struct.unpack('<I', b[i:i+4])[0]
    marker = '  <-- last_store' if (0xBFC19500 + i) == 0xBFC19538 else ''
    print(f"  0xBFC{0x19500+i:05X}: 0x{w:08X}{marker}")

print("\n=== fn_entry hits 0x1FC19000..0x1FC1A100 last 1k ===")
fs = send({'id':1,'cmd':'fn_stats'})
total = fs.get('entry_total', 0)
print(f"  entry_total = {total}")
seq_lo = max(0, total - 1000)
r = send({'id':1,'cmd':'fn_entry_dump',
          'addr_lo':'0x1FC19000','addr_hi':'0x1FC1A100',
          'seq_lo':str(seq_lo),'count':500})
funcs = {}
for e in r.get('entries', []):
    f = e['func']
    funcs[f] = funcs.get(f, 0) + 1
print(f"  unique funcs hit (last 1k seq):")
for f in sorted(funcs.keys()):
    print(f"    {f}: {funcs[f]} hits")

print("\n=== specific fn_entry — FUN_bfc19a58 + neighbors ===")
for (lo, hi, name) in [
    (0x1FC19A58, 0x1FC19A5C, 'FUN_bfc19a58'),
    (0x1FC19500, 0x1FC19560, 'around 0xBFC19538'),
    (0x1FC195A0, 0x1FC19600, 'after 19560'),
    (0x1FC19A00, 0x1FC19A58, 'before 19a58'),
    (0x1FC19A5C, 0x1FC19B00, 'after 19a58'),
]:
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':f'0x{lo:08X}','addr_hi':f'0x{hi:08X}',
              'seq_lo':'0','count':10})
    es = r.get('entries', [])
    print(f"  {name} ({lo:08X}..{hi:08X}): {len(es)} hits in last cleared window")
    for e in es[:3]:
        print(f"    seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} a0={e['a0']}")

s.close()
