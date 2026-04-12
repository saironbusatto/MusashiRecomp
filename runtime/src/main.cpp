/* main.cpp — Phase 2 runtime entry point.
 *
 * Loads BIOS ROM, initializes CPU state, calls into the recompiled
 * reset vector. Expects abort at first MMIO access.
 */

#include "cpu_state.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdlib>

/* memory.c */
extern "C" void memory_init(const char* bios_path);

/* timers.c */
extern "C" void timers_init(void);

/* interrupts.c */
extern "C" void interrupts_init(void);
extern "C" uint32_t psx_read_word(uint32_t addr);
extern "C" void     psx_write_word(uint32_t addr, uint32_t val);
extern "C" uint16_t psx_read_half(uint32_t addr);
extern "C" void     psx_write_half(uint32_t addr, uint16_t val);
extern "C" uint8_t  psx_read_byte(uint32_t addr);
extern "C" void     psx_write_byte(uint32_t addr, uint8_t val);

int main(int argc, char** argv) {
    /* Force line-buffered output so messages appear even if killed. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    const char* bios_path = "bios/SCPH1001.BIN";
    if (argc > 1) bios_path = argv[1];

    std::fprintf(stdout, "psxrecomp-v4 runtime: loading BIOS from %s\n", bios_path);
    memory_init(bios_path);
    timers_init();
    interrupts_init();

    /* Initialize CPU state. */
    CPUState cpu;
    std::memset(&cpu, 0, sizeof(cpu));

    /* Wire memory function pointers. */
    cpu.read_word  = psx_read_word;
    cpu.write_word = psx_write_word;
    cpu.read_half  = psx_read_half;
    cpu.write_half = psx_write_half;
    cpu.read_byte  = psx_read_byte;
    cpu.write_byte = psx_write_byte;

    /* R3000A reset state. */
    cpu.pc = 0xBFC00000u;
    cpu.cop0[12] = 0x00400000u; /* SR: BEV=1 (boot exception vectors) */

    /* Execute. */
    std::fprintf(stdout, "psxrecomp-v4 runtime: executing from PC=0x%08X\n", cpu.pc);
    psx_dispatch(&cpu, cpu.pc);

    /* If we reach here, all execution completed without MMIO abort. */
    std::fprintf(stdout, "psxrecomp-v4 runtime: execution completed, PC=0x%08X\n", cpu.pc);
    return 0;
}
