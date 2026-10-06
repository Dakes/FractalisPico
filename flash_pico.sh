#!/usr/bin/env bash
# Builds and flashes the firmware.
#   ./flash_pico.sh           normal build, the serial port stays silent
#   ./flash_pico.sh --debug   with the log on the serial port (tio /dev/ttyACM0), waits up to a second at startup
#                             for a serial terminal so it's complete

cd "$(dirname "$0")"

DEBUG=OFF
for arg in "$@"; do
    case "$arg" in
        --debug) DEBUG=ON ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--debug]"; exit 1 ;;
    esac
done

# Configure the build directory on first use, and again when the mode changes (the cache remembers it)
if [ ! -f build/build.ninja ] || ! grep -q "^FRACTALIS_DEBUG:BOOL=$DEBUG$" build/CMakeCache.txt; then
    cmake -B build -G Ninja -DFRACTALIS_DEBUG=$DEBUG || { echo "Configure failed. Exiting."; exit 1; }
fi
[ "$DEBUG" = ON ] && echo "Debug build"

# Build the project
ninja -C build
if [ $? -ne 0 ]; then
    echo "Build failed. Exiting."
    exit 1
fi

# Path to the UF2 file
UF2_FILE="build/FractalisPico.uf2"

# Function to flash the Pico
flash_pico() {
    picotool load "$UF2_FILE" -fx
}

# Loop until picotool succeeds
until flash_pico; do
    echo "Failed to flash. Please ensure the Pico is in bootloader mode."
    echo "Waiting for 1 second before retrying..."
    sleep 1
done

echo "Flash successful!"
