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

# Add executable. Default name is the project name, version 0.1
add_executable(pico_dp3364 main.c led_panel.c)

pico_set_program_name(pico_dp3364 "pico_dp3364")
pico_set_program_version(pico_dp3364 "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pico_dp3364 0)
pico_enable_stdio_usb(pico_dp3364 0)

# Add the standard library to the build
target_link_libraries(pico_dp3364 minigif_lib
        pico_stdlib pico_multicore hardware_pio)

# Add the standard include files to the build
target_include_directories(pico_dp3364 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(pico_dp3364 PRIVATE
        MATRIX_WIDTH=128
        MATRIX_HEIGHT=64
)

pico_add_extra_outputs(pico_dp3364)

