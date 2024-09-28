#!/bin/bash

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

