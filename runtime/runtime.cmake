# Shared psxrecomp-v4 runtime CMake helpers.
#
# Include this from either the framework runtime build or a sibling game
# project. Call psxrecomp_v4_add_runtime_target() after SDL2 detection has
# populated SDL2_INCLUDE_DIRS and SDL2_LIBRARIES.

if(NOT DEFINED PSXRECOMP_V4_ROOT)
    get_filename_component(PSXRECOMP_V4_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT SDL2_INCLUDE_DIRS OR NOT SDL2_LIBRARIES)
    if(MSVC)
        file(GLOB SDL2_MSVC_DIR "${PSXRECOMP_V4_ROOT}/../sdl2-msvc/SDL2-*")
        if(SDL2_MSVC_DIR)
            set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
            set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
            message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
        else()
            message(FATAL_ERROR "SDL2 MSVC dev package not found")
        endif()
    else()
        get_filename_component(_psxrecomp_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        find_program(_psxrecomp_pkg_config pkg-config
            HINTS "${_psxrecomp_compiler_dir}"
            NO_DEFAULT_PATH
        )
        if(_psxrecomp_pkg_config)
            set(PKG_CONFIG_EXECUTABLE "${_psxrecomp_pkg_config}" CACHE FILEPATH "pkg-config executable" FORCE)
        endif()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
endif()

set(PSXRECOMP_V4_RUNTIME_SOURCES
    ${PSXRECOMP_V4_ROOT}/runtime/src/main.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/memory.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/gpu.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/gpu_sw_renderer.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/dma.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/mdec.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/timers.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/interrupts.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/sio.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/memcard.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/debug_server.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/dirty_ram_interp.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/fntrace.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/traps.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/crash_trace.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/gte.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/crc32.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/cdrom.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/spu.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/iso_reader.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/iso_reader_c.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/psx_cycles.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/starvation_ring.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/card_read_summary.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/card_data_writes.c
)

set(PSXRECOMP_V4_RUNTIME_INCLUDE_DIRS
    ${PSXRECOMP_V4_ROOT}/runtime/include
)

set(PSXRECOMP_V4_BIOS_GENERATED
    ${PSXRECOMP_V4_ROOT}/generated/SCPH1001_full.c
    ${PSXRECOMP_V4_ROOT}/generated/SCPH1001_dispatch.c
)

function(psxrecomp_v4_add_runtime_target target)
    set(options ORACLE)
    set(oneValueArgs
        GAME_GENERATED_FULL_C
        GAME_GENERATED_DISPATCH_C
        DEBUG_PORT
        WINDOW_TITLE
        DEFAULT_BIOS_PATH
        DEFAULT_GAME_ROOT
        DEFAULT_MEMCARD_DIR
        DEFAULT_DISC_PATH
    )
    set(multiValueArgs EXTRAS_SOURCES)
    cmake_parse_arguments(PSXRT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT PSXRT_DEBUG_PORT)
        message(FATAL_ERROR "psxrecomp_v4_add_runtime_target(${target}) requires DEBUG_PORT")
    endif()
    if(NOT PSXRT_WINDOW_TITLE)
        set(PSXRT_WINDOW_TITLE "${target}")
    endif()
    if(NOT PSXRT_DEFAULT_BIOS_PATH)
        set(PSXRT_DEFAULT_BIOS_PATH "${PSXRECOMP_V4_ROOT}/bios/SCPH1001.BIN")
    endif()
    if(NOT PSXRT_DEFAULT_GAME_ROOT)
        set(PSXRT_DEFAULT_GAME_ROOT "${PSXRECOMP_V4_ROOT}")
    endif()
    if(NOT PSXRT_DEFAULT_MEMCARD_DIR)
        set(PSXRT_DEFAULT_MEMCARD_DIR "${PSXRT_DEFAULT_GAME_ROOT}")
    endif()
    if(NOT DEFINED PSXRT_DEFAULT_DISC_PATH)
        set(PSXRT_DEFAULT_DISC_PATH "")
    endif()

    set(generated_sources ${PSXRECOMP_V4_BIOS_GENERATED})
    if(PSXRT_GAME_GENERATED_FULL_C)
        set_source_files_properties("${PSXRT_GAME_GENERATED_FULL_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_GENERATED_FULL_C}")
        set(has_game_dispatch TRUE)
    endif()
    if(PSXRT_GAME_GENERATED_DISPATCH_C)
        set_source_files_properties("${PSXRT_GAME_GENERATED_DISPATCH_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_GENERATED_DISPATCH_C}")
        set(has_game_dispatch TRUE)
    endif()

    if(PSXRT_ORACLE)
        set(mode_source ${PSXRECOMP_V4_ROOT}/runtime/src/psx_interpreter.c)
    else()
        set(mode_source ${PSXRECOMP_V4_ROOT}/runtime/src/stub_interpreter.c)
    endif()

    add_executable(${target}
        ${PSXRECOMP_V4_RUNTIME_SOURCES}
        ${mode_source}
        ${generated_sources}
        ${PSXRT_EXTRAS_SOURCES}
    )

    target_include_directories(${target} PRIVATE
        ${PSXRECOMP_V4_RUNTIME_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})

    target_compile_definitions(${target} PRIVATE
        DEFAULT_DEBUG_PORT=${PSXRT_DEBUG_PORT}
        PSX_DEFAULT_BIOS_PATH="${PSXRT_DEFAULT_BIOS_PATH}"
        PSX_DEFAULT_GAME_ROOT="${PSXRT_DEFAULT_GAME_ROOT}"
        PSX_DEFAULT_MEMCARD_DIR="${PSXRT_DEFAULT_MEMCARD_DIR}"
        PSX_DEFAULT_DISC_PATH="${PSXRT_DEFAULT_DISC_PATH}"
        PSX_WINDOW_TITLE="${PSXRT_WINDOW_TITLE}"
        $<$<CXX_COMPILER_ID:MSVC>:SDL_MAIN_HANDLED>
    )

    if(PSXRT_ORACLE)
        target_compile_definitions(${target} PRIVATE PSX_ORACLE_BUILD=1)
    else()
        target_compile_definitions(${target} PRIVATE
            PSX_NATIVE_BUILD=1
            PSX_ENABLE_BLOCK_CYCLES=1
        )
    endif()
    if(has_game_dispatch)
        target_compile_definitions(${target} PRIVATE PSX_HAS_GAME_DISPATCH=1)
    endif()

    if(WIN32 OR MINGW)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()

    if(MINGW)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /GS- /guard:cf-)
        target_link_options(${target} PRIVATE /STACK:67108864,67108864 /GUARD:NO)
    endif()
endfunction()
