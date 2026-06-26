#!/usr/bin/env python3
"""
compile_overlays.py — Offline overlay compilation primitive (B-2 / A-2 shared path).

Reads overlay_captures.json, compiles each code overlay to a DLL, and writes
it to the cache directory.  The runtime's A-1 LoadLibrary path reads from the
same cache.

Usage:
  python3 tools/compile_overlays.py \\
      --captures  build-dev/overlay_captures.json \\
      --game-toml game.toml \\
      --recompiler psxrecomp/recompiler/build/psxrecomp-game.exe \\
      --runtime-include psxrecomp/runtime/include \\
      --out-dir   build-dev/cache

Each DLL is written to <out-dir>/<game-id>/<crc32hex>.dll.
Each DLL exports:
  overlay_init(dispatch_fn)  — call once after LoadLibrary to wire dispatch
  func_XXXXXXXX(CPUState*)   — one export per compiled function entry point
"""

import argparse
import base64
import binascii
from collections import Counter, deque
import os
import re
import struct
import subprocess
import sys
import tempfile

try:
    import tomllib  # Python 3.11+
except ImportError:
    try:
        import tomli as tomllib
    except ImportError:
        print("ERROR: need tomllib (Python 3.11+) or 'pip install tomli'")
        sys.exit(1)

import json
import platform
import re


def codegen_ver(runtime_include: str) -> int:
    """Parse PSX_OVERLAY_CODEGEN_VER from overlay_api.h so the cache path version
    is the SAME value the runtime (overlay_loader.c) uses — the two can't drift.
    The cache is namespaced gcc/<arch-abi>/cg<N>/, so a build with new emitter
    output reads+writes a FRESH dir and never reuses a stale DLL."""
    hdr = os.path.join(runtime_include, 'overlay_api.h')
    with open(hdr) as f:
        m = re.search(r'#define\s+PSX_OVERLAY_CODEGEN_VER\s+(\d+)', f.read())
    if not m:
        raise SystemExit(f'PSX_OVERLAY_CODEGEN_VER not found in {hdr}')
    return int(m.group(1))


def codegen_hash(runtime_include: str) -> int:
    """Parse PSX_OVERLAY_CODEGEN_HASH from the build-generated overlay_codegen_hash.h
    (next to overlay_api.h). Folded into the cache path as cg<N>_<hash> so ANY
    emitter change auto-invalidates the cache (no stale-but-cgN reuse). Falls back
    to 0 when the header is absent (a tree not yet built) — matching overlay_api.h's
    __has_include fallback, so the loader and compiler still agree on the path."""
    hdr = os.path.join(runtime_include, 'overlay_codegen_hash.h')
    try:
        with open(hdr) as f:
            m = re.search(r'#define\s+PSX_OVERLAY_CODEGEN_HASH\s+0x([0-9A-Fa-f]+)', f.read())
        if m:
            return int(m.group(1), 16)
    except FileNotFoundError:
        pass
    return 0


def cache_arch_abi() -> str:
    """Canonical cache arch-abi tag, IDENTICAL to overlay_loader.c's
    PSX_OVERLAY_ARCH_ABI ("<os>-<arch>": win|linux|macos + x64|arm64|x86).
    gcc DLLs are namespaced under <game_id>/gcc/<arch-abi>/ so same-OS
    different-arch caches (and, later, sljit blobs) never comingle. Keep this
    mapping in lockstep with overlay_loader.c."""
    sysname = platform.system()
    os_tag = {'Windows': 'win', 'Darwin': 'macos'}.get(sysname, 'linux')
    m = platform.machine().lower()
    if m in ('amd64', 'x86_64', 'x64'):
        arch = 'x64'
    elif m in ('arm64', 'aarch64'):
        arch = 'arm64'
    elif m in ('i386', 'i686', 'x86'):
        arch = 'x86'
    else:
        arch = 'unknown'
    return f'{os_tag}-{arch}'


# ---------------------------------------------------------------------------
# PS-EXE fake header
# ---------------------------------------------------------------------------

def make_psxexe(load_addr: int, entry_pc: int, data: bytes) -> bytes:
    """Wrap raw overlay bytes in a minimal PS-EXE header."""
    header = bytearray(2048)
    header[0:8]   = b'PS-X EXE'
    struct.pack_into('<I', header, 0x10, entry_pc)   # initial PC
    struct.pack_into('<I', header, 0x14, 0)           # initial GP
    struct.pack_into('<I', header, 0x18, load_addr)   # load address
    struct.pack_into('<I', header, 0x1C, len(data))   # text size
    return bytes(header) + data


# ---------------------------------------------------------------------------
# Overlay seed and generated-C audits
# ---------------------------------------------------------------------------

INCLUDE_REASONS = {
    'DISPATCH_ENTRY',
    'DIRECT_JAL_TARGET',
    'FUNCTION_POINTER_TARGET',
    'TOML_DECLARED_ENTRY',
    # Dispatch-proven PC with no callable boundary (mid-function code reached
    # through a function pointer or dispatch chain). Compiled as an
    # overlapping-alias entry into its host function — written to the seeds
    # file as 'interior 0x...' so the recompiler never uses it as a walk root
    # (a walk root there would truncate the host: the mid-function-seed
    # softlock class).
    'DISPATCH_INTERIOR',
    # Kernel-window-only promotion of an ORPHAN dispatch interior: a
    # dispatch-proven PC in kernel RAM [0, 0x10000) that no rooted walk
    # covers. There is no host to alias into and no host a root could
    # truncate — the static recompiler's install-slot hooks tail-dispatch
    # into exactly such PCs (e.g. RAM 0xCF0, the SIO data-byte stub).
    # Written to the seeds file as 'dispatch_root 0x...': a trusted walk
    # root, exempt from the recompiler's boundary re-check. Overlay regions
    # are NOT eligible: per-PC dispatch evidence persists across scene
    # variants there, so an orphan may belong to a non-resident variant
    # whose bytes in THIS image are data (the 0xE889C class) — rooting it
    # would walk garbage.
    'DISPATCH_ROOT',
}
FATAL_SEED_REASONS = {'BRANCH_TARGET_ONLY', 'OBSERVED_PC_ONLY', 'UNKNOWN'}


def _parse_addr(value) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 16)


def _parse_addr_list(values) -> set[int]:
    out = set()
    for v in values or []:
        try:
            out.add(_parse_addr(v))
        except (TypeError, ValueError):
            pass
    return out


def _word_at(data: bytes, load_addr: int, addr: int):
    off = addr - load_addr
    if off < 0 or off + 4 > len(data):
        return None
    return struct.unpack_from('<I', data, off)[0]


def _is_jr_ra(word) -> bool:
    return word == 0x03E00008


def _is_addiu_sp_neg(word) -> bool:
    if word is None:
        return False
    return ((word >> 26) & 0x3F) == 0x09 \
        and ((word >> 21) & 0x1F) == 29 \
        and ((word >> 16) & 0x1F) == 29 \
        and (word & 0x8000) != 0


def _is_control_flow(word) -> bool:
    if word is None:
        return False
    op = (word >> 26) & 0x3F
    fn = word & 0x3F
    return op in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                  0x14, 0x15, 0x16, 0x17) or (op == 0 and fn in (0x08, 0x09))


def _is_valid_mips_word(word) -> bool:
    if word is None or word in (0xFFFFFFFF, 0xFFFFFFFD):
        return False

    op = (word >> 26) & 0x3F
    fn = word & 0x3F
    rt = (word >> 16) & 0x1F

    if op == 0x00:
        return fn in {
            0x00, 0x02, 0x03, 0x04, 0x06, 0x07,
            0x08, 0x09, 0x0C, 0x0D,
            0x10, 0x11, 0x12, 0x13,
            0x18, 0x19, 0x1A, 0x1B,
            0x20, 0x21, 0x22, 0x23,
            0x24, 0x25, 0x26, 0x27,
            0x2A, 0x2B,
        }
    if op == 0x01:
        return rt in (0x00, 0x01, 0x10, 0x11)

    return op in {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x12,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x28, 0x29, 0x2A, 0x2B, 0x2E,
        0x30, 0x32, 0x38, 0x3A,
    }


