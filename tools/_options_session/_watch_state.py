"""Sample Tomba's state machine state every 500ms for 15 sec."""
import time, sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

prev = None
for i in range(30):
    ptr = _dbg.read_word(4470, 0x1F8001D4)
    state_word = _dbg.read_word(4470, ptr + 0x4C)  # halfword at +0x4E in upper16
    state = (state_word >> 16) & 0xFFFF
    # Also read the first 16 bytes of the struct (likely has fields)
    head = _dbg.read_word(4470, ptr + 0x00)
    flag2 = _dbg.read_word(4470, ptr + 0x44)
    p = _dbg.call(4470, "ping", timeout=2)
    sig = (ptr, state, head, flag2)
    star = " *CHANGED*" if prev and prev != sig else ""
    print(f"[{i:2}] fr={p.get('frame','?')} ptr=0x{ptr:08X} state=0x{state:04X} head=0x{head:08X} flag2=0x{flag2:08X}{star}")
    prev = sig
    time.sleep(0.5)
