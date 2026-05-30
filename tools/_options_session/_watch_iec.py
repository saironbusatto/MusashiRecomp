"""Sample SR / IRQ state to check if IEc ever goes back to 1."""
import time, sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

print(f"{'i':>3} {'frame':>6} {'sr':>10} {'IEc':>3} {'IEp':>3} {'IEo':>3} "
      f"{'i_stat':>10} {'i_mask':>10} {'pending':>10}")
for i in range(20):
    p = _dbg.call(4470, "ping", timeout=2)
    irq = _dbg.call(4470, "irq_state", timeout=2)
    sr_v = int(irq['cop0_sr'], 16)
    IEc = sr_v & 1
    IEp = (sr_v >> 2) & 1
    IEo = (sr_v >> 4) & 1
    print(f"{i:>3} {p['frame']:>6} {irq['cop0_sr']:>10} {IEc:>3} {IEp:>3} {IEo:>3} "
          f"{irq['i_stat']:>10} {irq['i_mask']:>10} {irq['pending']:>10}")
    time.sleep(0.5)