def _jump_target(pc: int, word: int) -> int:
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def _branch_target(pc: int, word: int) -> int:
    imm = word & 0xFFFF
    if imm & 0x8000:
        imm -= 0x10000
    return pc + 4 + (imm << 2)


def _classify_cf(pc: int, word: int) -> tuple[str, int]:
    op = (word >> 26) & 0x3F
    fn = word & 0x3F
    rs = (word >> 21) & 0x1F
    if op == 0 and fn == 0x08:
        return ('jr_ra' if rs == 31 else 'jr', 0)
    if op == 0 and fn == 0x09:
        return ('jalr', 0)
    if op == 0x02:
        return ('j', _jump_target(pc, word))
    if op == 0x03:
        return ('jal', _jump_target(pc, word))
    if op in (0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
        return ('branch', _branch_target(pc, word))
    return ('normal', 0)


def _find_jump_table_targets(data: bytes, load_addr: int, size: int,
                             entry: int, hard_cap: int,
                             jr_pc: int, jr_rs: int) -> set[int]:
    """Recognize the compiler's local jump-table idiom feeding `jr reg`."""
    lw_base = None
    lw_offset = 0
    addu_cand = [None, None]
    lui_val = None
    addiu_val = [0, 0]
    found_addiu = [False, False]
    table_count = 0

    for back in range(1, 41):
        scan_addr = jr_pc - back * 4
        if scan_addr < entry:
            break
        word = _word_at(data, load_addr, scan_addr)
        if word is None:
            break
        op = (word >> 26) & 0x3F
        rs = (word >> 21) & 0x1F
        rt = (word >> 16) & 0x1F
        rd = (word >> 11) & 0x1F
        fn = word & 0x3F

        if op == 0x23 and rt == jr_rs and lw_base is None:
            lw_base = rs
            lw_offset = word & 0xFFFF
            if lw_offset & 0x8000:
                lw_offset -= 0x10000
            continue
        if op == 0x00 and fn == 0x21 and lw_base is not None and rd == lw_base \
                and addu_cand[0] is None:
            addu_cand = [rs, rt]
            continue
        if op == 0x09 and addu_cand[0] is not None:
            for i, cand in enumerate(addu_cand):
                if cand is not None and not found_addiu[i] and rs == cand and rt == cand:
                    imm = word & 0xFFFF
                    if imm & 0x8000:
                        imm -= 0x10000
                    addiu_val[i] = imm
                    found_addiu[i] = True
                    break
            continue
        if op == 0x0F and addu_cand[0] is not None and lui_val is None:
            for i, cand in enumerate(addu_cand):
                if cand is not None and rt == cand:
                    lui_val = (word & 0xFFFF) << 16
                    addiu_val[0] = addiu_val[i] if found_addiu[i] else 0
                    found_addiu[0] = found_addiu[i]
                    break
            continue
        if op == 0x0B and table_count == 0:
            table_count = word & 0xFFFF
            continue
        if lui_val is not None and table_count:
            break

    if lui_val is None or table_count == 0 or table_count >= 512:
        return set()

    table_base = lui_val + (addiu_val[0] if found_addiu[0] else 0) + lw_offset
    lo = load_addr
    hi = load_addr + size
    targets = set()
    for i in range(table_count):
        target = _word_at(data, load_addr, table_base + i * 4)
        if target is None:
            continue
        if lo <= target < hi and entry <= target < hard_cap and (target & 3) == 0:
            targets.add(target)
    return targets


def _callable_legacy_seed(data: bytes, load_addr: int, addr: int) -> bool:
    """Conservative fallback for old captures that only had a `seeds` list."""
    word = _word_at(data, load_addr, addr)
    prev = _word_at(data, load_addr, addr - 4)
    prev8 = _word_at(data, load_addr, addr - 8)
    if not _is_valid_mips_word(word):
        return False
    if addr == load_addr:
        return True
    if _is_jr_ra(prev8):
        return True
    return _is_addiu_sp_neg(word) and not _is_control_flow(prev)


def _callable_direct_jal_target(data: bytes, load_addr: int, addr: int) -> bool:
    """True only when a direct JAL target has independent boundary evidence."""
    return _callable_legacy_seed(data, load_addr, addr)


def _walk_overlay_function(data: bytes, load_addr: int, size: int,
                           entry: int, hard_cap: int) -> dict:
    lo = load_addr
    hi = load_addr + size

    def in_function(addr: int) -> bool:
        return lo <= addr < hi and entry <= addr < hard_cap and (addr & 3) == 0

    work = deque([entry])
    visited = set()
    direct_jals = set()
    branch_targets = set()
    jump_table_targets = set()

    while work:
        pc = work.popleft()
        if pc in visited or not in_function(pc):
            continue
        word = _word_at(data, load_addr, pc)
        if word is None:
            continue
        visited.add(pc)
        kind, target = _classify_cf(pc, word)
        delay = pc + 4

        if kind == 'normal':
            if in_function(pc + 4):
                work.append(pc + 4)
        elif kind == 'branch':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
            if in_function(target):
                work.append(target)
                branch_targets.add(target)
        elif kind == 'j':
            if in_function(delay):
                visited.add(delay)
            if in_function(target):
                work.append(target)
                branch_targets.add(target)
        elif kind == 'jal':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
            if (lo <= target < hi and (target & 3) == 0 and
                    _callable_direct_jal_target(data, load_addr, target)):
                direct_jals.add(target)
        elif kind == 'jalr':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
        elif kind == 'jr':
            if in_function(delay):
                visited.add(delay)
            jr_rs = (word >> 21) & 0x1F
            for jt in _find_jump_table_targets(data, load_addr, size,
                                               entry, hard_cap, pc, jr_rs):
                jump_table_targets.add(jt)
                branch_targets.add(jt)
                if in_function(jt):
                    work.append(jt)
        elif kind == 'jr_ra':
            if in_function(delay):
                visited.add(delay)

    return {
        'visited': visited,
        'direct_jals': direct_jals,
        'branch_targets': branch_targets,
        'jump_table_targets': jump_table_targets,
    }


def _collect_toml_overlay_entries(toml_doc: dict, load_addr: int, crc32: int) -> set[int]:
    entries = set()
    for ov in toml_doc.get('overlays', []) or []:
        if not isinstance(ov, dict):
            continue
        ov_load = ov.get('load_addr') or ov.get('load_address')
        if ov_load is not None and _parse_addr(ov_load) != load_addr:
            continue
        ov_crc = ov.get('bytes_crc') or ov.get('crc32') or ov.get('crc')
        if ov_crc is not None and _parse_addr(ov_crc) != crc32:
            continue
        for key in ('entry', 'entries', 'function_entry_pcs', 'function_entries'):
            if key not in ov:
                continue
            val = ov[key]
            if isinstance(val, list):
                entries.update(_parse_addr_list(val))
            else:
                entries.add(_parse_addr(val))
    return entries


def classify_overlay_seeds(cap: dict, data: bytes, load_addr: int, size: int,
                           crc32: int, toml_doc: dict) -> tuple[list[str], dict]:
    lo = load_addr
    hi = load_addr + size
    region = lambda a: lo <= a < hi and (a & 3) == 0

    schema_keys = {'executed_pcs', 'observed_pcs', 'dispatch_entry_pcs',
                   'function_entry_pcs'}
    has_split_schema = any(k in cap for k in schema_keys)

    legacy_seeds = _parse_addr_list(cap.get('seeds', []))
    executed_pcs = _parse_addr_list(cap.get('executed_pcs',
                                            cap.get('observed_pcs',
                                                    legacy_seeds if not has_split_schema else [])))
    dispatch_entry_pcs = _parse_addr_list(cap.get('dispatch_entry_pcs', []))
    captured_function_entries = _parse_addr_list(cap.get('function_entry_pcs', []))
    toml_entries = _collect_toml_overlay_entries(toml_doc, load_addr, crc32)
    legacy_callable_seeds = {a for a in legacy_seeds
                             if region(a) and _callable_legacy_seed(data, load_addr, a)}

    included: dict[int, str] = {}
    excluded: dict[int, str] = {}
    game_text = _game_text_range(toml_doc)

    # A dispatch into dirty RAM can land on a jump-table case label. That
    # proves coverage, not a callable function boundary, so discover those
    # labels before promoting dispatch entries to seeds.
    pre_roots = set(legacy_callable_seeds) | set(captured_function_entries) | set(toml_entries)
    pre_roots.update(a for a in dispatch_entry_pcs
                     if region(a) and _callable_legacy_seed(data, load_addr, a))
    jump_table_targets = set()
    pre_roots_sorted = sorted(a for a in pre_roots if region(a))
    for i, entry in enumerate(pre_roots_sorted):
        hard_cap = pre_roots_sorted[i + 1] if i + 1 < len(pre_roots_sorted) else hi
        walk = _walk_overlay_function(data, load_addr, size, entry, hard_cap)
        jump_table_targets.update(walk['jump_table_targets'])

    def impossible_entry_start(addr: int) -> bool:
        word = _word_at(data, load_addr, addr)
        if word is None:
            return True
        if not _is_valid_mips_word(word):
            return True
        kind, target = _classify_cf(addr, word)
        if kind == 'j':
            return _classify_target(target, load_addr, size, game_text) == 'UNKNOWN_BAD'
        if kind == 'jal' and region(target):
            return not _callable_direct_jal_target(data, load_addr, target)
        return False

    def include(addr: int, reason: str):
        if not region(addr):
            excluded[addr] = 'UNKNOWN'
            return
        # Boundary gate: a dispatched-to PC is proof of *code reachability*, not
        # of a *callable function boundary*. A jr-driven jump-table case label
        # or a mid-function pointer target can be dispatched-to yet have no
        # prologue and no preceding jr $ra. Promoting it to a WALK ROOT would
        # truncate its host function (mid-function-seed softlock class) or run
        # off into adjacent data. Such PCs are dispatch-proven code, though —
        # classify them DISPATCH_INTERIOR: the recompiler emits them as
        # overlapping-alias entries into their host (never walk roots).
        # Invalid words stay excluded. Call-edge-proven reasons
        # (DIRECT_JAL_TARGET, FUNCTION_POINTER_TARGET, TOML_DECLARED_ENTRY) are
        # exempt — they carry their own proof.
        if reason == 'DISPATCH_ENTRY':
            if impossible_entry_start(addr):
                excluded[addr] = 'UNKNOWN'
                return
            if (addr in jump_table_targets or
                    not _callable_legacy_seed(data, load_addr, addr)):
                included.setdefault(addr, 'DISPATCH_INTERIOR')
                return
        elif (reason != 'TOML_DECLARED_ENTRY' and addr in jump_table_targets and
                not _callable_legacy_seed(data, load_addr, addr)):
            excluded[addr] = 'BRANCH_TARGET_ONLY'
            return
        if reason in FATAL_SEED_REASONS:
            raise RuntimeError(f'BUG: refusing to include 0x{addr:08X} as {reason}')
        old = included.get(addr)
        if old is None or old in ('FUNCTION_POINTER_TARGET', 'DISPATCH_INTERIOR'):
            included[addr] = reason

    for addr in dispatch_entry_pcs:
        include(addr, 'DISPATCH_ENTRY')
    for addr in captured_function_entries:
        include(addr, 'FUNCTION_POINTER_TARGET')
    for addr in toml_entries:
        include(addr, 'TOML_DECLARED_ENTRY')

    legacy_seed_mode = bool(legacy_seeds) and not cap.get('schema')
    if legacy_seed_mode:
        # Old capture files used `seeds` for interpreter-observed PCs. Include
        # only PCs whose surrounding bytes look callable; classify the rest
        # after we know branch targets from accepted roots.
        for addr in legacy_callable_seeds:
            include(addr, 'FUNCTION_POINTER_TARGET')

    # Walk roots: callable entries only. DISPATCH_INTERIOR addresses are NOT
    # roots — as roots they would hard-cap (truncate) the sibling walk that
    # owns them.
    known = {a for a, r in included.items() if r != 'DISPATCH_INTERIOR'}
    pending = deque(sorted(known))
    processed = set()
    all_branch_targets = set()
    kernel_window = (load_addr & 0x1FFFFFFF) < 0x10000

    while True:
        while pending:
            entry = pending.popleft()
            if entry in processed:
                continue
            processed.add(entry)
            sorted_known = sorted(known)
            hard_cap = next((x for x in sorted_known if x > entry), hi)
            walk = _walk_overlay_function(data, load_addr, size, entry, hard_cap)
            all_branch_targets.update(walk['branch_targets'])
            all_branch_targets.update(walk['jump_table_targets'])
            for target in sorted(walk['direct_jals']):
                if target not in known:
                    include(target, 'DIRECT_JAL_TARGET')
                    if target in included:
                        known.add(target)
                        pending.append(target)

        if not kernel_window:
            break

        # Orphan promotion (see DISPATCH_ROOT in INCLUDE_REASONS): a kernel
        # dispatch interior that no rooted walk covers is promoted to a
        # trusted walk root. Re-enter the walk loop — the new root's walk
        # may cover other interiors or discover direct-jal callees.
        covered = set()
        sorted_known = sorted(known)
        for i, entry in enumerate(sorted_known):
            hard_cap = sorted_known[i + 1] if i + 1 < len(sorted_known) else hi
            walk = _walk_overlay_function(data, load_addr, size, entry, hard_cap)
            covered |= walk['visited']
        promoted = sorted(a for a, r in included.items()
                          if r == 'DISPATCH_INTERIOR' and a not in covered
                          and _is_valid_mips_word(_word_at(data, load_addr, a)))
        if not promoted:
            break
        # Promote ONE orphan per iteration (lowest address first): its walk
        # usually covers the remaining orphans, which then stay interiors and
        # alias into the new root as host — rather than minting sibling roots
        # that split one real function and hard-cap each other.
        a = promoted[0]
        included[a] = 'DISPATCH_ROOT'
        known.add(a)
        pending.append(a)

    # Re-walk with the final function set so the branch-target exclusion count
    # matches the actual compilation boundaries.
    all_branch_targets.clear()
    sorted_known = sorted(known)
    for i, entry in enumerate(sorted_known):
        hard_cap = sorted_known[i + 1] if i + 1 < len(sorted_known) else hi
        walk = _walk_overlay_function(data, load_addr, size, entry, hard_cap)
        all_branch_targets.update(walk['branch_targets'])
        all_branch_targets.update(walk['jump_table_targets'])

    candidates = {a for a in (executed_pcs | dispatch_entry_pcs |
                              captured_function_entries | legacy_seeds | toml_entries)
                  if region(a)}
    for addr in sorted(candidates - set(included)):
        if addr in all_branch_targets or addr in jump_table_targets:
            excluded[addr] = 'BRANCH_TARGET_ONLY'
        elif addr in executed_pcs or addr in legacy_seeds:
            excluded[addr] = 'OBSERVED_PC_ONLY'
        else:
            excluded[addr] = 'UNKNOWN'

    bad_included = [(a, r) for a, r in included.items() if r in FATAL_SEED_REASONS]
    if bad_included:
        details = ', '.join(f'0x{a:08X}:{r}' for a, r in bad_included)
        raise RuntimeError(f'fatal seed classification: {details}')

    counts = Counter(included.values())
    excluded_counts = Counter(excluded.values())
    audit = {
        'load_addr': load_addr,
        'crc32': crc32,
        'lo': lo,
        'hi': hi,
        'executed_pcs': executed_pcs,
        'dispatch_entry_pcs': dispatch_entry_pcs,
        'function_entry_pcs': set(included),
        'included_reasons': included,
        'excluded_reasons': excluded,
        'branch_targets_excluded_count': len(all_branch_targets - set(included)),
        'counts': counts,
        'excluded_counts': excluded_counts,
    }
    # Interior entries carry the 'interior' marker so the recompiler emits
    # them as overlapping aliases, never as walk roots. Promoted kernel
    # orphans carry 'dispatch_root' so the recompiler roots them without
    # boundary re-verification.
    def seed_line(addr: int) -> str:
        r = included[addr]
        if r == 'DISPATCH_INTERIOR':
            return f'interior 0x{addr:08X}'
        if r == 'DISPATCH_ROOT':
            return f'dispatch_root 0x{addr:08X}'
        return f'0x{addr:08X}'
    seeds = [seed_line(addr) for addr in sorted(included)]
    return seeds, audit


def print_seed_audit(audit: dict) -> None:
    print(f'Overlay {audit["load_addr"]:08X}_{audit["crc32"]:08X}')
    print(f'Region: {audit["lo"]:08X}..{audit["hi"] - 1:08X}')
    print(f'executed_pcs: {len(audit["executed_pcs"])}')
    print(f'dispatch_entry_pcs: {len(audit["dispatch_entry_pcs"])}')
    print(f'function_entry_pcs: {len(audit["function_entry_pcs"])}')
    print(f'direct_jal_targets_included: {audit["counts"].get("DIRECT_JAL_TARGET", 0)}')
    print(f'function_pointer_targets_included: {audit["counts"].get("FUNCTION_POINTER_TARGET", 0)}')
    print(f'toml_entries_included: {audit["counts"].get("TOML_DECLARED_ENTRY", 0)}')
    print(f'dispatch_interior_included: {audit["counts"].get("DISPATCH_INTERIOR", 0)}')
    print(f'dispatch_roots_promoted: {audit["counts"].get("DISPATCH_ROOT", 0)}')
    print(f'branch_targets_excluded: {audit["branch_targets_excluded_count"]}')
    print(f'observed_only_excluded: {audit["excluded_counts"].get("OBSERVED_PC_ONLY", 0)}')
    print(f'unknown_excluded: {audit["excluded_counts"].get("UNKNOWN", 0)}')
    for addr in sorted(audit['included_reasons']):
        print(f'  {addr:08X}  {audit["included_reasons"][addr]}')
    for addr in sorted(audit['excluded_reasons']):
        reason = audit['excluded_reasons'][addr]
        if reason in ('BRANCH_TARGET_ONLY', 'OBSERVED_PC_ONLY', 'UNKNOWN'):
            print(f'  {addr:08X}  excluded: {reason}')


def _game_text_range(toml_doc: dict) -> tuple[int, int]:
    game = toml_doc.get('game', {})
    load = game.get('load_address')
    size = game.get('text_size')
    if load is None or size is None:
        return (0, 0)
    lo = _parse_addr(load) & 0x1FFFFFFF
    hi = lo + _parse_addr(size)
    return lo, hi


def _classify_target(addr: int, load_addr: int, size: int,
                     game_text: tuple[int, int]) -> str:
    phys = addr & 0x1FFFFFFF
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size
    if ov_lo <= phys < ov_hi:
        return 'INSIDE_OVERLAY'
    game_lo, game_hi = game_text
    if game_hi > game_lo and game_lo <= phys < game_hi:
        return 'MAIN_EXE'
    if 0x1FC00000 <= phys < 0x1FC80000 or 0xBFC00000 <= addr < 0xBFC80000:
        return 'BIOS'
    return 'UNKNOWN_BAD'


def audit_generated_c(src: str, load_addr: int, size: int,
                      crc32: int, toml_doc: dict) -> dict:
    defs = {int(x, 16) for x in re.findall(
        r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)$', src, re.MULTILINE)}
    decls = {int(x, 16) for x in re.findall(
        r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\);$', src, re.MULTILINE)}
    direct_calls = [int(x, 16) for x in re.findall(
        r'\bfunc_([0-9A-Fa-f]{8})\(cpu\)', src)]
    literal_cba = [int(x, 16) for x in re.findall(
        r'\bcall_by_address\(cpu,\s*0x([0-9A-Fa-f]{8})u?\)', src)]
    unsupported_todo_addrs = {int(x, 16) for x in re.findall(
        r'TODO:[^\n]*?0x([0-9A-Fa-f]{8}):', src)}

    game_text = _game_text_range(toml_doc)
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_overlay(addr: int) -> bool:
        phys = addr & 0x1FFFFFFF
        return ov_lo <= phys < ov_hi

    unknown_bad = set()
    missing_direct = set()
    direct_outside = set()
    for addr in direct_calls:
        if not in_overlay(addr):
            direct_outside.add(addr)
            unknown_bad.add(addr)
        elif addr not in defs:
            missing_direct.add(addr)
            unknown_bad.add(addr)

    cba_classes = Counter()
    for addr in literal_cba:
        cls = _classify_target(addr, load_addr, size, game_text)
        cba_classes[cls] += 1
        if cls == 'UNKNOWN_BAD':
            unknown_bad.add(addr)

    decl_without_def = {a for a in decls if in_overlay(a) and a not in defs}
    called_decl_without_def = decl_without_def & set(direct_calls)
    if called_decl_without_def:
        unknown_bad.update(called_decl_without_def)

    report = {
        'defs': defs,
        'decls': decls,
        'direct_calls': direct_calls,
        'literal_cba': literal_cba,
        'direct_inside': sum(1 for a in direct_calls if in_overlay(a)),
        'external_cba': sum(1 for a in literal_cba if not in_overlay(a)),
        'bios_calls': cba_classes.get('BIOS', 0),
        'syscall_uses': len(re.findall(r'\bpsx_syscall\s*\(', src)),
        'unknown_dispatch_uses': len(re.findall(r'\bpsx_unknown_dispatch\s*\(', src)),
        'unsupported_todo_addrs': unsupported_todo_addrs,
        'unknown_bad': unknown_bad,
        'direct_outside': direct_outside,
        'missing_direct': missing_direct,
        'decl_without_def': decl_without_def,
        'called_decl_without_def': called_decl_without_def,
        'cba_classes': cba_classes,
    }
    return report


def print_generated_c_audit(load_addr: int, size: int, crc32: int,
                            report: dict) -> None:
    print(f'Overlay {load_addr:08X}_{crc32:08X}')
    print(f'Region: {load_addr:08X}..{load_addr + size - 1:08X}')
    print(f'Function definitions: {len(report["defs"])}')
    print(f'Forward declarations: {len(report["decls"])}')
    print(f'Direct calls inside overlay: {report["direct_inside"]}')
    print(f'External calls via call_by_address: {report["external_cba"]}')
    print(f'BIOS calls: {report["bios_calls"]}')
    print(f'Syscall uses: {report["syscall_uses"]}')
    print(f'Unsupported instruction TODOs: {len(report["unsupported_todo_addrs"])}')
    print(f'Unknown/bad targets: {len(report["unknown_bad"])}')
    for addr in sorted(report['unknown_bad']):
        print(f'  0x{addr:08X} UNKNOWN_BAD')
    for addr in sorted(report['unsupported_todo_addrs'])[:20]:
        print(f'  0x{addr:08X} UNSUPPORTED_INSTRUCTION')
    if len(report['unsupported_todo_addrs']) > 20:
        print(f'  ... {len(report["unsupported_todo_addrs"]) - 20} more unsupported instructions')


# ---------------------------------------------------------------------------
# Post-process generated C for DLL compilation
# ---------------------------------------------------------------------------

DISPATCH_PREAMBLE = """\
#include "overlay_api.h"

/* ---- Overlay dispatch shim (inserted by compile_overlays.py) ----------- */
static OverlayCallbacks g_cbs;

/* Call-contract state (ABI v2): generated code reads/bumps the runtime's
 * bail state through these pointers (cpu_state.h PSX_OVERLAY_DLL_BUILD
 * branch).  They start at local dummies so a pre-init call can't fault. */
static int      s_bail_dummy;
static uint64_t s_ctr_dummy0, s_ctr_dummy1;
int      *g_psx_call_bail_p    = &s_bail_dummy;
uint64_t *g_psx_bail_first_p   = &s_ctr_dummy0;
uint64_t *g_psx_bail_resolved_p = &s_ctr_dummy1;

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int overlay_abi(void) { return PSX_OVERLAY_ABI_VERSION | (PSX_OVERLAY_FLAVOR << 16); }

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void overlay_init(const OverlayCallbacks *cbs) {
    g_cbs = *cbs;
    if (g_cbs.call_bail_flag) g_psx_call_bail_p    = g_cbs.call_bail_flag;
    if (g_cbs.bail_first)     g_psx_bail_first_p   = g_cbs.bail_first;
    if (g_cbs.bail_resolved)  g_psx_bail_resolved_p = g_cbs.bail_resolved;
}

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra) {
    (void)ra;
    g_cbs.dispatch_call(cpu, addr, cpu->gpr[31]);
}
void psx_check_interrupts(CPUState *cpu) {
    g_cbs.check_interrupts(cpu);
}
void gte_execute(CPUState *cpu, uint32_t cmd) {
    g_cbs.gte_execute(cpu, cmd);
}
int psx_syscall(CPUState *cpu, uint32_t code) {
    return g_cbs.psx_syscall(cpu, code);
}
void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys) {
    g_cbs.psx_unknown_dispatch(cpu, addr, phys);
}
void debug_server_log_call_entry(uint32_t func_addr) {
    if (g_cbs.log_call_entry) g_cbs.log_call_entry(func_addr);
}
void psx_restore_state_escape(void) {
    if (g_cbs.psx_restore_state_escape) g_cbs.psx_restore_state_escape();
}
/* Widescreen hooks (ABI v3): forward to the runtime's live widescreen state.
 * Identity fallback if the host predates the callbacks (NULL) — so a v3 DLL
 * stays correct (4:3) on an older host. */
int psx_ws_backdrop_x(int x) {
    return g_cbs.ws_backdrop_x ? g_cbs.ws_backdrop_x(x) : (int)(short)x;
}
int psx_ws_x_margin(void) {
    return g_cbs.ws_x_margin ? g_cbs.ws_x_margin() : 0;
}
/* Shared render-funnel screen-X cull widening ([widescreen.cull] auto_screen_x).
 * The recompiler emits `rt = psx_ws_cull_sltiu(sx, imm)` at every width-compare
 * cull site, INCLUDING in overlay-resident code, so the overlay DLL must define
 * it or it fails to link (undefined reference) -> no native shard -> that overlay
 * variant runs interpreted forever (the dwarf-village-style lag). Self-contained:
 * depends only on psx_ws_x_margin() above. Identity at 4:3 (margin 0). MUST stay
 * byte-for-byte identical to gpu.c psx_ws_cull_sltiu. */
int psx_ws_cull_sltiu(uint32_t sx, uint32_t imm) {
    int m = psx_ws_x_margin();
    return ((uint32_t)((int32_t)(int16_t)(uint16_t)sx + m)
            < (uint32_t)((int32_t)imm + 2 * m)) ? 1 : 0;
}
void psx_ws_sprite_tag(CPUState *cpu) {
    if (g_cbs.ws_sprite_tag) g_cbs.ws_sprite_tag(cpu);
}
/* Widescreen backdrop column-preload value (ABI v4). Identity fallback if the
 * host predates the callback, so the DLL stays correct (4:3) on an older host. */
uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols) {
    return g_cbs.ws_backdrop_value ? g_cbs.ws_backdrop_value(orig, is_end, window_cols) : orig;
}
/* ----------------------------------------------------------------------- */

"""

def patch_generated_c(src: str, load_addr: int, size: int) -> str:
    """
    Post-process psxrecomp-game's _full.c output for standalone DLL compilation:

    1. Prepend the dispatch function-pointer preamble (before any includes).
    2. Remove forward declarations for functions outside the overlay range —
       they'd be unresolved externals in the DLL.
    3. Replace direct calls to out-of-range func_XXXXXXXX(cpu) with
       call_by_address(cpu, 0xXXXXXXXXu) so they go through the dispatch ptr.
    4. Export all func_XXXXXXXX symbols so the runtime can enumerate them.
    """
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_overlay(addr: int) -> bool:
        phys = addr & 0x1FFFFFFF
        return ov_lo <= phys < ov_hi

    # 1. Insert preamble AFTER the last #include line (so CPUState is complete)
    last_inc = -1
    for m in re.finditer(r'^#include\s+[<"].*[>"]\s*$', src, re.MULTILINE):
        last_inc = m.end()
    if last_inc == -1:
        src = DISPATCH_PREAMBLE + src
    else:
        src = src[:last_inc] + '\n' + DISPATCH_PREAMBLE + src[last_inc:]

    # 2. Remove out-of-range forward declarations
    def drop_extern(m):
        addr = int(m.group(1), 16)
        return '' if not in_overlay(addr) else m.group(0)
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\);\n',
                 drop_extern, src, flags=re.MULTILINE)

    # 3. Replace out-of-range direct calls with call_by_address
    def fix_call(m):
        addr = int(m.group(1), 16)
        if in_overlay(addr):
            return m.group(0)
        return f'call_by_address(cpu, 0x{addr:08X}u)'
    src = re.sub(r'\bfunc_([0-9A-Fa-f]{8})\(cpu\)',
                 fix_call, src)

    # 4. Add dllexport to every in-overlay func_XXXXXXXX definition.
    #    psxrecomp-game emits "void func_XXXXXXXX(CPUState* cpu)" on one line
    #    with "{" on the NEXT line — match the signature line alone (no ";" = definition).
    def add_export(m):
        addr = int(m.group(1), 16)
        if not in_overlay(addr):
            return m.group(0)
        return (
            '#ifdef _WIN32\n__declspec(dllexport)\n#else\n'
            '__attribute__((visibility("default")))\n#endif\n'
            + m.group(0)
        )
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)$',
                 add_export, src, flags=re.MULTILINE)

    return src


