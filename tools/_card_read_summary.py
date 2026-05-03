import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=30)
s.sendall((json.dumps({'id':1,'cmd':'card_read_summary'}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
r = json.loads(buf.decode())
print(f"count={r['count']}/{r['cap']}")
for e in r['entries']:
    print(f"  seq={e['seq']:>3} slot={e['slot']} cmd={e['cmd']} sector={e['sector']:>4} chk={e['checksum']} idx={e['data_idx']:>3} dest={e['dest_ram']} peek={e['data_peek'][:32]}")
