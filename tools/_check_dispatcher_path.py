"""Trace the dispatcher path: does v0=4 lead to BFC1AA90?"""
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

send({'id':1,'cmd':'fn_clear'})
print(send({'id':1,'cmd':'press','buttons':0xBFFF,'frames':4}))
time.sleep(0.5)

# Check fn_entry hits for:
# - BFC1F600 (the dispatcher itself; goes through RAM-relocated 0x80037600)
# - BFC1A420 (cursor getter — confirmed running 30+ times)
# - BFC1AA90 (modal handler — should run if cursor==4 + main==0x32/0x33)
# - BFC1A1BC (modal stub — should run from BFC1AA90 chain)
for (lo, hi, name) in [
    (0x1FC1F600, 0x1FC1F6A8, 'FUN_bfc1f600 dispatcher'),
    (0x1FC1A420, 0x1FC1A430, 'BFC1A420 cursor getter'),
    (0x1FC1AA90, 0x1FC1AB00, 'FUN_bfc1aa90 (modal commit?)'),
    (0x1FC1AA90, 0x1FC1AC68, 'BFC1AA90..AC68 (full modal handler region)'),
    (0x1FC1A1BC, 0x1FC1A3B4, 'BFC1A1BC..A3B4 (modal stub body)'),
    (0x1FC1AC68, 0x1FC1B000, 'BFC1AC68..B000 (after modal region)'),
    (0x1FC1A6EC, 0x1FC1A800, 'BFC1A6EC start'),
    (0x1FC1AE68, 0x1FC1AF00, 'BFC1AE68 start'),
]:
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':f'0x{lo:08X}','addr_hi':f'0x{hi:08X}',
              'seq_lo':'0','count':30})
    es = r.get('entries', [])
    print(f"\n  -- {name}: {len(es)} hits --")
    for e in es[:5]:
        print(f"    seq={e['seq']} fr={e['frame']} func={e['func']} "
              f"ra={e['ra']} a0={e['a0']}")

# Also read mem[0x80079DF8] — this gates the JAL 0x80037CC8 above the main jal
import struct
rr = send({'id':1,'cmd':'read_ram','addr':'0x80079DF8','len':16})
b = bytes.fromhex(rr.get('hex',''))
print(f"\nmem[0x80079DF8]:")
for i in range(0, len(b), 4):
    if i+4>len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    print(f"  0x{0x80079DF8+i:08X}: 0x{w:08X}")

# main_state
rr = send({'id':1,'cmd':'read_ram','addr':'0x80066948','len':4})
b = bytes.fromhex(rr.get('hex',''))
main = struct.unpack('<I', b)[0]
print(f"\nmain_state (0x80066948) = 0x{main:08X}")
# cursor
rr = send({'id':1,'cmd':'read_ram','addr':'0x80066BB8','len':4})
b = bytes.fromhex(rr.get('hex',''))
print(f"cursor    (0x80066BB8) = 0x{struct.unpack('<I', b)[0]:08X}")
# 0x80079F64 (cursor getter return value source)
rr = send({'id':1,'cmd':'read_ram','addr':'0x80079F64','len':4})
b = bytes.fromhex(rr.get('hex',''))
print(f"cursor-mirror (0x80079F64) = 0x{struct.unpack('<I', b)[0]:08X}")

s.close()
