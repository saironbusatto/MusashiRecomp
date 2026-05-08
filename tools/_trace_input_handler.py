"""Press CROSS, but specifically trace 0x800321BC + its callees."""
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

# 1. Check dirty_ram_stats baseline
print("=== dirty_ram_stats (BEFORE clear) ===")
d = send({'id':1,'cmd':'dirty_ram_stats'})
print(f"  blocks_run={d.get('blocks_run')} insns_run={d.get('insns_run')} "
      f"aborts={d.get('aborts')} bitmap={d.get('dirty_bitmap')}")
per_pc = d.get('per_pc', [])
print(f"  unique RAM PCs: {len(per_pc)}")
# Look for 0x800321BC specifically
for e in per_pc:
    pc = to_int(e['pc'])
    if 0x80032000 <= pc <= 0x80033000 or 0x80046000 <= pc <= 0x80047000 \
       or 0x8003E000 <= pc <= 0x8003F000:
        print(f"    pc=0x{pc:08X} hits={e['hits']} insns={e['insns']}")

# 2. Clear rings + add watches on relevant state
send({'id':1,'cmd':'fn_clear'})
send({'id':1,'cmd':'wtrace_clear'})
# Add watches on state-block, button table, cursor, modal gate, and the
# substate-output addresses used by the install-handler stub
# (per disasm, 0x800321BC body writes 'state' values)
for (lo, hi) in [
    (0x80066940, 0x80066960),  # state block
    (0x80066B68, 0x80066BC0),  # button table + cursor
    (0x80078320, 0x80078340),  # state_idx/widget
    (0x80079E20, 0x80079E40),  # gate region
    (0x8007A180, 0x8007A1A0),  # modal gate
]:
    send({'id':1,'cmd':'wtrace_add',
          'lo':f'0x{lo:08X}','hi':f'0x{hi:08X}'})

print("\n=== watches added; pressing CROSS for 4 frames ===")
press_word = 0xFFFF & ~0x4000  # CROSS
print(send({'id':1,'cmd':'press','buttons':press_word,'frames':4}))
time.sleep(0.3)

# 3. Targeted fn_entry filters
print("\n=== fn_entry hits (specific targets) ===")
fs = send({'id':1,'cmd':'fn_stats'})
total = fs.get('entry_total', 0)
print(f"  fn_entry_total since clear: {total}")
for (lo, hi, name) in [
    (0x800321BC, 0x800321C0, '0x800321BC primary handler'),
    (0x80032000, 0x80033000, '0x80032xxx region'),
    (0x80046000, 0x80047000, '0x80046xxx region'),
    (0x8003E000, 0x8003F000, '0x8003Exxx region'),
    (0x1FC1E4A0, 0x1FC1E4A4, '0xBFC1E4A0 (= 0x8003E4A0)'),
    (0x1FC265DC, 0x1FC265E0, '0xBFC265DC (= 0x800465DC)'),
    (0x1FC19A58, 0x1FC19A5C, 'FUN_bfc19a58'),
    (0x1FC19400, 0x1FC19500, '0x1FC194xx around 193C0'),
    (0x1FC18A00, 0x1FC18B00, '0x1FC18A00..1FC18B00'),
]:
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':f'0x{lo:08X}','addr_hi':f'0x{hi:08X}',
              'seq_lo':'0','count':20})
    es = r.get('entries', [])
    if es:
        print(f"\n  -- {name}: {len(es)} hits --")
        for e in es[:6]:
            print(f"    seq={e['seq']} fr={e['frame']} func={e['func']} "
                  f"ra={e['ra']} a0={e['a0']} a1={e.get('a1')}")
    else:
        print(f"  {name}: 0 hits")

# 4. wtrace dump — what changed during the press?
ws = send({'id':1,'cmd':'wtrace_stats'})
wtot = ws.get('total', 0)
print(f"\n=== wtrace total since clear: {wtot} ===")
r = send({'id':1,'cmd':'wtrace_dump','start':0,'count':min(wtot, 200)})
for e in r.get('entries', []):
    addr = to_int(e['addr'])
    print(f"  fr={e.get('frame','?')} addr=0x{addr:08X} "
          f"old=0x{to_int(e.get('old',0)):08X} -> new=0x{to_int(e.get('new',0)):08X} "
          f"by func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")

s.close()
