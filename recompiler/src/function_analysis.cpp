#include "function_analysis.h"
#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <fmt/format.h>

namespace PSXRecomp {

FunctionAnalyzer::FunctionAnalyzer(const PS1Executable& exe) : exe_(exe) {}

void FunctionAnalyzer::add_forced_entry(uint32_t addr) {
    // Validate address is within the EXE range
    if (addr >= exe_.header.load_address && addr < exe_.end_address()) {
        forced_entry_points_.push_back(addr);
    }
}

bool FunctionAnalyzer::is_jr_ra(uint32_t instr) {
    // jr $ra: opcode=0, rs=31 ($ra), rt=0, rd=0, shamt=0, funct=8 (jr)
    // Format: 000000 11111 00000 00000 00000 001000
    // Hex: 0x03E00008
    return instr == 0x03E00008;
}

bool FunctionAnalyzer::is_prologue(uint32_t instr, int32_t& stack_size) {
    // addiu $sp, $sp, -N
    // Format: 001001 11101 11101 <16-bit signed immediate>
    // Opcode: 0x27 (addiu), rs=$sp (29), rt=$sp (29)
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm < 0) {
        stack_size = -imm; // Store positive stack frame size
        return true;
    }
    return false;
}

bool FunctionAnalyzer::is_epilogue(uint32_t instr, int32_t& stack_size) {
    // addiu $sp, $sp, +N
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm > 0) {
        stack_size = imm;
        return true;
    }
    return false;
}

bool FunctionAnalyzer::is_valid_mips_word(uint32_t instr) {
    if (instr == 0xFFFFFFFFu || instr == 0xFFFFFFFDu) return false;

    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t funct = instr & 0x3Fu;
    uint32_t rt = (instr >> 16) & 0x1Fu;

    if (opcode == 0x00u) {
        switch (funct) {
        case 0x00u: case 0x02u: case 0x03u: case 0x04u:
        case 0x06u: case 0x07u: case 0x08u: case 0x09u:
        case 0x0Cu: case 0x0Du:
        case 0x10u: case 0x11u: case 0x12u: case 0x13u:
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
        case 0x20u: case 0x21u: case 0x22u: case 0x23u:
        case 0x24u: case 0x25u: case 0x26u: case 0x27u:
        case 0x2Au: case 0x2Bu:
            return true;
        default:
            return false;
        }
    }
    if (opcode == 0x01u) {
        return rt == 0x00u || rt == 0x01u || rt == 0x10u || rt == 0x11u;
    }

    switch (opcode) {
    case 0x02u: case 0x03u: case 0x04u: case 0x05u:
    case 0x06u: case 0x07u:
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
    case 0x0Cu: case 0x0Du: case 0x0Eu: case 0x0Fu:
    case 0x10u: case 0x12u:
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
    case 0x24u: case 0x25u: case 0x26u:
    case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu:
    case 0x2Eu:
    case 0x30u: case 0x32u: case 0x38u: case 0x3Au:
        return true;
    default:
        return false;
    }
}

bool FunctionAnalyzer::is_branch_or_jump(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    // J, JAL
    if (opcode == 0x02 || opcode == 0x03) return true;
    // BEQ, BNE, BLEZ, BGTZ
    if (opcode >= 0x04 && opcode <= 0x07) return true;
    // REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL
    if (opcode == 0x01) return true;
    // SPECIAL: JR, JALR
    if (opcode == 0x00) {
        uint32_t funct = instr & 0x3F;
        if (funct == 0x08 || funct == 0x09) return true;
    }
    // COP1/COP2 branches (BC1F, BC1T, BC2F, BC2T) — opcode 0x11 or 0x12, rs=0x08
    if ((opcode == 0x11 || opcode == 0x12) && ((instr >> 21) & 0x1F) == 0x08) return true;
    return false;
}

static bool is_sw_reg_base(uint32_t instr, uint32_t base_reg, uint32_t value_reg) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    return opcode == 0x2B && rs == base_reg && rt == value_reg;
}