# ---------------------------------------------------------------------------
# Static (B-2) post-processing
# ---------------------------------------------------------------------------

STATIC_PREAMBLE = """\
/* ---- Static overlay (B-2): psx_runtime.h already provides call_by_address. */

"""

def patch_generated_c_static(src: str, load_addr: int, size: int) -> tuple:
    """
    Post-process psxrecomp-game's _full.c output for static binary compilation.

    Returns (patched_src, sorted_list_of_in_overlay_virt_addrs).

    Differences from DLL path:
    - No overlay_api.h / OverlayCallbacks / overlay_init
    - Callbacks are direct extern calls, not function pointers
    - No __declspec(dllexport) — all functions have normal external linkage
    - Returns function address list so caller can build the switch dispatch
    """
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_overlay(addr: int) -> bool:
        return ov_lo <= (addr & 0x1FFFFFFF) < ov_hi

    # 1. Insert preamble after last #include
    last_inc = -1
    for m in re.finditer(r'^#include\s+[<"].*[>"]\s*$', src, re.MULTILINE):
        last_inc = m.end()
    if last_inc == -1:
        src = STATIC_PREAMBLE + src
    else:
        src = src[:last_inc] + '\n' + STATIC_PREAMBLE + src[last_inc:]

    # 2. Remove out-of-range forward declarations
    def drop_extern(m):
        addr = int(m.group(1), 16)
        return '' if not in_overlay(addr) else m.group(0)
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\);\n',
                 drop_extern, src, flags=re.MULTILINE)

    # 3. Replace out-of-range direct calls with call_by_address
    def fix_call(m):
        addr = int(m.group(1), 16)
        if in_overlay(addr):
            return m.group(0)
        return f'call_by_address(cpu, 0x{addr:08X}u)'
    src = re.sub(r'\bfunc_([0-9A-Fa-f]{8})\(cpu\)', fix_call, src)

    # 4. Collect in-overlay function definition addresses (no export annotation needed)
    func_virt_addrs = []
    def collect_fn(m):
        addr = int(m.group(1), 16)
        if in_overlay(addr):
            func_virt_addrs.append(0x80000000 | (addr & 0x1FFFFFFF))
        return m.group(0)
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)$',
                 collect_fn, src, flags=re.MULTILINE)

    return src, sorted(func_virt_addrs)


