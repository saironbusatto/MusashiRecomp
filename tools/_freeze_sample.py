"""Sample freeze_check repeatedly to see what's actually running."""
import socket, json, time
PORT = 4370

s = socket.create_connection(('127.0.0.1', PORT), timeout=10)

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

for i in range(8):
    r = send({'id':1,'cmd':'freeze_check','window':32})
    fr = send({'id':1,'cmd':'frame'}).get('frame','?')
    fns = send({'id':1,'cmd':'fn_stats'})
    print(f"frame={fr} cur_func={r['current_func']} last_store_pc={r['last_store_pc']} "
          f"recent_total={r['recent_total']} fn_entry_total={fns['entry_total']} "
          f"hist={r.get('hist',[])[:3]}")
    time.sleep(0.05)

s.close()
