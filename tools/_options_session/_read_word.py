import sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg
addr = int(sys.argv[1], 16)
v = _dbg.read_word(4470, addr)
print(f"mem[0x{addr:08X}] = 0x{v:08X}")
