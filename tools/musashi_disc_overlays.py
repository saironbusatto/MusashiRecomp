#!/usr/bin/env python3
"""
musashi_disc_overlays.py — Extract Brave Fencer Musashi's code overlays straight
from the disc, for static (B-2) compilation into the binary.

BFM keeps its gameplay code in the MAIN.CD / SCxx.CD archives, not in the EXE:

  archive  : u32 count, u32 0, then count x (u32 first_sector, u32 byte_size)
  entry    : a run of PAC chunks, each starting on a sector boundary
  PAC chunk: "PAC\\0", u8 type, u8 more_follows, u16 0, u32 param, u32 size
             (size includes the 0x800-byte header sector; payload follows it)

The EXE's PAC reader (FUN_8001a338) copies type 1 payloads verbatim to the
caller's destination and streams type 4 payloads through the LZSS decoder at
FUN_80018730 (1 KiB ring in the scratchpad, write cursor starting at 1; flag
byte LSB-first, 1 = literal, 0 = 2-byte ref: offset = b0 | (b1 & 3) << 8,
length = (b1 >> 2) + 2, offset 0 terminates).

The destination is a runtime argument (FUN_80019a24 param_2), not part of the
file, so each code image's load address is recovered from its own absolute
`jal` targets: the base that lands the most targets on in-image function
prologues wins, and only a clear winner is accepted. A wrong base can never run
wrong code — the loader dispatches a compiled function only while live RAM
matches its compiled-from bytes — it would just never match.

Output is overlay_captures.json schema v2, one record per distinct
(load_addr, image), with `function_entry_pcs` seeded from direct-call targets
and code pointers found anywhere in the EXE or any code image, plus stack-frame
prologues that follow a `jr ra`.

Usage:
  python3 tools/musashi_disc_overlays.py \\
      --disc "games/musashi/Brave Fencer Musashi (USA).cue" \\
      --exe  games/musashi/SLUS_007.26 \\
      --out  build/musashi_disc_overlays.json
"""

import argparse
import base64
import hashlib
import json
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_overlays import DiscReader, enumerate_files, parse_cue  # noqa: E402
from compile_overlays import (_callable_direct_jal_target,  # noqa: E402
                              _callable_legacy_seed, _is_addiu_sp_neg, _is_jr_ra,
                              _is_valid_mips_word)

SECTOR = 0x800
PAC_MAGIC = b'PAC\0'
PAC_RAW, PAC_LZ = 1, 4
EXE_HEADER = 0x800
RAM_HI = 0x80200000

# Base acceptance (per signal, see infer_base): the winning base must collect
# at least MIN_VOTES references that land on prologues and beat the runner-up
# by DOMINANCE x. Images with no clear, consistent winner are skipped (they
# keep running in the dirty-RAM interpreter).
MIN_VOTES = 16
DOMINANCE = 1.5


def lz_decode(src: bytes) -> bytes:
    ring = bytearray(0x400)
    rp = 1
    out = bytearray()
    i = 0
    while i < len(src):
        flags = src[i]
        i += 1
        for bit in range(8):
            if i >= len(src):
                return bytes(out)
            if flags & (1 << bit):
                b = src[i]
                i += 1
                ring[rp] = b
                rp = (rp + 1) & 0x3FF
                out.append(b)
                continue
            b0, b1 = src[i], src[i + 1]
            i += 2
            off = b0 | ((b1 & 3) << 8)
            if off == 0:
                return bytes(out)
            for _ in range((b1 >> 2) + 2):
                b = ring[off]
                off = (off + 1) & 0x3FF
                ring[rp] = b
                rp = (rp + 1) & 0x3FF
                out.append(b)
    return bytes(out)


def pac_code_images(archive: bytes):
    """Yield (entry, chunk_offset, type, image) for every type 1/4 PAC chunk."""
    count = struct.unpack_from('<I', archive, 0)[0]
    for e in range(count):
        sector, esize = struct.unpack_from('<II', archive, 8 + e * 8)
        o, end = sector * SECTOR, sector * SECTOR + esize
        while o < end and archive[o:o + 4] == PAC_MAGIC:
            typ = archive[o + 4]
            size = struct.unpack_from('<I', archive, o + 12)[0]
            if size < SECTOR or o + size > len(archive):
                break
            payload = archive[o + SECTOR:o + size]
            if typ == PAC_RAW:
                yield e, o, typ, payload
            elif typ == PAC_LZ:
                yield e, o, typ, lz_decode(payload)
            o += (size + SECTOR - 1) & ~(SECTOR - 1)


def words(blob: bytes):
    return struct.unpack_from(f'<{len(blob) // 4}I', blob)


def jal_targets(blob: bytes):
    return [((w & 0x3FFFFFF) << 2) | 0x80000000 for w in words(blob) if w >> 26 == 3]


def _vote(refs: Counter, prologues: list, ram_lo: int):
    """(base, votes, runner_up) for the base landing most refs on prologues."""
    votes = Counter()
    for target, n in refs.items():
        for p in prologues:
            base = target - p
            if ram_lo <= base < RAM_HI:
                votes[base] += n
    top = votes.most_common(2)
    if not top:
        return None, 0, 0
    return top[0][0], top[0][1], (top[1][1] if len(top) > 1 else 0)


def _clear(vote) -> bool:
    base, votes, runner = vote
    return base is not None and votes >= MIN_VOTES and votes >= DOMINANCE * runner


