"""Read a list of RAM addresses from Beetle via emu_read_ram and print
side-by-side with the values we captured from recomp earlier.

Recomp values are baked in (as captured from the prior session)."""
import socket, json, sys

HOST, PORT = '127.0.0.1', 4370

# Recomp baseline (captured 2026-04-29 post-CROSS, settled).
RECOMP = {
    # addr -> (length_bytes, hex_string)
    0x800074A4: (16, "0000000000000000bc4900004c4a0000"),
    0x80007550: (32, "000000000000000000000000000000003f0000003f0000000101000001000000"),
    0x800075B0: (32, "0000000000000000000000000000000000000000000000006975000064750000"),
    0x80007AF0: (32, "0000000000000000000000000000000000000000000000000000000000000000"),
    0x8000BE48: (32, "4d43000000000000000000000000000000000000000000000000000000000000"),
    0x8000BEC8: (32, "4d43000000000000000000000000000000000000000000000000000000000000"),
}

# Extra Beetle-only ranges to dump for context.
EXTRA = [
    (0x80007AE0, 64),  # widen around [0x72F0]
    (0x80007540, 64),  # widen around [0x755A]/[0x7568]
    (0x800074A0, 32),  # widen around [0x74A4]
    (0x80008000, 64),  # kernel after the data-pointer area
    (0x800066A0, 32),  # shell flow gate _DAT_80066bc0
]


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


def fmt_diff(addr, recomp_hex, beetle_hex):
    n = min(len(recomp_hex), len(beetle_hex))
    diffs = []
    for i in range(0, n, 2):
        if recomp_hex[i:i+2] != beetle_hex[i:i+2]:
            diffs.append(f"+{i//2}:{recomp_hex[i:i+2]}->{beetle_hex[i:i+2]}")
    if not diffs:
        return "MATCH"
    if len(diffs) > 6:
        diffs = diffs[:6] + [f"... +{len(diffs)-6} more"]
    return "DIFF " + " ".join(diffs)


def main():
    s = socket.create_connection((HOST, PORT))
    s.settimeout(5.0)

    print("=== BEETLE COMPARE (recomp baseline vs Beetle now) ===\n")
    for addr, (length, recomp_hex) in RECOMP.items():
        try:
            r = request(s, 'emu_read_ram', addr=f"0x{addr:08X}", len=length)
            if not r.get('ok'):
                print(f"0x{addr:08X}: ERROR {r}")
                continue
            beetle_hex = r['hex'].lower()
            verdict = fmt_diff(addr, recomp_hex.lower(), beetle_hex)
            print(f"0x{addr:08X} ({length:>3}B):")
            print(f"  recomp: {recomp_hex}")
            print(f"  beetle: {beetle_hex}")
            print(f"  -> {verdict}\n")
        except Exception as e:
            print(f"0x{addr:08X}: EXC {e}\n")

    print("\n=== BEETLE-ONLY EXTRA CONTEXT ===\n")
    for addr, length in EXTRA:
        try:
            r = request(s, 'emu_read_ram', addr=f"0x{addr:08X}", len=length)
            if not r.get('ok'):
                print(f"0x{addr:08X}: ERROR {r}")
                continue
            print(f"0x{addr:08X} ({length:>3}B): {r['hex'].lower()}")
        except Exception as e:
            print(f"0x{addr:08X}: EXC {e}")

    s.close()


if __name__ == '__main__':
    main()