static bool is_sw_reg_sp(uint32_t instr, uint32_t value_reg) {
    return is_sw_reg_base(instr, 29, value_reg);
}

static bool is_lui(uint32_t instr, uint32_t& rt_out) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    if (opcode != 0x0F) return false;
    rt_out = (instr >> 16) & 0x1F;
    return true;
}

static bool is_lw_same_reg_base(uint32_t instr, uint32_t reg) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    return opcode == 0x23 && rs == reg && rt == reg;
}

uint32_t FunctionAnalyzer::find_function_start(uint32_t return_addr) {
    // Scan backward from jr $ra to find function start
    // Heuristic: Look for prologue or function alignment (16-byte boundary after prev function)

    uint32_t search_addr = return_addr;
    const uint32_t max_search = 4096; // Search up to 4096 instructions backward (16 KB)

    for (uint32_t i = 0; i < max_search; i++) {
        search_addr -= 4;

        if (search_addr < exe_.header.load_address) {
            // Reached beginning of code
            return exe_.header.load_address;
        }

        auto word_opt = exe_.read_word(search_addr);
        if (!word_opt.has_value()) {
            return return_addr; // Can't read, assume current position
        }

        uint32_t instr = *word_opt;
        int32_t stack_size;

        // Check if this is a prologue
        if (is_prologue(instr, stack_size)) {
            // Verify this isn't a delay slot of a branch/jump instruction.
            // MIPS compilers often place stack allocation in the delay slot of
            // the function's first conditional branch, e.g.:
            //   beq v0, zero, skip
            //   addiu sp, sp, -N   <- delay slot, looks like prologue but isn't a function start
            // If the preceding instruction is a branch/jump, skip this candidate.
            if (search_addr >= exe_.header.load_address + 4) {
                auto prev_opt = exe_.read_word(search_addr - 4);
                if (prev_opt.has_value() && is_branch_or_jump(*prev_opt)) {
                    continue;  // delay slot, not a real prologue — keep scanning
                }
            }
            if (search_addr >= exe_.header.load_address + 8) {
                auto prev2_opt = exe_.read_word(search_addr - 8);
                auto prev1_opt = exe_.read_word(search_addr - 4);
                if (prev2_opt.has_value() && prev1_opt.has_value()) {
                    uint32_t lui_rt = 0;
                    if (is_lui(*prev2_opt, lui_rt) &&
                        is_lw_same_reg_base(*prev1_opt, lui_rt)) {
                        return search_addr - 8;
                    }
                }
            }
            return search_addr;
        }

        // Check if we hit another function's return
        if (is_jr_ra(instr)) {
            // We've gone too far backward and hit another function
            // Return the address after this jr $ra (+ 8 for delay slot and alignment)
            return search_addr + 8;
        }
    }

    // Couldn't find clear start, assume max search distance
    return return_addr - (max_search * 4);
}

