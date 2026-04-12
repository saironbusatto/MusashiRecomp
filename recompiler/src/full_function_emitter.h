// full_function_emitter.h
// ----------------------------------------------------------------------------
// Phase 2: Full BIOS C emitter.
//
// Given a DiscoveryResult and the ROM image, emits:
//   - SCPH1001_full.c      (one C function per discovered BIOS function)
//   - SCPH1001_dispatch.c  (normalized-address -> function-pointer table)
//
// Design constraints:
//   - Does NOT modify strict_translator. Consumes TranslateResult metadata
//     but reconstructs terminator emission for multi-block control flow.
//   - Functions named func_{NORMALIZED:08X} (physical address after KSEG strip
//     and ROM-to-RAM alias resolution).
//   - Intra-function branches use goto + labels at basic block leaders.
//   - Inter-function calls (JAL) become direct C function calls.
//   - Indirect calls (JALR) and indirect jumps (JR non-$ra) go through
//     psx_dispatch().
//   - JR $ra becomes return.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "function_discovery.h"

namespace PSXRecompV4 {

struct EmitStats {
    uint32_t functions_emitted = 0;
    uint32_t functions_skipped = 0;  // e.g. FPU functions
    uint32_t total_instructions = 0;
    uint32_t dispatch_entries = 0;
    std::vector<std::pair<uint32_t, std::string>> skipped;  // (addr, reason)
};

class FullFunctionEmitter {
public:
    // Emit SCPH1001_full.c and SCPH1001_dispatch.c into out_dir.
    // rom: flat BIOS image bytes
    // base_addr: virtual address of rom[0] (0xBFC00000)
    // rom_end: last valid address inclusive (0xBFC7FFFF)
    // dr: discovery result from Phase 1c/2 discovery pass
    // bios_sha256: hex string for provenance header
    static EmitStats emit(
        const std::vector<uint8_t>& rom,
        uint32_t                    base_addr,
        uint32_t                    rom_end,
        const DiscoveryResult&      dr,
        const std::string&          bios_sha256,
        const std::string&          out_dir);

private:
    // Emit a single function's C code to the output stream.
    static bool emit_function(
        std::string&                out,
        const DiscoveredFunction&   func,
        const FunctionDiscovery::SingleFunctionResult& sfr,
        const std::set<uint32_t>&   all_function_entries,
        const std::vector<uint8_t>& rom,
        uint32_t                    base_addr,
        uint32_t                    rom_end);

    // Emit the dispatch table.
    static void emit_dispatch(
        std::string&              out,
        const DiscoveryResult&    dr,
        const std::set<uint32_t>& emitted_normalized,
        const std::string&        bios_sha256);

    static uint32_t normalize_address(uint32_t addr);
    static uint32_t read_u32_le(const std::vector<uint8_t>& rom, uint32_t offset);
};

} // namespace PSXRecompV4
