"""Scan Tomba's no-header EXE for PSY-Q-style BIOS thunks.

Pattern (MIPS LE, R3000A):
    addiu t2, zero, 0x{A0|B0|C0}    ; opcode 0x240A_00{A0|B0|C0}
    jr   t2                          ; 0x01400008
    addiu t1, zero, FN_NUM           ; 0x2409_NNNN  (FN is 16-bit imm)

Each thunk is 12 bytes; the tables tend to lay them out 16-byte aligned
with a trailing pad/nop, but we accept either stride.
"""
import struct, sys, os

BIOS_BASE = 0x80010000
PATH = sys.argv[1] if len(sys.argv) > 1 else 'F:/Projects/TombaRecomp/tomba/SCUS_942.36_no_header'

# Standard PSX BIOS function names. Source: docs/psx_bios_disasm.txt + nocash.
A0 = {
    0x00: "FileOpen", 0x01: "FileSeek", 0x02: "FileRead", 0x03: "FileWrite",
    0x04: "FileClose", 0x05: "FileIoctl", 0x06: "exit", 0x07: "FileGetDeviceFlag",
    0x08: "FileGetc", 0x09: "FilePutc", 0x0A: "todigit", 0x0B: "atof",
    0x0C: "strtoul", 0x0D: "strtol", 0x0E: "abs", 0x0F: "labs",
    0x10: "atoi", 0x11: "atol", 0x12: "atob", 0x13: "SaveState",
    0x14: "RestoreState", 0x15: "strcat", 0x16: "strncat", 0x17: "strcmp",
    0x18: "strncmp", 0x19: "strcpy", 0x1A: "strncpy", 0x1B: "strlen",
    0x1C: "index", 0x1D: "rindex", 0x1E: "strchr", 0x1F: "strrchr",
    0x20: "strpbrk", 0x21: "strspn", 0x22: "strcspn", 0x23: "strtok",
    0x24: "strstr", 0x25: "toupper", 0x26: "tolower", 0x27: "bcopy",
    0x28: "bzero", 0x29: "bcmp", 0x2A: "memcpy", 0x2B: "memset",
    0x2C: "memmove", 0x2D: "memcmp", 0x2E: "memchr", 0x2F: "rand",
    0x30: "srand", 0x31: "qsort", 0x32: "strtod", 0x33: "malloc",
    0x34: "free", 0x35: "lsearch", 0x36: "bsearch", 0x37: "calloc",
    0x38: "realloc", 0x39: "InitHeap", 0x3A: "_exit", 0x3B: "getchar",
    0x3C: "putchar", 0x3D: "gets", 0x3E: "puts", 0x3F: "printf",
    0x40: "SystemErrorUnresolvedException", 0x41: "LoadTest", 0x42: "Load", 0x43: "Exec",
    0x44: "FlushCache", 0x45: "InstallInterruptHandler", 0x46: "GPU_dw", 0x47: "mem2vram",
    0x48: "SendGP1Command", 0x49: "GPU_cw", 0x4A: "GPU_cwp", 0x4B: "send_image",
    0x4C: "InitCard", 0x4D: "StartCard", 0x4E: "StopCard", 0x4F: "_card_info_subfunc",
    0x50: "_card_write", 0x51: "_card_read", 0x52: "_new_card", 0x53: "Krom2RawAdd",
    0x54: "_unk_a54", 0x55: "Krom2Offset", 0x56: "GetLastError", 0x57: "GetLastFileError",
    0x58: "GetC0Table", 0x59: "GetB0Table", 0x5A: "_card_chan", 0x5B: "_unk_a5b",
    0x5C: "_unk_a5c", 0x5D: "ChangeClearPad", 0x5E: "_card_status", 0x5F: "_card_wait",
    0x70: "_bu_init", 0x71: "_96_init", 0x72: "_96_remove", 0x73: "_unk_a73",
    0x78: "_96_CdSeekL", 0x79: "_unk_a79", 0x7A: "_unk_a7a", 0x7B: "_unk_a7b",
    0x7C: "_96_CdGetStatus", 0x7D: "_unk_a7d", 0x7E: "_96_CdRead", 0x7F: "_unk_a7f",
    0xA0: "_patch_AdjustA0Table", 0xA1: "GetSystemInfo",
}
B0 = {
    0x00: "alloc_kernel_memory", 0x01: "free_kernel_memory", 0x02: "init_timer", 0x03: "get_timer",
    0x04: "enable_timer_irq", 0x05: "disable_timer_irq", 0x06: "restart_timer", 0x07: "DeliverEvent",
    0x08: "OpenEvent", 0x09: "CloseEvent", 0x0A: "WaitEvent", 0x0B: "EnableEvent",
    0x0C: "DisableEvent", 0x0D: "OpenTh", 0x0E: "TestEvent", 0x0F: "CloseTh",
    0x10: "ChangeTh", 0x11: "_unk_b11", 0x12: "InitPAD", 0x13: "StartPAD",
    0x14: "StopPAD", 0x15: "PAD_init", 0x16: "PAD_dr", 0x17: "ReturnFromException",
    0x18: "ResetEntryInt", 0x19: "HookEntryInt", 0x1A: "_unk_b1a", 0x1B: "_unk_b1b",
    0x1C: "_unk_b1c", 0x1D: "_unk_b1d", 0x1E: "_unk_b1e", 0x1F: "_unk_b1f",
    0x20: "UnDeliverEvent", 0x21: "_unk_b21", 0x22: "_unk_b22", 0x23: "_unk_b23",
    0x24: "_unk_b24", 0x25: "_unk_b25", 0x26: "_unk_b26", 0x27: "_unk_b27",
    0x28: "_unk_b28", 0x29: "_unk_b29", 0x2A: "_unk_b2a", 0x2B: "_unk_b2b",
    0x2C: "_unk_b2c", 0x2D: "_unk_b2d", 0x2E: "_unk_b2e", 0x2F: "_unk_b2f",
    0x30: "_unk_b30", 0x31: "_unk_b31", 0x32: "open", 0x33: "lseek",
    0x34: "read", 0x35: "write", 0x36: "close", 0x37: "ioctl",
    0x38: "exit", 0x39: "_unk_b39", 0x3A: "getc", 0x3B: "putc",
    0x3C: "getchar", 0x3D: "putchar", 0x3E: "gets", 0x3F: "puts",
    0x40: "cd", 0x41: "format", 0x42: "firstfile", 0x43: "nextfile",
    0x44: "rename", 0x45: "delete", 0x46: "undelete", 0x47: "AddDevice",
    0x48: "RemoveDevice", 0x49: "PrintInstalledDevices", 0x4A: "InitCARD", 0x4B: "StartCARD",
    0x4C: "StopCARD", 0x4D: "_card_info", 0x4E: "_card_load", 0x4F: "_card_auto",
    0x50: "bu_getfreemem", 0x51: "_unk_b51", 0x52: "set_card_auto_format", 0x53: "_unk_b53",
    0x54: "_unk_b54", 0x55: "_unk_b55", 0x56: "_unk_b56", 0x57: "_unk_b57",
    0x58: "_unk_b58", 0x59: "_unk_b59", 0x5A: "_unk_b5a", 0x5B: "ChangeClearPAD",
    0x5C: "_card_status", 0x5D: "_card_wait",
}
C0 = {
    0x00: "EnqueueTimerAndVblankIrqs", 0x01: "EnqueueSyscallHandler", 0x02: "SysEnqIntRP", 0x03: "SysDeqIntRP",
    0x04: "get_free_EvCB_slot", 0x05: "get_free_TCB_slot", 0x06: "ExceptionHandler", 0x07: "InstallExceptionHandlers",
    0x08: "SysInitMemory", 0x09: "SysInitKernelVariables", 0x0A: "ChangeClearRCnt", 0x0B: "_unk_c0b",
    0x0C: "InitDefInt", 0x0D: "_unk_c0d", 0x0E: "_unk_c0e", 0x0F: "_unk_c0f",
    0x10: "_unk_c10", 0x11: "_unk_c11", 0x12: "InstallDevices", 0x13: "FlushStdInOutPut",
    0x14: "_unk_c14", 0x15: "_cdevinput", 0x16: "_cdevscan", 0x17: "_circgetc",
    0x18: "_circputc", 0x19: "ioabort", 0x1A: "set_card_find_mode", 0x1B: "KernelRedirect",
    0x1C: "AdjustA0Table", 0x1D: "get_card_find_mode",
}