def generate_overlay_dispatch(all_virt_addrs: list) -> str:
    """Generate psx_overlay_dispatch() covering all statically-compiled overlays."""
    lines = [
        '',
        '/* Auto-generated overlay dispatch — do not edit. */',
        'int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) {',
        '    switch (addr) {',
    ]
    for va in sorted(all_virt_addrs):
        lines.append(f'        case 0x{va:08X}u: func_{va:08X}(cpu); return 1;')
    lines += [
        '        default: return 0;',
        '    }',
        '}',
        '',
    ]
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# DLL compilation
# ---------------------------------------------------------------------------

def write_overlay_ranges(src_path: str, out_path: str,
                         data: bytes, load_addr: int, size: int) -> int:
    """Filter the recompiler's _full.ranges manifest to in-overlay functions,
    compute each function's AUTHORITATIVE code hash from the captured bytes (the
    exact bytes the recompiler compiled from), and write {phys}_{crc}.ranges
    beside the DLL. The loader marks a compiled entry callable iff live RAM
    matches this hash — making per-entry validity timing-independent and
    reload-on-return correct (design §8). Returns the number of functions written.

    Manifest v2 line format:
      F <entry_hex> <code_crc_hex>     one per function
      R <lo_hex> <len_hex>             one per coalesced code range

    binascii.crc32 (zlib, poly 0xEDB88320, init/final 0xFFFFFFFF) is bit-identical
    to the runtime's crc32_compute, and `data` is the raw little-endian RAM image,
    so the offline hash matches the runtime's hash of live RAM byte-for-byte."""
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_ov(a: int) -> bool:
        return ov_lo <= (a & 0x1FFFFFFF) < ov_hi

    # Parse the recompiler manifest into [(entry, [(lo, len), ...]), ...].
    funcs: list[tuple[int, list[tuple[int, int]]]] = []
    cur = None
    with open(src_path) as f:
        for line in f:
            s = line.split()
            if not s:
                continue
            if s[0] == 'F':
                try:
                    addr = int(s[1], 16)
                except (IndexError, ValueError):
                    cur = None
                    continue
                if in_ov(addr):
                    cur = (addr, [])
                    funcs.append(cur)
                else:
                    cur = None
            elif s[0] == 'R' and cur is not None:
                try:
                    lo, length = int(s[1], 16), int(s[2], 16)
                except (IndexError, ValueError):
                    continue
                cur[1].append((lo, length))

    out_lines = ['# psxrecomp overlay code-range manifest v2 (entry+code_crc)\n']
    n = 0
    for entry, ranges in funcs:
        if not ranges:
            continue
        crc = 0
        ok = True
        for lo, length in ranges:
            off = (lo & 0x1FFFFFFF) - ov_lo
            if off < 0 or off + length > len(data):
                ok = False  # range outside captured bytes — can't hash reliably
                break
            crc = binascii.crc32(data[off:off + length], crc)
        if not ok:
            continue
        ev = (entry & 0x1FFFFFFF) | 0x80000000
        out_lines.append(f'F {ev:08X} {crc & 0xFFFFFFFF:08X}\n')
        for lo, length in ranges:
            out_lines.append(f'R {(lo & 0x1FFFFFFF) | 0x80000000:08X} {length:X}\n')
        n += 1
    with open(out_path, 'w') as f:
        f.writelines(out_lines)
    return n


