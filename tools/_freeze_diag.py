"""Diagnose the current freeze: history span, freeze_check, last-tail of every ring."""
import socket, json

s = socket.create_connection(('127.0.0.1', 4370), timeout=30)

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

print("=== history ===")
print(json.dumps(send({'id':1,'cmd':'history'}), indent=2))

print("\n=== freeze_check (window=256) ===")
print(json.dumps(send({'id':1,'cmd':'freeze_check','window':256}), indent=2))

print("\n=== frame ===")
print(json.dumps(send({'id':1,'cmd':'frame'}), indent=2))

print("\n=== sio_state ===")
print(json.dumps(send({'id':1,'cmd':'sio_state'}), indent=2))

print("\n=== irq_state ===")
print(json.dumps(send({'id':1,'cmd':'irq_state'}), indent=2))

print("\n=== mc_status ===")
print(json.dumps(send({'id':1,'cmd':'mc_status'}), indent=2))

print("\n=== pace_state ===")
print(json.dumps(send({'id':1,'cmd':'pace_state'}), indent=2))

print("\n=== fn_stats ===")
print(json.dumps(send({'id':1,'cmd':'fn_stats'}), indent=2))

print("\n=== wtrace_stats ===")
print(json.dumps(send({'id':1,'cmd':'wtrace_stats'}), indent=2))

print("\n=== dirty_ram_stats ===")
print(json.dumps(send({'id':1,'cmd':'dirty_ram_stats'}), indent=2))

s.close()