with open(PATH, 'rb') as f:
    blob = f.read()

# Pattern matches (fully aligned, full 12-byte sequence)
LI_T2_A0 = 0x240A00A0
LI_T2_B0 = 0x240A00B0
LI_T2_C0 = 0x240A00C0
JR_T2    = 0x01400008  # jalr zero, t2 == jr t2
ADDIU_T1 = 0x24090000  # addiu t1, zero, imm   — match top 16 bits

results = []  # (addr, vector_char, fn_id, fn_name)
for off in range(0, len(blob) - 12, 4):
    w0 = struct.unpack_from('<I', blob, off)[0]
    if w0 == LI_T2_A0:
        vec = 'A'; tab = A0
    elif w0 == LI_T2_B0:
        vec = 'B'; tab = B0
    elif w0 == LI_T2_C0:
        vec = 'C'; tab = C0
    else:
        continue
    w1 = struct.unpack_from('<I', blob, off + 4)[0]
    if w1 != JR_T2:
        continue
    w2 = struct.unpack_from('<I', blob, off + 8)[0]
    if (w2 & 0xFFFF0000) != ADDIU_T1:
        continue
    fn = w2 & 0xFFFF
    name = tab.get(fn, f"_unk_{vec.lower()}{fn:02x}")
    addr = BIOS_BASE + off
    results.append((addr, vec, fn, name))

