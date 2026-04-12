cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

add_executable(psx-runtime
    src/main.cpp
    src/memory.c
    src/timers.c
    src/interrupts.c
    src/traps.c
    ../generated/SCPH1001_full.c
    ../generated/SCPH1001_dispatch.c
)

target_include_directories(psx-runtime PRIVATE
    include
)

# Large stack for deep call chains during BIOS execution.
if(MINGW OR WIN32)
    target_link_options(psx-runtime PRIVATE -Wl,--stack,67108864)
endif()
