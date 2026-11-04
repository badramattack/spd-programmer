cmake_minimum_required(VERSION 3.13)

# PICO_SDK_PATH must point to a Pico SDK checkout (2.x). Without it, the SDK is
# fetched from GitHub into the build directory.
if (NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_FETCH_FROM_GIT ON)
    set(PICO_SDK_FETCH_FROM_GIT_TAG 2.1.1)
endif()

set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(spd_programmer C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(spd_programmer
    src/main.c
    src/board.c
    src/power.c
    src/bus.c
    src/ee1004.c
    src/spd5118.c
    src/spd.c
    src/cmd.c
    src/edit.c
    src/store.c
    src/presets.c
    src/ui.c
    src/timing.c
)

target_compile_options(spd_programmer PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(spd_programmer
    pico_stdlib
    pico_unique_id
    hardware_i2c
    hardware_adc
    hardware_pwm
    hardware_flash
    pico_flash
    hardware_watchdog
)

# USB CDC only. The SDK's USB stdio also exposes the reset interface, so
# `picotool load -f` / `picotool reboot -f -u` work without pressing BOOTSEL.
pico_enable_stdio_usb(spd_programmer 1)
pico_enable_stdio_uart(spd_programmer 0)
target_compile_definitions(spd_programmer PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_DEFAULT_CRLF=0
    # The host tool finds the board by this USB product string.
    USBD_PRODUCT="SPD Programmer"
)

pico_set_program_name(spd_programmer "spd-programmer")
pico_set_program_description(spd_programmer "DDR4/DDR5 SPD programmer (EE1004 / SPD5118)")

pico_add_extra_outputs(spd_programmer)
