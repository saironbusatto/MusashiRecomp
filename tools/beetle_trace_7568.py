"""Trace [0x80007568] across Beetle frames. Smaller batches to avoid TCP timeouts."""
import socket, json, sys, time

HOST, PORT = '127.0.0.1', 4370


def request(cmd, **kw):
    s = socket.create_connection((HOST, PORT))
    s.settimeout(60.0)
    msg = {'id': 1, 'cmd': cmd}
    msg.update(kw)
    s.sendall(json.dumps(msg).encode() + b'\n')
    buf = b''
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        depth = 0; in_str = False; esc = False
        for c in buf:
            if esc: esc = False; continue
            if c == 0x5C: esc = True; continue
            if c == 0x22: in_str = not in_str; continue
            if in_str: continue
            if c == 0x7B: depth += 1
            elif c == 0x7D: depth -= 1
        if depth == 0 and buf.strip():
            break
    s.close()
    text = buf.decode(errors='replace').strip()
    if '\n' in text:
        # take last complete object
        parts = [p for p in text.split('\n') if p.strip()]
        text = parts[-1]
    return json.loads(text)


def read_word(addr):
    r = request('emu_read_ram', addr=f"0x{addr:08X}", len=4)
    return r.get('hex', '')


def step(n, pad1):
    return request('emu_step', frames=n, pad1=pad1)


def main():
    print("=== INITIAL ===")
    print(f"  [0x7568..0x756B] = {read_word(0x80007568)}")

    # Watch via polled reads (manual sample): step in batches of 30 frames,
    # check value, repeat. Print whenever it changes.
    last = read_word(0x80007568)
    print(f"  start: 0x{0x80007568:08X} = {last}")

    frame_total = 0

    # Phase 1: idle boot, 12 batches of 30 frames = 360 frames (~6s).
    for batch in range(12):
        r = step(30, 0xFFFF)
        frame_total += 30
        v = read_word(0x80007568)
        if v != last:
            print(f"  frame≈{r.get('oracle_frame'):>4}: 0x7568..0x756B {last} -> {v}")
            last = v

    # Phase 2: press CROSS, 1 frame batches for 6 frames.
    print("--- press CROSS ---")
    for f in range(6):
        r = step(1, 0xBFFF)
        v = read_word(0x80007568)
        if v != last:
            print(f"  CROSS  frame={r.get('oracle_frame'):>4}: {last} -> {v}")
            last = v

    # Phase 3: release, 30-frame batches for 12 batches (~6s).
    print("--- release ---")
    for batch in range(12):
        r = step(30, 0xFFFF)
        v = read_word(0x80007568)
        if v != last:
            print(f"  release frame={r.get('oracle_frame'):>4}: {last} -> {v}")
            last = v

    print()
    print("=== FINAL STATE ===")
    for label, addr, length in [
        ("[0x7560..0x756F] (accum+status)", 0x80007560, 16),
        ("[0x7550..0x755F]               ", 0x80007550, 16),
        ("[0x80007AF0..0x80007AF8] [72F0?]", 0x800072F0, 8),
    ]:
        r = request('emu_read_ram', addr=f"0x{addr:08X}", len=length)
        print(f"  {label}: {r.get('hex')}")


if __name__ == '__main__':
    main()
