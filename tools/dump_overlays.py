#!/usr/bin/env python3
"""
dump_overlays.py — Extract executed overlay regions from a live runtime.

Queries the overlay_dump TCP command which:
  1. Scans the dirty_ram bitmap for contiguous dirty pages above --lo
  2. Writes each region as <crc32>.bin in --dir
  3. Returns a JSON manifest

Then writes logs/<game_id>/overlay_map.jsonl from the manifest so the
build tool (B-2) has (load_addr, size, crc32, bytes_file) for each overlay.

Usage:
  python3 tools/dump_overlays.py [--port PORT] [--lo PHYS] [--dir DIR] [--out JSONL]

Defaults:
  port  4470
  lo    0x98000   (above main EXE text)
  dir   logs/SCUS-94236/overlays  (relative to cwd)
  out   logs/SCUS-94236/overlay_map.jsonl
"""

import socket, json, sys, os, argparse

def send_cmd(port, cmd, **kwargs):
    payload = {'id': 1, 'cmd': cmd}
    payload.update(kwargs)
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.sendall(json.dumps(payload).encode() + b'\n')
    data = b''
    s.settimeout(30.0)
    while True:
        chunk = s.recv(65536)
        if not chunk: break
        data += chunk
        try:
            json.loads(data.decode())
            break
        except Exception:
            pass
    s.close()
    return json.loads(data.decode())

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=4470)
    ap.add_argument('--lo',   default='0x98000',
                    help='Low physical address threshold (default: 0x98000)')
    ap.add_argument('--dir',  default='logs/SCUS-94236/overlays',
                    help='Directory for .bin files (created if missing)')
    ap.add_argument('--out',  default='logs/SCUS-94236/overlay_map.jsonl',
                    help='Output JSONL manifest path')
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out) or '.', exist_ok=True)

    print(f'Querying overlay_dump on port {args.port} (lo={args.lo}, dir={args.dir})...')
    result = send_cmd(args.port, 'overlay_dump', lo=args.lo, dir=args.dir)

    if not result.get('ok'):
        print(f'ERROR: {result.get("error")}')
        sys.exit(1)

    regions = result.get('regions', [])
    print(f'Found {len(regions)} overlay region(s).')

    # Read existing JSONL to avoid duplicate entries
    existing = set()
    if os.path.exists(args.out):
        with open(args.out, encoding='utf-8') as f:
            for line in f:
                try:
                    e = json.loads(line)
                    existing.add((e.get('crc32'), e.get('load_addr')))
                except Exception:
                    pass

    new_count = 0
    with open(args.out, 'a', encoding='utf-8') as f:
        for r in regions:
            key = (r['crc32'], r['addr'])
            if key in existing:
                print(f'  skip  {r["addr"]}  {r["size"]} bytes  {r["crc32"]}  (already logged)')
                continue
            entry = {
                'crc32':      r['crc32'],
                'load_addr':  r['addr'],
                'size':       r['size'],
                'bytes_file': f'overlays/{r["file"]}',
            }
            f.write(json.dumps(entry) + '\n')
            existing.add(key)
            new_count += 1
            print(f'  new   {r["addr"]}  {r["size"]} bytes  {r["crc32"]}  -> {r["file"]}')

    print(f'\n{new_count} new overlay(s) appended to {args.out}')

if __name__ == '__main__':
    main()
