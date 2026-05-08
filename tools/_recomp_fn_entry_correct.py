"""Correctly query recomp fn_entry filtered to ROM shell range,
because recomp's normalize() maps RAM 0x30000-0x5AFFF to ROM 0x1FC18000+
before recording fn_entry."""
import socket, json, time, collections

s = socket.create_connection(('127.0.0.1', 4370), timeout=10)

def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1 << 20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

# ROM shell area (where recomp records normalized PCs for shell code)
print('-- fn_entry setup --')
print('fn_filter (ROM shell):', send({'id': 1, 'cmd': 'fn_filter',
                                      'lo': '0x1FC18000', 'hi': '0x1FC43000'}))
print('fn_clear:', send({'id': 2, 'cmd': 'fn_clear'}))

print('\n-- press --')
print('press   :', send({'id': 3, 'cmd': 'press', 'buttons': 0xBFFF, 'frames': 4}))
time.sleep(0.6)

# Dump
fn = send({'id': 4, 'cmd': 'fn_entry_dump', 'max_count': 2000})
total = fn.get('total')
entries = fn.get('entries', [])
print(f"\ntotal: {total}, captured: {len(entries)}")

# Print top addresses
counter = collections.Counter()
for e in entries:
    counter[int(e.get('func','0x0'),16)] += 1
print('\ntop 30 addresses:')
for addr, n in counter.most_common(30):
    # Translate phys back to RAM addr for readability
    ram = (addr - 0x1FC18000 + 0x80030000) if 0x1FC18000 <= addr < 0x1FC43000 else addr
    print(f"  0x{addr:08X} (RAM 0x{ram:08X})  hits={n}")

# Look specifically for the coordinator (phys 0x1FC214B0 = RAM 0x800394B0)
COORD_PHYS = 0x1FC214B0
CALLER_PHYS_1 = 0x1FC18558  # RAM 0x80030558 + offset
print(f'\n--- entries hitting coordinator 0x{COORD_PHYS:08X} (RAM 0x800394B0) ---')
hits = [e for e in entries if int(e.get('func','0x0'),16) == COORD_PHYS]
print(f'  {len(hits)} hits')
for e in hits[:5]:
    print(f"    seq={e.get('seq')}  ra=0x{int(e.get('ra','0x0'),16):08X}  a0=0x{int(e.get('a0','0x0'),16):08X}")

# Also check: is the FUNCTION CONTAINING the JAL site at 0x80030558 dispatched?
# That function's entry must be earlier — let's accept any fn_entry in 0x1FC18400..0x1FC18600
print(f'\n--- entries in 0x1FC18400..0x1FC18600 (likely JAL caller function) ---')
hits2 = [e for e in entries
         if 0x1FC18400 <= int(e.get('func','0x0'),16) < 0x1FC18600]
print(f'  {len(hits2)} hits')
for e in hits2[:10]:
    a = int(e.get('func','0x0'),16)
    print(f"    seq={e.get('seq')}  addr=0x{a:08X}  ra=0x{int(e.get('ra','0x0'),16):08X}")

# And the coordinator's function neighborhood (entries near 0x1FC214B0)
print(f'\n--- entries in 0x1FC21000..0x1FC22000 (coordinator neighborhood) ---')
hits3 = [e for e in entries
         if 0x1FC21000 <= int(e.get('func','0x0'),16) < 0x1FC22000]
print(f'  {len(hits3)} hits')
for e in hits3[:10]:
    print(f"    seq={e.get('seq')}  addr=0x{int(e.get('func','0x0'),16):08X}  ra=0x{int(e.get('ra','0x0'),16):08X}")
s.close()