def infer_base(image: bytes, ram_lo: int):
    """Return (base, votes, runner_up_votes), base None when unproven.

    Two independent signals from the image's own absolute code references, each
    voting for every base that lands a reference on an in-image prologue:
    `jal` targets, and word-sized pointers (function tables, actor callbacks).
    Pointers are noisy in data-heavy images and jals in small modules that
    mostly call out, so a base is accepted when jal, pointer or their sum has a
    clear winner and every clear winner names the same base.
    Bases below ram_lo (the end of the resident EXE text) are impossible: an
    overlay never loads over the EXE that loads it."""
    ws = words(image)
    prologues = [i * 4 for i, w in enumerate(ws)
                 if (w & 0xFFFF8000) == 0x27BD8000]       # addiu $sp,$sp,-N
    jal_refs = Counter(jal_targets(image))
    ptr_refs = Counter(w for w in ws if ram_lo <= w < RAM_HI and w % 4 == 0)
    jal = _vote(jal_refs, prologues, ram_lo)
    ptr = _vote(ptr_refs, prologues, ram_lo)
    both = _vote(jal_refs + ptr_refs, prologues, ram_lo)
    clear = [v for v in (jal, ptr, both) if _clear(v)]
    if not clear or len({v[0] for v in clear}) > 1:
        best = max(jal, ptr, both, key=lambda v: v[1])
        return None, best[1], best[2]
    return clear[0]


def global_refs(blobs) -> tuple:
    """(jal targets, word-sized pointers) found in `blobs`. Overlays call into
    each other — scene modules jal into the resident MAIN module — so an
    image's entries are referenced from outside it too."""
    calls, ptrs = set(), set()
    for blob in blobs:
        calls.update(jal_targets(blob))
        ptrs.update(w for w in words(blob) if 0x80000000 <= w < RAM_HI and w % 4 == 0)
    return calls, ptrs


def entry_seeds(image: bytes, base: int, calls: set, ptrs: set) -> list:
    end = base + len(image)
    seeds = {t for t in calls if base <= t < end and _callable_direct_jal_target(image, base, t)}
    seeds |= {p for p in ptrs if base <= p < end and _callable_legacy_seed(image, base, p)}
    # Compiler function boundaries: a stack-frame prologue right after the
    # previous function's `jr ra` + delay slot, whether or not anything we can
    # see references it (calls through register-built pointers, lui/addiu).
    ws = words(image)
    seeds |= {base + i * 4 for i in range(2, len(ws))
              if _is_jr_ra(ws[i - 2]) and _is_addiu_sp_neg(ws[i])}
    # Resident modules lead with a small header word (not an instruction);
    # their entry point is the first word after it.
    if len(ws) > 1 and not _is_valid_mips_word(ws[0]) and _is_valid_mips_word(ws[1]):
        seeds.add(base + 4)
    return sorted(seeds)


def read_archives(cue: str) -> dict:
    bin_path, is_raw = parse_cue(cue)
    disc = DiscReader(bin_path, raw=is_raw)
    try:
        return {path: disc.read_file_bytes(lba, size)
                for path, lba, size in enumerate_files(disc)
                if path.upper().endswith('.CD') and path.upper() != 'LIST.CD'}
    finally:
        disc.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disc', required=True, help='BFM .cue')
    ap.add_argument('--exe', required=True, help='SLUS_007.26 (PS-X EXE)')
    ap.add_argument('--out', required=True, help='output overlay_captures-format JSON')
    args = ap.parse_args()

    exe = open(args.exe, 'rb').read()
    t_addr, t_size = struct.unpack_from('<II', exe, 0x18)
    exe_text = exe[EXE_HEADER:EXE_HEADER + t_size]
    ram_lo = t_addr + t_size

    accepted, seen, skipped = [], set(), []
    for name, archive in sorted(read_archives(args.disc).items()):
        for e, off, typ, image in pac_code_images(archive):
            image = image[:len(image) // 4 * 4]
            key = hashlib.sha1(image).digest()
            if key in seen:
                continue
            seen.add(key)
            src = f'{name}#{e}@0x{off:X}'
            base, votes, runner = infer_base(image, ram_lo)
            if base is None:
                skipped.append((src, len(image), votes, runner))
                continue
            accepted.append((src, typ, base, image, votes, runner))

    # Images sharing a load slot replace each other, so a reference from one is
    # meaningless inside another: only the EXE, the image itself and modules in
    # other slots can point at an image's entries.
    by_slot = {}
    for a in accepted:
        by_slot.setdefault(a[2], []).append(a[3])
    slot_refs = {b: global_refs(imgs) for b, imgs in by_slot.items()}
    exe_refs = global_refs([exe_text])
    records = []
    for src, typ, base, image, votes, runner in accepted:
        own = global_refs([image])
        calls = exe_refs[0] | own[0]
        ptrs = exe_refs[1] | own[1]
        for b, (c, p) in slot_refs.items():
            if b != base:
                calls |= c
                ptrs |= p
        seeds = entry_seeds(image, base, calls, ptrs)
        if not seeds:
            skipped.append((src, len(image), votes, runner))
            continue
        records.append({
            'schema': 'psxrecomp overlay capture v2',
            'source': src,
            'load_addr': f'0x{base:08X}',
            'size': len(image),
            'bytes_b64': base64.b64encode(image).decode('ascii'),
            'function_entry_pcs': [f'0x{s:08X}' for s in seeds],
        })
        print(f'{src:28s} type {typ}  base 0x{base:08X}  {len(image):7d} B  '
              f'votes {votes}/{runner}  seeds {len(seeds)}')

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w') as f:
        json.dump(records, f)
    print(f'\n{len(records)} code image(s) -> {args.out}')
    print(f'{len(skipped)} image(s) without a provable load address (left to the interpreter):')
    for src, size, votes, runner in skipped:
        print(f'  {src:28s} {size:7d} B  votes {votes}/{runner}')


if __name__ == '__main__':
    main()
