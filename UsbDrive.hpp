#ifndef USB_DRIVE_H
#define USB_DRIVE_H

#include <cstdint>

/**
 * A small USB drive (FAT12, 1 MB) whose files are made up on the fly, nothing of it is stored:
 *   VIEWS.TXT   given by the main program
 *   SCREEN.BMP  the image, rendered row by row while the computer reads it
 *   README.TXT  how it works
 * What the computer writes isn't stored either. The drive picks lines of text out of it (like the Pico's own
 * bootloader drive picks UF2 blocks out of what is written to it), the main program takes them from there.
 *
 * Computers keep what they read from a drive. For new content the drive goes away for a few seconds (medium not
 * present) and comes back (medium changed), like a card in a card reader.
 *
 * The USB side (status, read, write, eject) runs wherever TinyUSB runs: in an interrupt on core0, or on core1 while
 * it prints. Everything else in the main loop.
 */
namespace usb_drive {

constexpr uint32_t SECTOR_SIZE = 512;
constexpr uint32_t SECTORS = 2048;
constexpr int IMAGE_W = 320;
constexpr int IMAGE_H = 240;

// A row of the image (y from the top) as RGB565 in native byte order. Called from the USB side.
using RowSource = void (*)(int y, uint16_t* row);
// Writes VIEWS.TXT into out, returns its length. Called from the main loop.
using ViewsSource = int (*)(char* out, int size);

/**
 * keywords: the lines the computer writes are only kept if they start with one of them (count of them in
 * keyword_count).
 */
void start(RowSource rows, ViewsSource views, const char* const* keywords, int keyword_count, uint32_t serial);
// New content: the drive goes away and comes back with it. Also brings it back after the computer ejected it.
void refresh();
// Call regularly from the main loop
void update(uint32_t now_ms);
// Present on the computer (not away for a refresh, not ejected)
bool present();
/**
 * The next line the computer wrote, without its line break, once the writing paused for a moment. Returns false if
 * there is none (yet).
 */
bool next_line(char* out, int size);

// USB side
enum class Status { READY, NOT_PRESENT, CHANGED };
// Test unit ready. CHANGED is returned once after the drive came back.
Status status();
// Returns the bytes read or written, -1 if the drive isn't present
int32_t read(uint32_t lba, uint32_t offset, void* buffer, uint32_t size);
int32_t write(uint32_t lba, uint32_t offset, const uint8_t* buffer, uint32_t size);
// The computer ejected the drive: it stays away until the next refresh()
void eject();

}  // namespace usb_drive

#endif // USB_DRIVE_H