# Per-vector summaries
by_vec = {'A': [], 'B': [], 'C': []}
for addr, vec, fn, name in results:
    by_vec[vec].append((addr, fn, name))

print(f"=== Tomba BIOS thunk inventory ({len(results)} thunks) ===\n")
for vec in ('A', 'B', 'C'):
    rows = by_vec[vec]
    seen = sorted(set((fn, name) for _addr, fn, name in rows))
    print(f"--- {vec}0 vector: {len(rows)} thunks, {len(seen)} unique functions ---")
    for fn, name in seen:
        addrs = [addr for addr, f, _ in rows if f == fn]
        print(f"  {vec}0:0x{fn:02X}  {name:30s}  @ {','.join(f'0x{a:08X}' for a in addrs)}")
    print()

# Also dump syscall instruction occurrences (EnterCriticalSection / ExitCriticalSection)
SYSCALL = 0x0000000C
syscalls = []
for off in range(0, len(blob) - 4, 4):
    w = struct.unpack_from('<I', blob, off)[0]
    if w == SYSCALL:
        # Look at preceding instruction for `li a0, N`
        if off >= 4:
            pre = struct.unpack_from('<I', blob, off - 4)[0]
            if (pre & 0xFFFF0000) == 0x24040000:  # addiu a0, zero, N
                a0 = pre & 0xFFFF
                kind = {1: "EnterCriticalSection", 2: "ExitCriticalSection",
                        3: "ChangeThreadSubFunction"}.get(a0, f"syscall_a0_{a0}")
                syscalls.append((BIOS_BASE + off - 4, a0, kind))

print(f"--- syscall instruction sites: {len(syscalls)} ---")
seen_sc = sorted(set((a0, kind) for _, a0, kind in syscalls))
for a0, kind in seen_sc:
    cnt = sum(1 for _, x, _ in syscalls if x == a0)
    print(f"  syscall a0=0x{a0:X}  {kind:30s}  ({cnt} sites)")

# Persist to seeds dir for later use by the audit
out_dir = 'F:/Projects/TombaRecomp/seeds'
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, 'tomba_bios_thunks.txt')
with open(out, 'w') as f:
    f.write(f"# Tomba PSX BIOS thunk inventory\n")
    f.write(f"# Source: F:/Projects/TombaRecomp/tomba/SCUS_942.36_no_header\n")
    f.write(f"# Pattern: li t2, 0x{{A0|B0|C0}}; jr t2; li t1, FN  (PSY-Q libapi/libetc style)\n")
    f.write(f"# Total thunks: {len(results)}\n\n")
    for vec in ('A', 'B', 'C'):
        seen = sorted(set((fn, name) for _addr, fn, name in by_vec[vec]))
        f.write(f"## {vec}0 vector ({len(by_vec[vec])} thunks, {len(seen)} unique)\n")
        for fn, name in seen:
            addrs = [addr for addr, x, _ in by_vec[vec] if x == fn]
            f.write(f"  {vec}0:0x{fn:02X}  {name:30s}  {','.join(f'0x{a:08X}' for a in addrs)}\n")
        f.write("\n")
    f.write(f"## syscall instruction sites ({len(syscalls)} total)\n")
    for a0, kind in seen_sc:
        cnt = sum(1 for _, x, _ in syscalls if x == a0)
        f.write(f"  syscall a0=0x{a0:X}  {kind:30s}  ({cnt} sites)\n")

print(f"\nWrote inventory: {out}")
