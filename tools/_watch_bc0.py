"""Add wtrace coverage for the bc0 byte and surrounding cursor block,
then sample writes to it."""
import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=15)
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

print("ranges:", send({'id':1,'cmd':'wtrace_ranges'}))
# Add specific watch on cursor block (bb8/bbc/bc0/bc4 = 0x80066BB8..0x80066BC8)
print("add cursor block:", send({'id':1,'cmd':'wtrace_add',
    'lo':'0x80066BB8','hi':'0x80066BC8'}))
# Add watch on a180 modal gate
print("add a180:", send({'id':1,'cmd':'wtrace_add',
    'lo':'0x8007A17C','hi':'0x8007A188'}))
# Add watch on state idx + widget id
print("add 0x80078320:", send({'id':1,'cmd':'wtrace_add',
    'lo':'0x80078320','hi':'0x80078340'}))
# Clear and re-check stats
print("clear:", send({'id':1,'cmd':'wtrace_clear'}))
print("ranges after:", send({'id':1,'cmd':'wtrace_ranges'}))
print("stats:", send({'id':1,'cmd':'wtrace_stats'}))
s.close()
