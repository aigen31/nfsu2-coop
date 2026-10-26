cmake_minimum_required(VERSION 3.20)
project(nfsu2coop LANGUAGES C CXX)

# The supported, tested build is build.ps1 (MinGW i686). This file mirrors it for
# MSVC / IDE users; the same targets, the same flags that matter.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(_WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# --- ENet (vendored, MIT) ---------------------------------------------------
file(GLOB ENET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/enet/*.c")
list(FILTER ENET_SOURCES EXCLUDE REGEX "unix\\.c$")
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/enet/include")
target_link_libraries(enet PUBLIC ws2_32 winmm)

# --- portable core ----------------------------------------------------------
file(GLOB CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
add_library(nfc_core STATIC ${CORE_SOURCES})
target_include_directories(nfc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(nfc_core PUBLIC enet)

# --- apps -------------------------------------------------------------------
add_executable(coopd src/apps/coopd.cpp)
target_link_libraries(coopd PRIVATE nfc_core)

add_executable(coop-bot src/apps/coopbot.cpp)
target_link_libraries(coop-bot PRIVATE nfc_core)

add_executable(coop-probe src/apps/probe.cpp src/game/scanner.cpp)
target_link_libraries(coop-probe PRIVATE nfc_core)

# --- tests ------------------------------------------------------------------
foreach(t core sync net)
    if(t STREQUAL "core")
        add_executable(core-tests tests/core_tests.cpp)
        target_link_libraries(core-tests PRIVATE nfc_core)
    elseif(t STREQUAL "sync")
        add_executable(sync-tests tests/sync_tests.cpp)
        target_link_libraries(sync-tests PRIVATE nfc_core)
    else()
        add_executable(net-tests tests/net_tests.cpp)
        target_link_libraries(net-tests PRIVATE nfc_core)
    endif()
    target_include_directories(${t}-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
endforeach()

# --- game plugin ------------------------------------------------------------
set(IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_dx9.cpp)

set(MINHOOK_SOURCES
    vendor/minhook/src/buffer.c
    vendor/minhook/src/hook.c
    vendor/minhook/src/trampoline.c
    vendor/minhook/src/hde/hde32.c
    vendor/minhook/src/hde/hde64.c)

add_library(NFSU2Coop SHARED
    src/plugin/main.cpp
    src/game/config.cpp
    src/game/nfsu2_bindings.cpp
    src/game/scanner.cpp
    src/game/overlay.cpp
    ${IMGUI_SOURCES}
    ${MINHOOK_SOURCES})

target_include_directories(NFSU2Coop PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/minhook/include")

target_link_libraries(NFSU2Coop PRIVATE nfc_core d3d9 dxguid user32 gdi32 ole32)

set_target_properties(NFSU2Coop PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "NFSU2Coop")

# Keep the tested compiler honest: 32-bit only, speed2.exe is i386.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "NFSU2Coop must be built for x86 (use -A Win32 or an i686 toolchain).")
endif()
