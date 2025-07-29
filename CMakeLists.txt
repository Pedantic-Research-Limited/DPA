cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_dpa_dsp)

# Initialize the SDK
pico_sdk_init()

# Create executable
add_executable(pico_dpa_dsp
    main.c
)

# Link libraries
target_link_libraries(pico_dpa_dsp 
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_timer
    hardware_irq
)

# Enable USB output, disable UART
pico_enable_stdio_usb(pico_dpa_dsp 1)
pico_enable_stdio_uart(pico_dpa_dsp 0)

# Create UF2 file (for drag-and-drop)
pico_add_extra_outputs(pico_dpa_dsp)