"""Query live thread_trace and see if ChangeTh ping-pong is active now."""
import sys, json, collections
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

try:
    tt = _dbg.call(4470, "thread_trace", count=512, timeout=4)
    if not tt.get('ok'):
        print(f"err: {tt}")
        sys.exit(1)
    es = tt.get('entries', [])
    print(f"thread_trace: {len(es)} entries (total={tt.get('total')}, available={tt.get('available')})")
    if not es:
        print("(empty)")
        sys.exit(0)
    # Last 20
    print(f"Last 20 (newest first):")
    for e in es[:20]:
        print(f"  seq={e.get('seq')} fr={e.get('frame')} kind={e.get('kind')} name={e.get('name')} "
              f"cur={e.get('current_tcb')} tgt={e.get('target_tcb')} pc={e.get('target_pc')}")

    # Pairs of (cur_tcb, target_tcb)
    pairs = collections.Counter((e['current_tcb'], e['target_tcb'], e['name']) for e in es)
    print()
    print("Top transition signatures:")
    for (c,t,n), cnt in pairs.most_common(15):
        print(f"  {n}: {c} -> {t}: {cnt}")
except Exception as e:
    print(f"err: {e}")
