"""Check if FUN_bfc1a1bc (twin of 0x800321BC modal stub) is being called."""
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

# Clear, press, capture
send({'id':1,'cmd':'fn_clear'})
print(send({'id':1,'cmd':'press','buttons':0xBFFF,'frames':4}))
time.sleep(0.5)

fs = send({'id':1,'cmd':'fn_stats'})
total = fs.get('entry_total', 0)
print(f"\nfn_entry total since clear: {total}")

# Wide range covering BFC1A1BC + neighbors
for (lo, hi, name) in [
    (0x1FC1A1BC, 0x1FC1A1C0, 'FUN_bfc1a1bc (modal twin) STATIC'),
    (0x1FC1A150, 0x1FC1A1C0, 'BFC1A150..BFC1A1C0 (above modal twin)'),
    (0x1FC1A1C0, 0x1FC1A3B4, 'BFC1A1C0..BFC1A3B4 (modal twin body)'),
    (0x1FC1A3B4, 0x1FC1A500, 'BFC1A3B4..BFC1A500 (after modal twin)'),
    (0x1FC1A000, 0x1FC1A1BC, 'BFC1A000..BFC1A1BC (just below)'),
    (0x1FC19F00, 0x1FC1A000, 'BFC19F00..BFC1A000 (below)'),
]:
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':f'0x{lo:08X}','addr_hi':f'0x{hi:08X}',
              'seq_lo':'0','count':30})
    es = r.get('entries', [])
    print(f"\n  -- {name}: {len(es)} hits --")
    for e in es[:6]:
        print(f"    seq={e['seq']} fr={e['frame']} func={e['func']} "
              f"ra={e['ra']} a0={e['a0']} a1={e.get('a1')}")

s.close()
