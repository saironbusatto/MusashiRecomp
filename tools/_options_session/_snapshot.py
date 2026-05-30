"""Compact one-line snapshot of runtime state for diff'ing across nav events."""
import sys, json
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

label = sys.argv[1] if len(sys.argv) > 1 else "snap"

p   = _dbg.call(4470, "ping", timeout=2)
g   = _dbg.call(4470, "gpu_state", timeout=2)
s   = _dbg.call(4470, "sio_state", timeout=2)
irq = _dbg.call(4470, "irq_state", timeout=2)
mc  = _dbg.call(4470, "mc_status", timeout=2)

print(f"=== [{label}] fr={p['frame']} ===")
print(f"  GPU: draw={g['gp0_draw']} fill={g['gp0_fill']} env={g['gp0_env']} copy={g['gp0_copy']} writes={g['gp0_writes']:,} disabled={g['disabled']}")
print(f"       disp=({g['display_x']},{g['display_y']}) draw_area={g['draw_area']} gpustat={g['gpustat']}")
print(f"  SIO: mc_probes={s['mc_probes']} mc_reads={s['mc_reads']} mc_max_state={s['mc_max_state']} tx_writes={s['tx_writes']} sio_stat={s['sio_stat']} sio_ctrl={s['sio_ctrl']} card_caller={s['mc_last_caller']}")
print(f"  IRQ: i_stat={irq['i_stat']} i_mask={irq['i_mask']} pending={irq['pending']} cop0_sr={irq['cop0_sr']} IEc={irq['IEc']} IM2={irq['IM2']}")
print(f"  MC:  {mc}")
