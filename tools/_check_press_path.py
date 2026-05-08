"""Sanity check: is the press actually being delivered? What RAM PCs run?"""
import socket, json, time, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=20)
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
def to_int(v):
    if isinstance(v, str): return int(v, 16) if v.startswith('0x') else int(v)
    return v

print("=== pad_status ===")
print(json.dumps(send({'id':1,'cmd':'pad_status'}), indent=2))

print("\n=== sio_state pad_buttons ===")
ss = send({'id':1,'cmd':'sio_state'})
print(f"  pad_buttons={ss.get('pad_buttons')}")

print("\n=== set_input then re-check pad_status ===")
print(send({'id':1,'cmd':'set_input','buttons':'0xBFFF'}))
time.sleep(0.05)
print(json.dumps(send({'id':1,'cmd':'pad_status'}), indent=2))
ss = send({'id':1,'cmd':'sio_state'})
print(f"  pad_buttons after set={ss.get('pad_buttons')}")

print("\n=== full dirty_ram per_pc ===")
d = send({'id':1,'cmd':'dirty_ram_stats'})
per_pc = d.get('per_pc', [])
print(f"  blocks_run={d.get('blocks_run')} insns_run={d.get('insns_run')} "
      f"aborts={d.get('aborts')}")
for e in sorted(per_pc, key=lambda x: -int(x['hits'])):
    print(f"  pc=0x{to_int(e['pc']):08X} hits={e['hits']} insns={e['insns']}")

# Clear input back
send({'id':1,'cmd':'clear_input'})
print(send({'id':1,'cmd':'pad_status'}))
s.close()
