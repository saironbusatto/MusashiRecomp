cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- Shared sources (both native and oracle) ----

set(PSX_SHARED_SOURCES
    src/main.cpp
    src/memory.c
    src/gpu.c
    src/gpu_sw_renderer.c
    src/dma.c
    src/timers.c
    src/interrupts.c
    src/sio.c
    src/memcard.c
    src/debug_server.c
    src/traps.c
    src/gte.cpp
    src/crc32.c
    src/cdrom.c
    src/spu.c
    src/iso_reader.cpp
    src/iso_reader_c.cpp
)

# ---- Native target: recompiled BIOS, stub interpreter ----

add_executable(psx-runtime
    ${PSX_SHARED_SOURCES}
    src/stub_interpreter.c
    ../generated/SCPH1001_full.c
    ../generated/SCPH1001_dispatch.c
)

target_include_directories(psx-runtime PRIVATE include)
target_compile_definitions(psx-runtime PRIVATE
    PSX_NATIVE_BUILD=1
    DEFAULT_DEBUG_PORT=4370
)

# ---- Oracle target: R3000A interpreter, no generated code ----

add_executable(psx-oracle
    ${PSX_SHARED_SOURCES}
    src/psx_interpreter.c
    ../generated/SCPH1001_full.c
    ../generated/SCPH1001_dispatch.c
)

target_include_directories(psx-oracle PRIVATE include)
target_compile_definitions(psx-oracle PRIVATE
    PSX_ORACLE_BUILD=1
    DEFAULT_DEBUG_PORT=4371
)

# ---- Common dependencies ----

foreach(target psx-runtime psx-oracle)
    # SDL2 for display presentation.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})

    # Winsock2 for TCP debug server.
    if(WIN32 OR MINGW)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()

    # Large stack for deep call chains during BIOS execution.
    if(MINGW OR WIN32)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
    endif()
endforeach()