bool FunctionAnalyzer::is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const {
    uint32_t size = end_addr - start_addr;
    if (size < 100) return false;  // Minimum check size

    uint32_t total_words = size / 4;
    uint32_t invalid_jal_count = 0;
    uint32_t undefined_opcode_count = 0;

    // Valid PS1 (MIPS R3000) primary opcodes
    static const bool valid_opcode[64] = {
        true,  true,  true,  true,  true,  true,  true,  true,   // 0x00-0x07
        true,  true,  true,  true,  true,  true,  true,  true,   // 0x08-0x0F
        true,  false, true,  false, false, false, false, false,   // 0x10-0x17 (COP0=0x10, COP2=0x12)
        false, false, false, false, false, false, false, false,   // 0x18-0x1F
        true,  true,  true,  true,  true,  true,  true,  false,  // 0x20-0x27
        true,  true,  true,  true,  false, false, true,  false,  // 0x28-0x2F
        true,  false, true,  false, false, false, false, false,  // 0x30-0x37 (LWC0=0x30, LWC2=0x32)
        true,  false, true,  false, false, false, false, false,  // 0x38-0x3F (SWC0=0x38, SWC2=0x3A)
    };

    for (uint32_t addr = start_addr; addr < end_addr; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;

        uint32_t opcode = (instr >> 26) & 0x3F;

        // Check for JAL with invalid target
        if (opcode == 3) {  // JAL opcode
            // PS1 JAL target: upper 4 bits from PC region (0x80000000), low 28 bits from instr
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;
            if (target > 0x801FFFFFu) {
                invalid_jal_count++;
            }
        }

        // Check for undefined opcode
        if (!valid_opcode[opcode]) {
            undefined_opcode_count++;
        }
    }

    // Size-graduated JAL threshold: higher ratio needed for smaller functions
    uint32_t jal_ratio_x100 = (total_words > 0) ? (invalid_jal_count * 100u / total_words) : 0u;
    if (size >= 10000 && jal_ratio_x100 > 5u)  return true;  // Large: >5% invalid JALs
    if (size >= 1000  && jal_ratio_x100 > 30u) return true;  // Medium: >30% invalid JALs
    if (size >= 100   && jal_ratio_x100 > 60u) return true;  // Small: >60% invalid JALs

    // Undefined opcode check:
    // Real PS1 code consistently uses 0% undefined opcodes (calibrated on Tomba!).
    // Data sections masquerading as functions have ~9-27% undefined opcodes.
    // A safe threshold of 7% catches all observed data sections with no false positives.
    uint32_t undef_ratio_x100 = (total_words > 0) ? (undefined_opcode_count * 100u / total_words) : 0u;
    if (size >= 1000 && undef_ratio_x100 > 7u) return true;   // Large: >7% undefined opcodes
    if (size >= 400  && undef_ratio_x100 > 50u) return true;  // Small: >50% undefined opcodes (conservative)

    return false;
}

namespace {

enum class ExactCfKind {
    Normal,
    Branch,
    Jump,
    Jal,
    JrRa,
    JrOther,
    Jalr,
};

struct ExactCf {
    ExactCfKind kind = ExactCfKind::Normal;
    uint32_t target = 0;
};

struct ExactWalkResult {
    std::set<uint32_t> visited;
    std::set<uint32_t> direct_jal_targets;
    std::set<uint32_t> jump_table_targets;
    uint32_t jr_ra_count = 0;
};

static bool exact_is_jr_ra_word(uint32_t instr) {
    return instr == 0x03E00008u;
}

static bool exact_is_addiu_sp_neg(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t rs = (instr >> 21) & 0x1Fu;
    uint32_t rt = (instr >> 16) & 0x1Fu;
    int16_t imm = static_cast<int16_t>(instr & 0xFFFFu);
    return opcode == 0x09u && rs == 29u && rt == 29u && imm < 0;
}

static bool exact_is_valid_mips_word(uint32_t instr) {
    return FunctionAnalyzer::is_valid_mips_word(instr);
}

static uint32_t exact_branch_target(uint32_t pc, uint32_t instr) {
    int16_t imm = static_cast<int16_t>(instr & 0xFFFFu);
    return pc + 4u + (static_cast<int32_t>(imm) << 2);
}

static uint32_t exact_jump_target(uint32_t pc, uint32_t instr) {
    return ((pc + 4u) & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
}

static ExactCf exact_classify_cf(uint32_t pc, uint32_t instr) {
    ExactCf cf;
    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t funct = instr & 0x3Fu;
    uint32_t rs = (instr >> 21) & 0x1Fu;

    if (opcode == 0x00u && funct == 0x08u) {
        cf.kind = (rs == 31u) ? ExactCfKind::JrRa : ExactCfKind::JrOther;
        return cf;
    }
    if (opcode == 0x00u && funct == 0x09u) {
        cf.kind = ExactCfKind::Jalr;
        return cf;
    }
    if (opcode == 0x02u) {
        cf.kind = ExactCfKind::Jump;
        cf.target = exact_jump_target(pc, instr);
        return cf;
    }
    if (opcode == 0x03u) {
        cf.kind = ExactCfKind::Jal;
        cf.target = exact_jump_target(pc, instr);
        return cf;
    }
    if (opcode == 0x01u || (opcode >= 0x04u && opcode <= 0x07u) ||
        (opcode >= 0x14u && opcode <= 0x17u)) {
        cf.kind = ExactCfKind::Branch;
        cf.target = exact_branch_target(pc, instr);
        return cf;
    }
    return cf;
}

} // namespace

FunctionAnalysisResult FunctionAnalyzer::analyze_exact_entries(const std::vector<uint32_t>& entries) {
    FunctionAnalysisResult result;
    result.total_instructions = 0;
    result.jr_ra_count = 0;
    result.prologue_count = 0;
    result.call_discovered_count = 0;
    result.state_continuation_count = 0;

    fmt::print("\n=== Exact Overlay Function Analysis ===\n\n");

    auto in_exe = [&](uint32_t addr) {
        return addr >= exe_.header.load_address && addr < exe_.end_address() && (addr & 3u) == 0;
    };

    auto callable_direct_jal_target = [&](uint32_t addr) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value() || !exact_is_valid_mips_word(*word_opt)) return false;
        if (addr == exe_.header.load_address) return true;

