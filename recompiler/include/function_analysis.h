#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "ps1_exe_parser.h"

namespace PSXRecomp {

struct Function {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t size;
    bool has_prologue;
    bool has_epilogue;
    int32_t stack_frame_size; // Size of stack frame (if has prologue)
    std::string name; // Optional name (for now, just "func_<addr>")
    bool is_data_section = false; // True if this "function" is actually a data section
    // Overlapping-alias entry: nonzero means start_addr is an INTERIOR address of
    // the host function beginning at alias_walk_lo. The CFG covers the host's
    // full range [alias_walk_lo, end_addr) and the emitted C function enters via
    // a goto to start_addr's block. The host function is emitted unchanged —
    // aliases never cap or truncate it (the mid-function-seed hazard class).
    uint32_t alias_walk_lo = 0;
    // All alias entries sharing this host (including start_addr). Injected as
    // block leaders so every sibling alias gets an identical CFG, letting the
    // emitter generate ONE shared body per host with an entry switch instead
    // of duplicating the host's blocks per alias.
    std::vector<uint32_t> alias_group_entries;
};

struct FunctionAnalysisResult {
    std::vector<Function> functions;
    int total_instructions;
    int jr_ra_count;
    int prologue_count;
    int call_discovered_count = 0; // Functions found via JAL call-target following
    int strong_prologue_count = 0; // Functions found from prologues with saved $ra
    int state_continuation_count = 0; // Split entries after calls to SaveState-style helpers
};

class FunctionAnalyzer {
public:
    explicit FunctionAnalyzer(const PS1Executable& exe);

    // Scan entire executable for function boundaries
    FunctionAnalysisResult analyze();

    // Analyze only explicit entry points and direct JAL targets reachable from
    // them. This is for runtime-loaded overlays where scanning the whole blob
    // treats data and basic-block targets as standalone functions.
    FunctionAnalysisResult analyze_exact_entries(const std::vector<uint32_t>& entries);

    // Add a forced entry point address that is treated as a function start
    // even if it has no standard ADDIU $sp prologue. The function will be
    // included in the analysis result with has_prologue = false.
    void add_forced_entry(uint32_t addr);

    // Check if instruction is jr $ra (return)
    static bool is_jr_ra(uint32_t instr);

    // Check if instruction is function prologue (addiu $sp, $sp, -N)
    static bool is_prologue(uint32_t instr, int32_t& stack_size);

    // Check if instruction is epilogue (addiu $sp, $sp, +N)
    static bool is_epilogue(uint32_t instr, int32_t& stack_size);

    // Check if instruction is a branch or jump (has a delay slot)
    static bool is_branch_or_jump(uint32_t instr);

    // Check if a raw word decodes as a plausible R3000 instruction. Used to
    // validate scanned data pointers before promoting/aliasing them.
    static bool is_valid_mips_word(uint32_t instr);

private:
    const PS1Executable& exe_;

    // Forced entry points (from add_forced_entry())
    std::vector<uint32_t> forced_entry_points_;

    // Find function start by scanning backward from jr $ra
    uint32_t find_function_start(uint32_t return_addr);

    // Detect if a region is likely a data section masquerading as code
    bool is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const;
};

} // namespace PSXRecomp
