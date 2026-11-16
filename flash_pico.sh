#!/usr/bin/env bash
# Builds and flashes the firmware.
#   ./flash_pico.sh           normal build, the serial port stays silent
#   ./flash_pico.sh --debug   with the log on the serial port (tio /dev/ttyACM0), waits up to a second at startup
#                             for a serial terminal so it's complete
#   ./flash_pico.sh --small   for the Display Pack 1.14" (240 x 135) instead of the 2.8", built in build_small
# The flags can be combined.

cd "$(dirname "$0")"

DEBUG=OFF
SMALL=OFF
for arg in "$@"; do
    case "$arg" in
        --debug) DEBUG=ON ;;
        --small) SMALL=ON ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--debug] [--small]"; exit 1 ;;
    esac
done

# Each display has its own build directory, switching between them doesn't rebuild everything
BUILD=build
[ "$SMALL" = ON ] && BUILD=build_small

# Configure the build directory on first use, and again when the mode changes (the cache remembers it)
if [ ! -f $BUILD/build.ninja ] || ! grep -q "^FRACTALIS_DEBUG:BOOL=$DEBUG$" $BUILD/CMakeCache.txt \
        || ! grep -q "^FRACTALIS_SMALL_DISPLAY:BOOL=$SMALL$" $BUILD/CMakeCache.txt; then
    cmake -B $BUILD -G Ninja -DFRACTALIS_DEBUG=$DEBUG -DFRACTALIS_SMALL_DISPLAY=$SMALL \
        || { echo "Configure failed. Exiting."; exit 1; }
fi
[ "$DEBUG" = ON ] && echo "Debug build"
[ "$SMALL" = ON ] && echo "Display Pack 1.14\""

# Build the project
ninja -C $BUILD
if [ $? -ne 0 ]; then
    echo "Build failed. Exiting."
    exit 1
fi

# Path to the UF2 file
UF2_FILE="$BUILD/FractalisPico.uf2"

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