        if (addr >= exe_.header.load_address + 8u) {
            auto prev8_opt = exe_.read_word(addr - 8u);
            if (prev8_opt.has_value() && exact_is_jr_ra_word(*prev8_opt)) return true;
        }

        if (exact_is_addiu_sp_neg(*word_opt)) {
            if (addr < exe_.header.load_address + 4u) return true;
            auto prev_opt = exe_.read_word(addr - 4u);
            if (!prev_opt.has_value()) return true;
            return exact_classify_cf(addr - 4u, *prev_opt).kind == ExactCfKind::Normal;
        }

        return false;
    };

    auto find_jump_table_targets = [&](uint32_t entry, uint32_t hard_cap,
                                       uint32_t jr_pc, uint32_t jr_rs) {
        std::vector<uint32_t> targets;
        uint32_t lw_base = 0xFFu;
        int32_t lw_offset = 0;
        uint32_t addu_cand[2] = {0xFFu, 0xFFu};
        uint32_t lui_val = 0;
        int16_t addiu_val[2] = {0, 0};
        bool found_addiu[2] = {false, false};
        bool found_lui = false;
        uint32_t table_count = 0;

        for (int back = 1; back <= 40; back++) {
            uint32_t scan_addr = jr_pc - static_cast<uint32_t>(back * 4);
            if (scan_addr < entry) break;
            auto scan_opt = exe_.read_word(scan_addr);
            if (!scan_opt.has_value()) break;

            uint32_t instr = *scan_opt;
            uint32_t op = (instr >> 26) & 0x3Fu;
            uint32_t rs = (instr >> 21) & 0x1Fu;
            uint32_t rt = (instr >> 16) & 0x1Fu;
            uint32_t rd = (instr >> 11) & 0x1Fu;
            uint32_t fn = instr & 0x3Fu;

            if (op == 0x23u && rt == jr_rs && lw_base == 0xFFu) {
                lw_base = rs;
                lw_offset = static_cast<int32_t>(static_cast<int16_t>(instr & 0xFFFFu));
                continue;
            }
            if (op == 0x00u && fn == 0x21u && rd == lw_base && lw_base != 0xFFu &&
                addu_cand[0] == 0xFFu) {
                addu_cand[0] = rs;
                addu_cand[1] = rt;
                continue;
            }
            if (op == 0x09u && addu_cand[0] != 0xFFu) {
                for (int c = 0; c < 2; c++) {
                    if (!found_addiu[c] && addu_cand[c] != 0xFFu &&
                        rs == addu_cand[c] && rt == addu_cand[c]) {
                        addiu_val[c] = static_cast<int16_t>(instr & 0xFFFFu);
                        found_addiu[c] = true;
                        break;
                    }
                }
                continue;
            }
            if (op == 0x0Fu && addu_cand[0] != 0xFFu && !found_lui) {
                for (int c = 0; c < 2; c++) {
                    if (addu_cand[c] != 0xFFu && rt == addu_cand[c]) {
                        lui_val = (instr & 0xFFFFu) << 16;
                        if (!found_addiu[c]) addiu_val[c] = 0;
                        addiu_val[0] = addiu_val[c];
                        found_addiu[0] = found_addiu[c];
                        found_lui = true;
                        break;
                    }
                }
                continue;
            }
            if (op == 0x0Bu && table_count == 0) {
                table_count = instr & 0xFFFFu;
                continue;
            }
            if (found_lui && table_count != 0) break;
        }

        if (!found_lui || table_count == 0 || table_count >= 512) return targets;

        uint32_t table_base = lui_val +
            (found_addiu[0] ? static_cast<uint32_t>(static_cast<int32_t>(addiu_val[0])) : 0u) +
            static_cast<uint32_t>(lw_offset);
        for (uint32_t i = 0; i < table_count; i++) {
            auto entry_opt = exe_.read_word(table_base + i * 4u);
            if (!entry_opt.has_value()) continue;
            uint32_t target = *entry_opt;
            if (target >= entry && target < hard_cap && in_exe(target)) {
                targets.push_back(target);
            }
        }
        return targets;
    };

    auto walk = [&](uint32_t entry, uint32_t hard_cap) {
        ExactWalkResult wr;
        std::queue<uint32_t> work;
        work.push(entry);

        auto in_function = [&](uint32_t addr) {
            return in_exe(addr) && addr >= entry && addr < hard_cap;
        };

        while (!work.empty()) {
            uint32_t pc = work.front();
            work.pop();

            if (wr.visited.count(pc) || !in_function(pc)) continue;
            auto word_opt = exe_.read_word(pc);
            if (!word_opt.has_value()) continue;

            uint32_t instr = *word_opt;
            wr.visited.insert(pc);
            ExactCf cf = exact_classify_cf(pc, instr);
            uint32_t delay = pc + 4u;

            switch (cf.kind) {
            case ExactCfKind::Normal:
                if (in_function(pc + 4u)) work.push(pc + 4u);
                break;
            case ExactCfKind::Branch:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                if (in_function(cf.target)) work.push(cf.target);
                break;
            case ExactCfKind::Jump:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(cf.target)) work.push(cf.target);
                break;
            case ExactCfKind::Jal:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                if (in_exe(cf.target) && callable_direct_jal_target(cf.target)) {
                    wr.direct_jal_targets.insert(cf.target);
                }
                break;
            case ExactCfKind::Jalr:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                break;
            case ExactCfKind::JrRa:
                if (in_function(delay)) wr.visited.insert(delay);
                wr.jr_ra_count++;
                break;
            case ExactCfKind::JrOther:
                if (in_function(delay)) wr.visited.insert(delay);
                {
                    uint32_t jr_rs = (instr >> 21) & 0x1Fu;
                    for (uint32_t target : find_jump_table_targets(entry, hard_cap, pc, jr_rs)) {
                        wr.jump_table_targets.insert(target);
                        if (in_function(target)) work.push(target);
                    }
                }
                break;
            }
        }
        return wr;
    };

    std::set<uint32_t> known_entries;
    std::queue<uint32_t> pending;
    for (uint32_t entry : entries) {
        if (!in_exe(entry)) continue;
        if (known_entries.insert(entry).second) pending.push(entry);
    }

    size_t explicit_count = known_entries.size();
    fmt::print("Explicit entries: {}\n", explicit_count);

    std::set<uint32_t> processed;
    while (!pending.empty()) {
        uint32_t entry = pending.front();
        pending.pop();
        if (processed.count(entry)) continue;
        processed.insert(entry);

        auto next_it = known_entries.upper_bound(entry);
        uint32_t hard_cap = (next_it != known_entries.end()) ? *next_it : exe_.end_address();
        ExactWalkResult wr = walk(entry, hard_cap);

        for (uint32_t target : wr.direct_jal_targets) {
            if (known_entries.insert(target).second) pending.push(target);
        }
    }

    result.call_discovered_count = static_cast<int>(known_entries.size() - explicit_count);
    fmt::print("Direct-JAL entries: {}\n", result.call_discovered_count);
    fmt::print("Total exact entries: {}\n\n", known_entries.size());

    std::vector<uint32_t> starts_vec(known_entries.begin(), known_entries.end());
    std::sort(starts_vec.begin(), starts_vec.end());

    for (size_t i = 0; i < starts_vec.size(); i++) {
        uint32_t entry = starts_vec[i];
        uint32_t hard_cap = (i + 1 < starts_vec.size()) ? starts_vec[i + 1] : exe_.end_address();
        ExactWalkResult wr = walk(entry, hard_cap);
        if (wr.visited.empty()) continue;

        Function func;
        func.start_addr = entry;
        func.end_addr = *wr.visited.rbegin() + 4u;
        if (func.end_addr > hard_cap) func.end_addr = hard_cap;
        if (func.end_addr <= func.start_addr) func.end_addr = func.start_addr + 4u;
        func.size = func.end_addr - func.start_addr;
        func.name = fmt::format("func_{:08X}", func.start_addr);

        auto first_instr = exe_.read_word(func.start_addr);
        if (first_instr.has_value()) {
            int32_t stack_size;
            func.has_prologue = is_prologue(*first_instr, stack_size);
            func.stack_frame_size = func.has_prologue ? stack_size : 0;
            if (func.has_prologue) result.prologue_count++;
        } else {
            func.has_prologue = false;
            func.stack_frame_size = 0;
        }

        func.has_epilogue = wr.jr_ra_count > 0;
        func.is_data_section = false;

        result.jr_ra_count += static_cast<int>(wr.jr_ra_count);
        result.total_instructions += static_cast<int>(wr.visited.size());
        result.functions.push_back(func);
    }

    return result;
}


