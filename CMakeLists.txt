# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico_dp3364 C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

include(FetchContent)

FetchContent_Declare(
  minigif
  GIT_REPOSITORY https://github.com/illia-lykhoshvai/minigif.git
  GIT_TAG        main  # or a specific tag/commit/branch
)
FetchContent_MakeAvailable(minigif)

# -----------------------------------------------------------------------
# Two targets built from the SAME source tree (main.c, led_panel.c), swapped
# via the HUB75_DRIVER compile definition (see led_panel.h):
#   pico_dp3364_pio  – PIO-accelerated driver (fast, current focus)
#   pico_dp3364_cpu  – CPU bit-banged driver (original baseline, useful as
#                       a control experiment for symptoms that might be
#                       hardware/wiring-related rather than PIO-specific)
# -----------------------------------------------------------------------

add_executable(pico_dp3364_pio main.c led_panel.c)

# Compile the PIO assembly and generate the header hub75_pio.pio.h
pico_generate_pio_header(pico_dp3364_pio ${CMAKE_CURRENT_LIST_DIR}/hub75_pio.pio)

pico_set_program_name(pico_dp3364_pio "pico_dp3364_pio")
pico_set_program_version(pico_dp3364_pio "0.1")

pico_enable_stdio_uart(pico_dp3364_pio 0)
pico_enable_stdio_usb(pico_dp3364_pio 0)

target_link_libraries(pico_dp3364_pio minigif_lib
        pico_stdlib pico_multicore hardware_pio)

target_include_directories(pico_dp3364_pio PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(pico_dp3364_pio PRIVATE
        MATRIX_WIDTH=128
        MATRIX_HEIGHT=64
        HUB75_DRIVER=1   # HUB75_DRIVER_PIO
)

pico_add_extra_outputs(pico_dp3364_pio)

# -----------------------------------------------------------------------

add_executable(pico_dp3364_cpu main.c led_panel.c)

pico_set_program_name(pico_dp3364_cpu "pico_dp3364_cpu")
pico_set_program_version(pico_dp3364_cpu "0.1")

pico_enable_stdio_uart(pico_dp3364_cpu 0)
pico_enable_stdio_usb(pico_dp3364_cpu 0)

# No hardware_pio needed — the CPU driver never touches the PIO peripheral.
target_link_libraries(pico_dp3364_cpu minigif_lib
        pico_stdlib pico_multicore)

target_include_directories(pico_dp3364_cpu PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(pico_dp3364_cpu PRIVATE
        MATRIX_WIDTH=128
        MATRIX_HEIGHT=64
        HUB75_DRIVER=2   # HUB75_DRIVER_CPU
)

pico_add_extra_outputs(pico_dp3364_cpu)

