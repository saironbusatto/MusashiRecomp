import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
s.sendall((json.dumps({'id':1,'cmd':'card_txn_dump','count':500}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    try: json.loads(buf.decode()); break
    except: continue
r = json.loads(buf.decode())
ents = r.get('entries', [])
for slot in (0, 1):
    s_ents = [e for e in ents if e['slot'] == slot]
    s_succ = [e for e in s_ents if e['end_reason'] == 'success']
    s_read = [e for e in s_ents if e['cmd'] == '0x52' and e['end_reason'] == 'success']
    sectors = sorted(set(int(e['sector'], 16) for e in s_read))
    print(f"slot {slot}: total={len(s_ents)} success={len(s_succ)} read_success={len(s_read)} sectors={sectors}")