def _toolchain_env(gcc: str):
    """Build an environment that guarantees gcc's toolchain dir is on PATH.

    A mingw gcc invoked by absolute path (as the runtime's cmd.exe autocompile
    spawn does, and as any launch without mingw64/bin on PATH does) fails
    SILENTLY — exit 1, empty stderr — because cc1/as/ld/collect2 and the runtime
    DLLs (libwinpthread-1, libgcc_s_seh-1, libstdc++-6) are resolved via PATH and
    can't be found, so gcc never actually compiles. Auto-detect the toolchain
    bin dir and prepend it. Detection order: $PSX_MINGW_BIN override, the dir of
    the given --gcc path, `which gcc`, then common install locations. Returns
    (env, resolved_gcc_dir_or_None)."""
    import shutil
    env = os.environ.copy()
    cands = []
    if os.environ.get('PSX_MINGW_BIN'):
        cands.append(os.environ['PSX_MINGW_BIN'])
    if ('/' in gcc) or ('\\' in gcc):
        cands.append(os.path.dirname(os.path.abspath(gcc)))
    w = shutil.which(gcc)
    if w:
        cands.append(os.path.dirname(w))
    cands += [r'C:\msys64\mingw64\bin', r'C:\mingw64\bin', '/usr/bin']
    for d in cands:
        if d and (os.path.isfile(os.path.join(d, 'gcc.exe')) or
                  os.path.isfile(os.path.join(d, 'gcc'))):
            env['PATH'] = d + os.pathsep + env.get('PATH', '')
            return env, d
    return env, None


