cmake_minimum_required(VERSION 3.12)

# Set PICO_SDK_PATH if not defined
# if(NOT DEFINED ENV{PICO_SDK_PATH})
    # set(ENV{PICO_SDK_PATH} "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
# endif()

# Set PIMORONI_PICO_PATH directly in CMake
set(PIMORONI_PICO_PATH "${CMAKE_CURRENT_LIST_DIR}/pimoroni-pico" CACHE PATH "Path to the Pimoroni Pico libraries")
set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk" CACHE PATH "Path to the Pico sdk")

# Change your executable name to something creative!
set(NAME FractalisPico)

include(pico_sdk_import.cmake)
include(pimoroni_pico_import.cmake)

# Gooey boilerplate
project(${NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# Add your source files
add_executable(${NAME}
    FractalisPico.cpp # Your main source file
)

# Include required libraries
# This assumes `pimoroni-pico` is stored alongside your project
include(common/pimoroni_i2c)
include(common/pimoroni_bus)
include(libraries/bitmap_fonts/bitmap_fonts)
include(libraries/hershey_fonts/hershey_fonts)
include(libraries/pico_graphics/pico_graphics)
include(libraries/pico_display/pico_display)
include(drivers/rgbled/rgbled)
include(drivers/button/button)
include(drivers/st7789/st7789)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_SDK_PATH}/src/common/pico_stdlib/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_gpio/include
    ${PICO_SDK_PATH}/src/common/pico_base/include
    ${PIMORONI_PICO_PATH}
    ${PIMORONI_PICO_PATH}/common
    ${PIMORONI_PICO_PATH}/libraries/pico_graphics
    ${PIMORONI_PICO_PATH}/libraries/pico_display
    ${PIMORONI_PICO_PATH}/drivers/rgbled
    ${PIMORONI_PICO_PATH}/drivers/button
    ${PIMORONI_PICO_PATH}/drivers/st7789
)

# Don't forget to link the libraries you need!
target_link_libraries(${NAME}
    pico_stdlib
    hardware_spi
    hardware_pwm
    hardware_dma
    pimoroni_i2c
    pico_graphics
    pico_display
    pico_multicore
    bitmap_fonts
    hershey_fonts
    rgbled
    button
    st7789
)

# Enable USB output, disable UART output
pico_enable_stdio_usb(${NAME} 1)
pico_enable_stdio_uart(${NAME} 0)

# create map/bin/hex file etc.
pico_add_extra_outputs(${NAME})

# Set up files for the release packages
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.uf2
    ${CMAKE_CURRENT_LIST_DIR}/README.md
    DESTINATION .
)

set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_GENERATOR "ZIP" "TGZ")
include(CPack)
