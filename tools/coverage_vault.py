#!/usr/bin/env python3
"""Additive coverage vault for overlay captures + compiled cache.

Overlay coverage — overlay_captures.json (raw disc-derived overlay bytes +
executed/entry PCs) and the gcc-compiled cache DLLs — normally lives ONLY in
gitignored build dirs, so a `cmake` clean or a fresh worktree wipes it, and it's
re-derivable only by replaying those game areas. This tool maintains a single,
safe, ADDITIVE vault OUTSIDE any build dir: every merge UNIONS new coverage in
and never drops what's already there.

  - captures: union by VARIANT (load_addr + hash of the captured bytes). Same
    variant => union its executed_pcs / dispatch_entry_pcs. Distinct variants
    (same address, different scene's overlay) are all kept.
  - cache: filenames are content-keyed (<addr>_<crc>.dll/.ranges), so a copy-if-
    absent (or newer) is a safe additive union.

It contains game-derived bytes, so the vault dir must stay gitignored / private
(same rule as overlay_captures.json). This script is pure tooling (no game data)
and is committed.

Usage:
  coverage_vault.py merge --vault DIR [--captures captures.json] [--cache CACHE_DIR]
  coverage_vault.py stats --vault DIR
"""
import argparse, json, os, shutil, hashlib, sys

CAP_NAME = "overlay_captures.json"
CACHE_SUB = "cache"

def _variant_key(region):
    b = region.get("bytes_b64", "") or ""
    return "%s:%s" % (region.get("load_addr"), hashlib.sha1(b.encode()).hexdigest())

def _load_list(path):
    if not os.path.exists(path):
        return []
    try:
        v = json.load(open(path, encoding="utf-8"))
        return v if isinstance(v, list) else []
    except Exception as e:
        print("  warn: could not read %s (%s); starting fresh" % (path, e))
        return []

def merge_captures(vault_json, src_json):
    if not src_json or not os.path.exists(src_json):
        return 0, 0
    src = _load_list(src_json)
    if not src:
        return 0, 0
    index = { _variant_key(r): r for r in _load_list(vault_json) }
    new_variants = new_pcs = 0
    for r in src:
        k = _variant_key(r)
        if k not in index:
            index[k] = dict(r)
            new_variants += 1
            new_pcs += len(r.get("executed_pcs", []))
        else:
            tgt = index[k]
            for fld in ("executed_pcs", "dispatch_entry_pcs"):
                cur = set(tgt.get(fld, []))
                add = set(r.get(fld, []))
                if fld == "executed_pcs":
                    new_pcs += len(add - cur)
                tgt[fld] = sorted(cur | add)
    os.makedirs(os.path.dirname(vault_json) or ".", exist_ok=True)
    json.dump(list(index.values()), open(vault_json, "w", encoding="utf-8"), indent=1)
    return new_variants, new_pcs

def merge_cache(vault_cache, src_cache):
    if not src_cache or not os.path.isdir(src_cache):
        return 0
    os.makedirs(vault_cache, exist_ok=True)
    added = 0
    for fn in os.listdir(src_cache):
        if not (fn.endswith(".dll") or fn.endswith(".ranges")):
            continue
        src = os.path.join(src_cache, fn)
        dst = os.path.join(vault_cache, fn)
        if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst) + 1:
            shutil.copy2(src, dst)
            if fn.endswith(".dll"):
                added += 1
    return added

def cmd_stats(vault):
    cj = os.path.join(vault, CAP_NAME)
    regs = _load_list(cj)
    pcs = sum(len(r.get("executed_pcs", [])) for r in regs)
    dlls = [f for f in os.listdir(os.path.join(vault, CACHE_SUB))] if os.path.isdir(os.path.join(vault, CACHE_SUB)) else []
    ndll = len([f for f in dlls if f.endswith(".dll")])
    print("vault: %s" % vault)
    print("  captures: %d variant(s), %d executed PC(s)" % (len(regs), pcs))
    print("  cache:    %d DLL(s)" % ndll)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["merge", "stats"])
    ap.add_argument("--vault", required=True, help="vault directory (kept gitignored/private)")
    ap.add_argument("--captures", help="source overlay_captures.json to merge in")
    ap.add_argument("--cache", help="source cache dir (e.g. build/cache/<game_id>) to merge in")
    a = ap.parse_args()
    if a.cmd == "stats":
        cmd_stats(a.vault)
        return 0
    vj = os.path.join(a.vault, CAP_NAME)
    vc = os.path.join(a.vault, CACHE_SUB)
    nv, np_ = merge_captures(vj, a.captures) if a.captures else (0, 0)
    nd = merge_cache(vc, a.cache) if a.cache else 0
    print("coverage_vault: +%d new variant(s), +%d new PC(s), +%d new DLL(s) -> %s" % (nv, np_, nd, a.vault))
    return 0

if __name__ == "__main__":
    sys.exit(main())