# ---- TinyCC (tcc) overlay compile — toolchain-free user fallback -----------
# tcc has its own built-in linker (no ld/collect2) and bundles its own headers,
# so it is self-contained. The only friction is that tcc 0.9.27 does not skip a
# UTF-8 BOM, and the recompiler's runtime headers carry one — so we feed tcc a
# BOM-stripped copy of the include dirs (used ONLY for tcc's -I; the codegen hash
# still derives from the real headers, so the cache dir matches the runtime).
_TCC_BOMFREE_INC = {}   # orig include dir -> bom-stripped temp dir (memoized)

def _bom_free_incdir(d: str) -> str:
    d = os.path.abspath(d)
    if d in _TCC_BOMFREE_INC:
        return _TCC_BOMFREE_INC[d]
    out = tempfile.mkdtemp(prefix='tcc_inc_')
    for name in os.listdir(d):
        if not name.endswith('.h'):
            continue
        with open(os.path.join(d, name), 'rb') as f:
            data = f.read()
        if data[:3] == b'\xef\xbb\xbf':
            data = data[3:]
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
    _TCC_BOMFREE_INC[d] = out
    return out

def _compile_dll_tcc(c_path: str, out_dll: str, include_dirs, flavor: int,
                     tcc: str) -> bool:
    # strip a UTF-8 BOM off the overlay C itself (tcc 0.9.27 chokes on it)
    with open(c_path, 'rb') as f:
        data = f.read()
    if data[:3] == b'\xef\xbb\xbf':
        with open(c_path, 'wb') as f:
            f.write(data[3:])
    cmd = [tcc, '-shared',
           '-DPSX_OVERLAY_DLL_BUILD',
           f'-DPSX_OVERLAY_FLAVOR={int(flavor)}',
           c_path, '-o', out_dll]
    for d in include_dirs:
        cmd.append('-I' + _bom_free_incdir(d))
    print(f'  compile (tcc): {" ".join(cmd)}')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f'  TCC COMPILE ERROR (exit {r.returncode}):\n{r.stderr or r.stdout}')
        return False
    return True


