cmake_minimum_required(VERSION 3.20)
project(virtual-xinput CXX)

if(NOT MSVC)
    message(FATAL_ERROR "virtual-xinput targets MSVC; the .def-based exports and "
                        "DirectInput libraries assume the Microsoft toolchain.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the DLL is dropped into arbitrary game folders, where no matching
# Visual C++ redistributable can be assumed to exist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(VX_BUILD_ALIASES
       "Also build xinput1_1/1_2/1_4/9_1_0.dll for games that load those versions"
       OFF)
option(VX_BUILD_TESTS "Build the test executables" ON)
option(VX_BUILD_GUI   "Build the windowed configurator (needs Direct3D 11)" ON)

add_compile_options(/W4 /permissive-)
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(VX_ARCH_DIR "x64")
else()
    set(VX_ARCH_DIR "x86")
endif()

# ---------------------------------------------------------------------------
# Shared code
# ---------------------------------------------------------------------------
add_library(vx_common STATIC
    src/common/config.cpp
    src/common/detect.cpp
    src/common/di_device.cpp
    src/common/games.cpp
    src/common/log.cpp
    src/common/mapping.cpp
    src/common/pad_manager.cpp
    src/common/profiles.cpp
    src/common/strutil.cpp
    src/common/yaml.cpp
)
target_include_directories(vx_common PUBLIC src/common)

# dinput8 supplies the c_dfDIJoystick2 data format; DirectInput8Create itself is
# resolved at runtime from System32, so no dinput8.dll import ends up in the
# DLL's import table. ole32 is for CLSIDFromString.
target_link_libraries(vx_common PUBLIC dinput8 dxguid user32 ole32)

# ---------------------------------------------------------------------------
# The wrapper DLLs
#
# Each XInput version gets its OWN .def file, because the real Microsoft DLLs
# number their exports differently per version. These are emphatically not
# interchangeable copies: the DirectX SDK import libraries link XInput BY
# ORDINAL, so a game built against them will call whatever sits at that ordinal.
# Wrong numbering means the wrong function with a mismatched __stdcall
# signature, which corrupts the stack and crashes the game.
# ---------------------------------------------------------------------------
function(vx_add_wrapper name)
    add_library(${name} SHARED
        src/dll/dllmain.cpp
        src/dll/xinput_exports.cpp
        src/dll/${name}.def
    )
    target_link_libraries(${name} PRIVATE vx_common)
    set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}" PREFIX "")
    install(FILES $<TARGET_FILE:${name}> DESTINATION ${VX_ARCH_DIR})
endfunction()

vx_add_wrapper(xinput1_3)

if(VX_BUILD_ALIASES)
    vx_add_wrapper(xinput1_4)
    vx_add_wrapper(xinput9_1_0)
    vx_add_wrapper(xinput1_1)
    vx_add_wrapper(xinput1_2)
endif()

# ---------------------------------------------------------------------------
# Console configurator
# ---------------------------------------------------------------------------
add_executable(virtual-xinput-config src/configurator/main.cpp)
target_link_libraries(virtual-xinput-config PRIVATE vx_common)

# ---------------------------------------------------------------------------
# Windowed configurator
#
# Dear ImGui is vendored under third_party/imgui; see VENDORING.md there. It is
# a separate target purely so the project's /W4 does not apply to it - upstream
# is warning-clean at its own settings, not at ours, and patching it would make
# updating painful.
# ---------------------------------------------------------------------------
if(VX_BUILD_GUI)
    set(VX_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

    add_library(vx_imgui STATIC
        ${VX_IMGUI_DIR}/imgui.cpp
        ${VX_IMGUI_DIR}/imgui_draw.cpp
        ${VX_IMGUI_DIR}/imgui_tables.cpp
        ${VX_IMGUI_DIR}/imgui_widgets.cpp
        ${VX_IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${VX_IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(vx_imgui PUBLIC ${VX_IMGUI_DIR} ${VX_IMGUI_DIR}/backends)
    target_compile_options(vx_imgui PRIVATE /W0)

    # imgui_impl_win32.cpp LoadLibrary's xinput1_4.dll, then xinput1_3.dll, and
    # so on, to drive gamepad UI navigation. Those are the names this project
    # builds: run the GUI from a folder holding our wrapper and ImGui would load
    # it, standing up a second DirectInput stack inside the configurator that
    # polls the very pads the configurator is already reading. It would also let
    # the pad being configured move the UI focus while you configure it.
    target_compile_definitions(vx_imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

    # d3dcompiler, gdi32 and dwmapi arrive through #pragma comment(lib) in the
    # backends themselves.
    target_link_libraries(vx_imgui PUBLIC d3d11 dxgi)

    add_executable(virtual-xinput-gui WIN32
        src/gui/main.cpp
        src/gui/platform.cpp
        src/gui/app.cpp
        src/gui/draw_pad.cpp
    )
    target_link_libraries(virtual-xinput-gui PRIVATE vx_common vx_imgui)
    install(PROGRAMS $<TARGET_FILE:virtual-xinput-gui> DESTINATION .)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VX_BUILD_TESTS)
    enable_testing()

    add_executable(vx_tests tests/test_main.cpp)
    target_link_libraries(vx_tests PRIVATE vx_common)
    add_test(NAME vx_tests COMMAND vx_tests)

    # Loads the built DLL the way a game does. Not linked against vx_common, so
    # it exercises the real export surface rather than the internals.
    add_executable(vx_dll_host tests/test_dll_host.cpp)
    add_dependencies(vx_dll_host xinput1_3)
    add_test(NAME vx_dll_host COMMAND vx_dll_host $<TARGET_FILE:xinput1_3>)
endif()

# ---------------------------------------------------------------------------
# Install layout
#
#   dist/
#     virtual-xinput-gui.exe       the windowed configurator
#     virtual-xinput-config.exe    run from anywhere; keeps games.yml beside it
#     virtual-xinput.yml           reference config
#     README.md
#     x86/xinput1_3.dll            payload the configurator installs into games
#     x64/xinput1_3.dll
#
# Explicit FILES/PROGRAMS rules rather than install(TARGETS), so the import
# libraries and .exp files stay out of it. The DLLs install themselves through
# vx_add_wrapper above.
#
# The configurator is architecture-independent in what it can reach, so a single
# copy sits at the root. Installing x86 then x64 leaves the 64-bit build there.
# ---------------------------------------------------------------------------
install(PROGRAMS $<TARGET_FILE:virtual-xinput-config> DESTINATION .)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/virtual-xinput.yml
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    DESTINATION .)
