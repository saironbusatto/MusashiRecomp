"""Sample gpu_state every 500ms for 10 sec and see if display origin moves."""
import time, json, sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

prev = None
for i in range(20):
    g = _dbg.call(4470, "gpu_state", timeout=2)
    sig = (g['display_x'], g['display_y'], g['draw_area'][0], g['draw_area'][1],
           g['gp0_writes'], g['gp0_draw'], g['gp0_fill'])
    # Get frame from ping
    p = _dbg.call(4470, "ping", timeout=2)
    fr = p.get('frame', '?')
    sig_disp = f"disp=({g['display_x']},{g['display_y']}) draw_ofs=({g['draw_offset'][0]},{g['draw_offset'][1]}) draw_area=({g['draw_area'][0]},{g['draw_area'][1]}..{g['draw_area'][2]},{g['draw_area'][3]}) gpustat={g['gpustat']}"
    sig_cnt  = f"gp0_writes={g['gp0_writes']:,} gp0_draw={g['gp0_draw']} gp0_fill={g['gp0_fill']} gp0_copy={g['gp0_copy']} disabled={g['disabled']}"
    print(f"[{i:2}] fr={fr:>5}  {sig_disp}")
    print(f"           {sig_cnt}")
    if prev and prev != sig:
        diff = []
        for k in g:
            if k in prev_full and prev_full[k] != g[k]:
                diff.append(f"{k}: {prev_full[k]} -> {g[k]}")
        if diff:
            print(f"  CHANGED: {', '.join(diff[:6])}")
    prev = sig
    prev_full = g
    time.sleep(0.5)