FunctionAnalysisResult FunctionAnalyzer::analyze() {
    FunctionAnalysisResult result;
    result.total_instructions = 0;
    result.jr_ra_count = 0;
    result.prologue_count = 0;
    result.call_discovered_count = 0;
    result.strong_prologue_count = 0;
    result.state_continuation_count = 0;

    fmt::print("\n=== Function Boundary Detection ===\n\n");

    // Pass 1: Find all jr $ra instructions
    std::vector<uint32_t> return_addresses;

    uint32_t current_addr = exe_.header.load_address;
    uint32_t end_addr = exe_.end_address();

    fmt::print("Scanning {} KB of code for function returns...\n",
               (end_addr - current_addr) / 1024);

    while (current_addr < end_addr) {
        auto word_opt = exe_.read_word(current_addr);
        if (!word_opt.has_value()) {
            break;
        }

        uint32_t instr = *word_opt;
        result.total_instructions++;

        if (is_jr_ra(instr)) {
            return_addresses.push_back(current_addr);
            result.jr_ra_count++;
        }

        int32_t stack_size;
        if (is_prologue(instr, stack_size)) {
            result.prologue_count++;
        }

        current_addr += 4;
    }

    fmt::print("✓ Found {} jr $ra instructions\n", result.jr_ra_count);
    fmt::print("✓ Found {} function prologues\n", result.prologue_count);
    fmt::print("✓ Scanned {} total instructions\n\n", result.total_instructions);

    // Pass 2: For each jr $ra, find function boundaries
    fmt::print("Analyzing function boundaries...\n");

    std::set<uint32_t> function_starts; // Use set to avoid duplicates
    std::map<uint32_t, uint32_t> function_last_return;

    for (uint32_t return_addr : return_addresses) {
        uint32_t func_start = find_function_start(return_addr);
        function_starts.insert(func_start);
        auto it = function_last_return.find(func_start);
        if (it == function_last_return.end() || return_addr > it->second) {
            function_last_return[func_start] = return_addr;
        }
    }

    size_t jr_ra_discovered = function_starts.size();
    fmt::print("✓ Identified {} unique functions from jr $ra scan\n", jr_ra_discovered);

    // Pass 2.5: Follow JAL call targets to discover additional functions
    // This finds functions that don't have standard jr $ra prologues
    fmt::print("Following JAL call targets to discover additional functions...\n");

    uint32_t exe_start = exe_.header.load_address;
    uint32_t exe_end   = exe_.end_address();

    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;

        uint32_t opcode = (instr >> 26) & 0x3F;
        if (opcode == 3) {  // JAL
            // PS1 JAL target: upper 4 bits from KSEG0 region (0x80000000)
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;

            // Only add if target is within EXE range and word-aligned
            if (target >= exe_start && target < exe_end && (target & 3) == 0) {
                function_starts.insert(target);
            }
        }
    }

    result.call_discovered_count = static_cast<int>(function_starts.size() - jr_ra_discovered);
    fmt::print("✓ Identified {} unique functions ({} call-discovered)\n\n",
               function_starts.size(), result.call_discovered_count);

    // Pass 2.55: promote strong standalone prologues.
    //
    // Return-scanning alone misses scheduler/task entry points that never
    // return normally, and direct JAL following misses callback entries whose
    // addresses are derived at runtime. A standard stack-frame prologue that
    // quickly saves $ra is a strong function-entry signal, provided the ADDIU
    // is not itself a branch delay slot.
    fmt::print("Finding strong standalone prologue entries...\n");
    int strong_prologues = 0;
    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;

        int32_t stack_size = 0;
        if (!is_prologue(*word_opt, stack_size)) continue;

        if (addr >= exe_start + 4) {
            auto prev_opt = exe_.read_word(addr - 4);
            if (prev_opt.has_value() && is_branch_or_jump(*prev_opt)) {
                continue;
            }
        }

        if (addr >= exe_start + 8) {
            auto prev2_opt = exe_.read_word(addr - 8);
            auto prev1_opt = exe_.read_word(addr - 4);
            if (prev2_opt.has_value() && prev1_opt.has_value()) {
                uint32_t lui_rt = 0;
                if (is_lui(*prev2_opt, lui_rt) &&
                    is_lw_same_reg_base(*prev1_opt, lui_rt)) {
                    continue;
                }
            }
        }

        auto existing_it = function_starts.upper_bound(addr);
        if (existing_it != function_starts.begin()) {
            --existing_it;
            auto return_it = function_last_return.find(*existing_it);
            if (return_it != function_last_return.end() &&
                return_it->second + 8u > addr) {
                continue;
            }
        }

        bool saves_ra = false;
        for (uint32_t look = addr + 4; look < addr + 64 && look < exe_end; look += 4) {
            auto next_opt = exe_.read_word(look);
            if (!next_opt.has_value()) break;
            if (is_sw_reg_sp(*next_opt, 31)) {
                saves_ra = true;
                break;
            }
            if (is_branch_or_jump(*next_opt)) {
                break;
            }
        }

        if (saves_ra && function_starts.insert(addr).second) {
            strong_prologues++;
        }
    }
    result.strong_prologue_count = strong_prologues;
    fmt::print("Identified {} strong prologue entries\n\n",
               result.strong_prologue_count);

    // Pass 2.6: discover continuations saved by setjmp/SaveState-style helpers.
    //
    // Some games save the caller's $ra into an application context object and
    // later restore it from an exception/VSync callback. The restored PC is the
    // instruction after the original JAL delay slot, not a normal prologue, so
    // it must be available as a split dispatch entry.
    fmt::print("Finding SaveState-style continuation entries...\n");
    int state_continuations = 0;
    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;
        uint32_t opcode = (instr >> 26) & 0x3F;
        if (opcode != 3) continue;  // JAL

        uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;
        uint32_t cont = addr + 8;
        if (target < exe_start || target >= exe_end || (target & 3) != 0) continue;
        if (cont < exe_start || cont >= exe_end || (cont & 3) != 0) continue;

        auto target_first = exe_.read_word(target);
        if (!target_first.has_value()) continue;

        // sw $ra, imm($a0) at callee entry is the narrow signature for these
        // context-save helpers. It avoids splitting ordinary calls.
        if (is_sw_reg_base(*target_first, 4, 31)) {
            bool inserted = function_starts.insert(cont).second;
            if (inserted) state_continuations++;
        }
    }
    result.state_continuation_count = state_continuations;
    fmt::print("Identified {} SaveState continuation entries\n\n",
               result.state_continuation_count);

    // Pass 2.7: Add forced entry points
    // These are function starts that do not have a standard prologue (e.g. the
    // PS1 EXE entry point, which is launched directly by the BIOS without a
    // JAL and starts with a BSS-zeroing loop rather than ADDIU $sp, $sp, -N).
    if (!forced_entry_points_.empty()) {
        fmt::print("Adding {} forced entry point(s)...\n", forced_entry_points_.size());
        for (uint32_t addr : forced_entry_points_) {
            bool inserted = function_starts.insert(addr).second;
            if (inserted) {
                fmt::print("  Forced entry 0x{:08X} added\n", addr);
            } else {
                fmt::print("  Forced entry 0x{:08X} already present (skipped)\n", addr);
            }
        }
        fmt::print("\n");
    }

    // Pass 3: Build function table with details
    std::vector<uint32_t> starts_vec(function_starts.begin(), function_starts.end());
    std::sort(starts_vec.begin(), starts_vec.end());

    for (size_t i = 0; i < starts_vec.size(); i++) {
        Function func;
        func.start_addr = starts_vec[i];

        // Estimate end address (next function start or end of executable)
        if (i + 1 < starts_vec.size()) {
            func.end_addr = starts_vec[i + 1];
        } else {
            func.end_addr = end_addr;
        }
        auto ret_it = function_last_return.find(func.start_addr);
        if (ret_it != function_last_return.end()) {
            uint32_t return_end = ret_it->second + 8u;  /* include jr delay slot */
            if (return_end > func.start_addr && return_end < func.end_addr) {
                func.end_addr = return_end;
            }
        }

        func.size = func.end_addr - func.start_addr;
        func.name = fmt::format("func_{:08X}", func.start_addr);

        // Check for prologue at start
        auto first_instr = exe_.read_word(func.start_addr);
        if (first_instr.has_value()) {
            int32_t stack_size;
            func.has_prologue = is_prologue(*first_instr, stack_size);
            func.stack_frame_size = func.has_prologue ? stack_size : 0;
        } else {
            func.has_prologue = false;
            func.stack_frame_size = 0;
        }

        // Check for jr $ra before end
        func.has_epilogue = false;
        uint32_t search_end = func.end_addr - 4;
        for (uint32_t addr = func.start_addr; addr <= search_end && addr < func.end_addr; addr += 4) {
            auto instr_opt = exe_.read_word(addr);
            if (instr_opt.has_value() && is_jr_ra(*instr_opt)) {
                func.has_epilogue = true;
                break;
            }
        }

        // Check if this "function" is actually a data section
        func.is_data_section = is_likely_data_section(func.start_addr, func.end_addr);

        result.functions.push_back(func);
    }

    return result;
}

} // namespace PSXRecomp
