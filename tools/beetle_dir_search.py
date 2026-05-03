"""Search Beetle RAM for evidence of memcard directory frames (sectors 1..15).

Each PSX memcard frame = 128 bytes. Directory frames (sectors 1..15) start
with a 4-byte status/block-info header. If Beetle has loaded the directory
into RAM, we should see 15 frames somewhere — possibly at:
  - 0x8000C000-ish (continuing past the 0x8000BE48/0xBEC8 sector 0 buffers)
  - kernel directory table area
  - shell-allocated buffer
"""
import socket, json, sys

HOST, PORT = '127.0.0.1', 4370


def request(s, cmd, **kw):
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
    return json.loads(buf.decode().strip().split('\n')[-1] if '\n' in buf.decode() else buf.decode())


def hex_dump(label, addr, length, hexstr):
    print(f"{label} @ 0x{addr:08X} ({length}B): {hexstr.lower()}")


def main():
    s = socket.create_connection((HOST, PORT))
    s.settimeout(5.0)

    # Sector 0 buffers (already known-good).
    for label, addr, length in [
        ("slot0 sec0 hdr   ", 0x8000BE48, 16),
        ("slot0 next 128B  ", 0x8000BEC8, 16),
        ("slot1 sec0 hdr   ", 0x8000BEC8, 16),
        ("slot1 next 128B  ", 0x8000BF48, 16),
    ]:
        r = request(s, 'emu_read_ram', addr=f"0x{addr:08X}", len=length)
        hex_dump(label, addr, length, r.get('hex', '?'))

    print()
    # Walk forward from each slot's sector 0 buffer 16 frames (15 dir + 1 sec0).
    for slot, base in [(0, 0x8000BE48), (1, 0x8000BEC8)]:
        print(f"--- slot {slot} from 0x{base:08X}, 16 frames of 128B ---")
        for i in range(16):
            a = base + i * 128
            r = request(s, 'emu_read_ram', addr=f"0x{a:08X}", len=8)
            print(f"  frame {i:>2} @ 0x{a:08X}: {r.get('hex','?').lower()}")
        print()

    # Look for likely directory tables elsewhere.
    print("--- candidate dir tables ---")
    for label, addr, length in [
        ("after slot0     ", 0x8000C0C8, 64),
        ("0x8000B800      ", 0x8000B800, 64),
        ("0x80008000      ", 0x80008000, 64),
        ("0x8000A000      ", 0x8000A000, 64),
        ("0x80020000      ", 0x80020000, 64),
        ("0x800066B0 shell", 0x800066B0, 64),
        ("0x800072F0 [72F0]", 0x800072F0, 64),
        ("0x80007280      ", 0x80007280, 64),
    ]:
        r = request(s, 'emu_read_ram', addr=f"0x{addr:08X}", len=length)
        hex_dump(label, addr, length, r.get('hex', '?'))

    s.close()


if __name__ == '__main__':
    main()
