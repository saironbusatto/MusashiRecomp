"""Query live runtime's recent activity rings to see what's looping."""
import sys, json, collections
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

# Try fntrace_dump first
try:
    fn = _dbg.call(4470, "fntrace_dump", count=512, timeout=4)
    if fn.get('ok') and fn.get('entries'):
        print(f"=== fntrace_dump ({len(fn['entries'])} entries, newest-first) ===")
        recent = fn['entries'][:60]
        # Hot targets
        targets = collections.Counter(e.get('target') for e in fn['entries'])
        print(f"Top 15 targets in last {len(fn['entries'])}:")
        for t, c in targets.most_common(15):
            print(f"  {t}: {c} hits")
        print()
        print("Last 15 entries (newest first):")
        for e in recent[:15]:
            print(f"  {e}")
    else:
        print(f"fntrace_dump empty / error: {fn}")
except Exception as e:
    print(f"fntrace_dump exception: {e}")

# Also try dirty_block_log
print()
try:
    db = _dbg.call(4470, "dirty_block_log", count=512, timeout=4)
    if db.get('ok') and db.get('entries'):
        print(f"=== dirty_block_log ({len(db['entries'])} entries, newest-first) ===")
        targets = collections.Counter(e.get('target') for e in db['entries'])
        print(f"Top 15 targets:")
        for t, c in targets.most_common(15):
            print(f"  {t}: {c} hits")
        print()
        print("Last 12 entries (newest-first):")
        for e in db['entries'][:12]:
            print(f"  {e}")
    else:
        print(f"dirty_block_log empty / error: {db}")
except Exception as e:
    print(f"dirty_block_log exception: {e}")