def compile_dll(c_path: str, out_dll: str, include_dirs: list[str],
                gcc: str = 'gcc', flavor: int = 0,
                compiler: str = 'gcc', tcc: str = 'tcc') -> bool:
    import platform
    # Absolute OS-native paths: gcc invoked from cmd.exe (the runtime's
    # autocompile spawn) silently fails (exit 1, no stderr) on relative
    # forward-slash paths; absolute backslash paths work in every context.
    c_path  = os.path.abspath(c_path)
    out_dll = os.path.abspath(out_dll)
    if compiler == 'tcc':
        return _compile_dll_tcc(c_path, out_dll, include_dirs, flavor, tcc)
    env, tc_dir = _toolchain_env(gcc)
    includes = [f'-I{os.path.abspath(d)}' for d in include_dirs]
    # On Windows, DLLs use PE relocations — -fPIC triggers GCC CRT init
    # that conflicts with the host process. Use -shared without -fPIC.
    pic_flag = [] if platform.system() == 'Windows' else ['-fPIC']
    cmd = [
        gcc, '-shared', *pic_flag, '-O2',
        '-DPSX_OVERLAY_DLL_BUILD',
        # Codegen-flavor tag baked into overlay_abi() (base=0). The loader
        # rejects DLLs whose flavor differs from the runtime's, so a widescreen
        # cache and a base cache can never cross-contaminate even if they share
        # a directory (they key by guest-bytes CRC, which is flavor-blind).
        f'-DPSX_OVERLAY_FLAVOR={int(flavor)}',
        c_path,
        '-o', out_dll,
        *includes,
        '-lm',
    ]
    if tc_dir is None:
        print('  WARNING: no gcc toolchain dir found (PSX_MINGW_BIN / --gcc dir / '
              'PATH / common locations); compile will likely fail silently.')
    print(f'  compile: {" ".join(cmd)}')
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        print(f'  COMPILE ERROR (exit {r.returncode}):\n{r.stderr or r.stdout}')
        return False
    return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--captures',        default=None,
                    help='overlay_captures.json from the runtime. Optional: the '
                         'runtime injects PSX_OVERLAY_CAPTURES (the canonical '
                         '<exe>/overlay_captures.json), which wins. Required only '
                         'for manual/offline invocation.')
    ap.add_argument('--game-toml',       required=True,
                    help='game.toml (for game_id)')
    ap.add_argument('--recompiler',      required=True,
                    help='path to psxrecomp-game.exe')
    ap.add_argument('--runtime-include', required=True,
                    help='path to psxrecomp runtime/include dir')
    ap.add_argument('--out-dir',         default='build-dev/cache',
                    help='cache root dir (default: build-dev/cache)')
    ap.add_argument('--gcc',             default='gcc',
                    help='GCC binary (default: gcc)')
    ap.add_argument('--compiler',        choices=['gcc', 'tcc'], default='gcc',
                    help='overlay shard compiler: gcc (default; dev/production, '
                         'best-optimized) or tcc (bundled, toolchain-free user '
                         'fallback). tcc shards land in the tcc/ cache namespace.')
    ap.add_argument('--tcc',             default='tcc',
                    help='TinyCC binary (used when --compiler tcc)')
    ap.add_argument('--force',           action='store_true',
                    help='recompile even if output already exists')
    ap.add_argument('--static',          action='store_true',
                    help='B-2 mode: compile into binary (overlays_static.c) instead of DLL')
    ap.add_argument('--flavor',          type=int, default=0,
                    help='codegen flavor id baked into overlay_abi() (0=base/master; '
                         'widescreen build passes 1). The loader rejects DLLs whose '
                         'flavor != the runtime flavor, keeping caches separate.')
    ap.add_argument('--cps',             action='store_true',
                    help='continuation-passing (RECURSION_BUG.md §25): set PSX_CPS '
                         'when invoking the recompiler so overlay funcs tail-transfer '
                         '+ carry an entry-switch. Must match the runtime build.')
    args = ap.parse_args()

    # ---- Framework-injected cache location wins over CLI flags ----------------
    # The runtime (autocompile.c) exports PSX_OVERLAY_CACHE_DIR / PSX_OVERLAY_CAPTURES
    # set to the loader's CANONICAL <exe>/cache and <exe>/overlay_captures.json. We
    # honor those above --out-dir/--captures so the WRITE cache and READ captures
    # are always exactly where the loader reads — no per-game config, no drift
    # between dev and prod (the read/write-location divergence bug). The CLI flags
    # remain the fallback for manual/offline invocation (no env set).
    _env_out = os.environ.get('PSX_OVERLAY_CACHE_DIR')
    if _env_out:
        if _env_out != args.out_dir:
            print(f'[cache] PSX_OVERLAY_CACHE_DIR overrides --out-dir: {_env_out}')
        args.out_dir = _env_out
    _env_cap = os.environ.get('PSX_OVERLAY_CAPTURES')
    if _env_cap:
        if _env_cap != args.captures:
            print(f'[cache] PSX_OVERLAY_CAPTURES overrides --captures: {_env_cap}')
        args.captures = _env_cap
    if not args.captures:
        ap.error('no captures file: set PSX_OVERLAY_CAPTURES (runtime injects it) '
                 'or pass --captures for manual/offline use')

    # Resolve the recompiler exe and runtime-include to absolute paths up front.
    # game.toml's overlay_autocompile_cmd uses paths RELATIVE to the project root
    # (the runtime spawns this script via cmd.exe with cwd = project root) so the
    # shipped config is portable across machines — never bake absolute paths into
    # game.toml. But two Windows quirks break relative paths once we hand them off:
    #   1. subprocess/CreateProcess does NOT resolve a relative *executable*
    #      against the child cwd (the recompiler spawn below, cwd=toml_dir), so a
    #      relative --recompiler fails with WinError 2 and the cache never warms.
    #   2. gcc launched from cmd.exe rejects relative forward-slash -I/-c/-o paths
    #      (already handled inside compile_dll via os.path.abspath).
    # Anchoring both here (against this process's cwd = project root, where the
    # relative paths are meant to resolve) makes relative config work everywhere.
    args.recompiler      = os.path.abspath(args.recompiler)
    args.runtime_include = os.path.abspath(args.runtime_include)

    # Read game ID from game.toml (strip BOM if present)
    with open(args.game_toml, 'rb') as f:
        raw = f.read().lstrip(b'\xef\xbb\xbf')  # UTF-8 BOM
    toml = tomllib.loads(raw.decode('utf-8'))
    game_id = toml.get('game', {}).get('id', 'UNKNOWN')
    print(f'Game ID: {game_id}')

    if args.static:
        static_out = os.path.join(args.out_dir, 'overlays_static.c')
        if os.path.exists(static_out) and not args.force:
            print(f'SKIP: {static_out} already exists (use --force to recompile)')
            return
        os.makedirs(args.out_dir, exist_ok=True)
    else:
        # Namespaced + versioned gcc cache: <game_id>/gcc/<arch-abi>/cg<N>/
        # (SLJIT.md §4 — no comingling; cg<N> = codegen version so a new emitter
        # build never reuses a stale DLL, old versions coexist). MUST match
        # overlay_loader.c scan_cache_dir(). Pre-1.0: no legacy fallback.
        cg = codegen_ver(args.runtime_include)
        ch = codegen_hash(args.runtime_include)
        cache_dir = os.path.join(args.out_dir, game_id, args.compiler, cache_arch_abi(),
                                 f'cg{cg}_{ch:08x}')
        os.makedirs(cache_dir, exist_ok=True)
        print(f'Cache dir: {cache_dir}  (codegen ver {cg}, hash {ch:08x})')

    with open(args.captures) as f:
        captures = json.load(f)

    print(f'Captures: {len(captures)} overlay(s) to process\n')

    # B-2 static mode: accumulate all overlay C into one file
    static_parts = []   # list of (patched_src, func_virt_addrs)

    for cap in captures:
        load_addr = int(cap['load_addr'], 16)
        size      = int(cap['size'])
        data      = base64.b64decode(cap['bytes_b64'])
        crc32     = binascii.crc32(data) & 0xFFFFFFFF
        phys_addr = (load_addr & 0x1FFFFFFF)

        # Merge evidence from a prior build of the SAME bytes: every F entry
        # in an existing ranges manifest for this exact image was proven
        # compilable before, so a fresh (poorer) capture can't regress
        # coverage on rebuild. Callable entries re-enter as captured function
        # entries (walk-root eligible); non-callable ones (alias wrappers
        # from a prior build) re-enter as dispatch entries so the classifier
        # re-derives their interior/alias disposition — feeding them back as
        # roots would truncate their hosts.
        if not args.static:
            ranges_name = f'{phys_addr:08X}_{crc32:08X}.ranges'
            prior_ranges = os.path.join(cache_dir, ranges_name)
            if os.path.exists(prior_ranges):
                prior_entries = []
                with open(prior_ranges) as pf:
                    for ln in pf:
                        parts = ln.split()
                        if parts and parts[0] == 'F':
                            try:
                                prior_entries.append(int(parts[1], 16))
                            except (IndexError, ValueError):
                                pass
                if prior_entries:
                    fe = _parse_addr_list(cap.get('function_entry_pcs', []))
                    de = _parse_addr_list(cap.get('dispatch_entry_pcs', []))
                    for a in prior_entries:
                        if _callable_legacy_seed(data, load_addr, a):
                            fe.add(a)
                        else:
                            de.add(a)
                    cap['function_entry_pcs'] = sorted(fe)
                    cap['dispatch_entry_pcs'] = sorted(de)
                    cap.setdefault('schema', 'merged')
                    print(f'  merged {len(prior_entries)} prior-manifest entries '
                          f'from {prior_ranges}')

        seeds, seed_audit = classify_overlay_seeds(cap, data, load_addr, size,
                                                   crc32, toml)

        if not args.static:
            dll_path = os.path.join(cache_dir, f'{phys_addr:08X}_{crc32:08X}.dll')

        print(f'Overlay  load=0x{load_addr:08X}  size={size}  crc32=0x{crc32:08X}')
        if args.static:
            print(f'  seeds: {len(seeds)}  mode: static -> {static_out}')
        else:
            print(f'  seeds: {len(seeds)}  dll: {dll_path}')
        print_seed_audit(seed_audit)

        root_seeds = [s for s in seeds if not s.startswith('interior')]
        if not root_seeds:
            print('  SKIP: no walk-root seeds (data-only region)\n')
            continue

        if not args.static and os.path.exists(dll_path) and not args.force:
            print('  SKIP: DLL already exists (use --force to recompile)\n')
            continue

        with tempfile.TemporaryDirectory() as tmp:
            # Write fake PS-EXE. The header entry PC becomes a walk root in the
            # recompiler, so it must be a walk-root seed — never an 'interior'
            # one. Root seed lines are either '0x...' or 'dispatch_root 0x...'.
            entry_pc = int(root_seeds[0].split()[-1], 16)
            psx_path = os.path.join(tmp, f'overlay_{load_addr:08X}.psx')
            with open(psx_path, 'wb') as f:
                f.write(make_psxexe(load_addr, entry_pc, data))

            # Write seeds file
            seeds_path = os.path.join(tmp, 'seeds.txt')
            with open(seeds_path, 'w') as f:
                for s in seeds:
                    f.write(s + '\n')

            out_dir_tmp = os.path.join(tmp, 'out')
            os.makedirs(out_dir_tmp)

            # Run psxrecomp-game in --overlay mode (always, for every overlay
            # input). Evidence-scoped discovery: compile only the proven entry
            # seeds and the code reachable from them; never whole-byte sweep
            # (which decodes embedded data tables as code). Branch/jump-table
            # targets stay as in-parent labels, not standalone functions. This
            # is the overlay-compilation contract, not a tunable.
            cmd = [args.recompiler, psx_path,
                   '--seeds', seeds_path,
                   '--out-dir', out_dir_tmp,
                   '--overlay',
                   # Forward the [widescreen] site lists so overlay-resident
                   # emits (backdrop screenX squash, and any sprite-tag/cull
                   # sites that resolve into overlay code) are applied. --ws-config
                   # only adopts the widescreen lists, not the game's exe/paths.
                   '--ws-config', os.path.abspath(args.game_toml)]
            print(f'  recompile: {args.recompiler} ...{" [CPS]" if args.cps else ""}')
            toml_dir = os.path.dirname(os.path.abspath(args.game_toml))
            sub_env = dict(os.environ)
            if args.cps:
                sub_env['PSX_CPS'] = '1'   # §25: emit continuation-passing overlay C
            r = subprocess.run(cmd, capture_output=True, text=True,
                               cwd=toml_dir, env=sub_env)
            if r.returncode != 0:
                print(f'  RECOMPILER ERROR:\n{r.stderr or r.stdout}')
                continue

            # Find the generated _full.c
            stem = os.path.basename(psx_path)
            full_c = os.path.join(out_dir_tmp, stem + '_full.c')
            if not os.path.exists(full_c):
                # psxrecomp-game uses filename() not stem()
                candidates = [f for f in os.listdir(out_dir_tmp) if f.endswith('_full.c')]
                if not candidates:
                    print(f'  ERROR: no _full.c in {out_dir_tmp}')
                    continue
                full_c = os.path.join(out_dir_tmp, candidates[0])

            with open(full_c) as f:
                src = f.read()

            # Post-process
            if args.static:
                src, func_addrs = patch_generated_c_static(src, load_addr, size)
                c_audit = audit_generated_c(src, load_addr, size, crc32, toml)
                print_generated_c_audit(load_addr, size, crc32, c_audit)
                if c_audit['unknown_bad'] or c_audit['unsupported_todo_addrs']:
                    print('  GENERATED-C AUDIT FAILED\n')
                    continue
                static_parts.append((src, func_addrs))
                print(f'  recompiled: {len(func_addrs)} functions\n')
            else:
                src = patch_generated_c(src, load_addr, size)
                c_audit = audit_generated_c(src, load_addr, size, crc32, toml)
                print_generated_c_audit(load_addr, size, crc32, c_audit)
                # Always save the debug copy for inspection — including on audit
                # failure, so opcode gaps / boundary artifacts can be classified.
                os.makedirs(os.path.dirname(dll_path), exist_ok=True)
                debug_c = os.path.join(os.path.dirname(dll_path),
                                       f'{crc32:08X}_patched.c')
                with open(debug_c, 'w') as f:
                    f.write(src)
                if c_audit['unknown_bad'] or c_audit['unsupported_todo_addrs']:
                    print('  GENERATED-C AUDIT FAILED\n')
                    continue
                patched_c = os.path.join(tmp, 'overlay_patched.c')
                with open(patched_c, 'w') as f:
                    f.write(src)

                # Compile to DLL
                include_dirs = [args.runtime_include]
                recomp_root = os.path.dirname(os.path.dirname(args.recompiler))
                for lib_inc in ['lib/fmt/include']:
                    p = os.path.join(recomp_root, lib_inc)
                    if os.path.isdir(p):
                        include_dirs.append(p)

                success = compile_dll(patched_c, dll_path, include_dirs,
                                      gcc=args.gcc, flavor=args.flavor,
                                      compiler=args.compiler, tcc=args.tcc)
                if success:
                    # Emit the per-entry code-range manifest beside the DLL.
                    # The loader keys it by the same filename stem with .ranges
                    # (replacing .dll). Without it the loader leaves the region
                    # to the interpreter (precision-first), so warn loudly.
                    ranges_src = None
                    for fn in os.listdir(out_dir_tmp):
                        if fn.endswith('_full.ranges'):
                            ranges_src = os.path.join(out_dir_tmp, fn)
                            break
                    ranges_out = dll_path[:-4] + '.ranges'
                    if ranges_src:
                        nfn = write_overlay_ranges(ranges_src, ranges_out,
                                                   data, load_addr, size)
                        print(f'  ranges: {nfn} functions -> {ranges_out}')
                    else:
                        print('  WARNING: recompiler emitted no _full.ranges — '
                              'loader will leave this region to the interpreter')
                    print(f'  OK -> {dll_path}\n')
                else:
                    print(f'  FAILED\n')

    # B-2: write combined static C file
    if args.static and static_parts:
        all_addrs = []
        combined = '/* Auto-generated overlay dispatch — do not edit.\n'
        combined += ' * Rebuild: python3 psxrecomp/tools/compile_overlays.py --static ...\n'
        combined += ' */\n'
        for src, addrs in static_parts:
            combined += src
            all_addrs.extend(addrs)
        combined += generate_overlay_dispatch(all_addrs)
        with open(static_out, 'w') as f:
            f.write(combined)
        print(f'Static output: {static_out}  ({len(all_addrs)} functions total)')

    print('Done.')


if __name__ == '__main__':
    main()
