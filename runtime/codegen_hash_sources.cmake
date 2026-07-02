# codegen_hash_sources.cmake — the CANONICAL list of recompiler codegen sources
# folded into the overlay cache tag hash (hash_codegen.cmake).
#
# Included from BOTH:
#   - runtime/runtime.cmake        (bakes the hash into the runtime's cg tag)
#   - recompiler/CMakeLists.txt    (bakes the SAME hash into psxrecomp-game,
#                                   exposed via `--codegen-hash`)
# so the two hash computations can never drift by list divergence. Any file
# added to the emitter pipeline must be added HERE, once.
#
# Input:  PSXRECOMP_CODEGEN_HASH_ROOT — the psxrecomp repo root.
# Output: PSXRECOMP_CODEGEN_HASH_SRCS — absolute paths to hash.

set(PSXRECOMP_CODEGEN_HASH_SRCS
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/code_generator.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/mips_decoder.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/control_flow.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/basic_block.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_analysis.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/full_function_emitter.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/strict_translator.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_discovery.h)
