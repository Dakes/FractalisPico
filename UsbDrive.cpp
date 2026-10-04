#include "UsbDrive.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>

#if PICO_ON_DEVICE
#include "pico/critical_section.h"
#else
#include <mutex>
#endif

namespace usb_drive {
namespace {

// Short locks between the USB side (an interrupt or the other core) and the main loop
#if PICO_ON_DEVICE
class SpinLock {
public:
    void init() { critical_section_init(&section); }
    void lock() { critical_section_enter_blocking(&section); }
    void unlock() { critical_section_exit(&section); }
private:
    critical_section_t section;
};
#else
class SpinLock {
public:
    void init() {}
    void lock() { mutex.lock(); }
    void unlock() { mutex.unlock(); }
private:
    std::mutex mutex;
};
#endif

struct Locked {
    explicit Locked(SpinLock& l) : l(l) { l.lock(); }
    ~Locked() { l.unlock(); }
    SpinLock& l;
};

// FAT12 layout: boot sector, 2 FATs, root directory, then one sector per cluster
constexpr uint32_t FAT_START = 1;
constexpr uint32_t FAT_SECTORS = 6;
constexpr uint32_t FATS = 2;
constexpr uint32_t ROOT_START = FAT_START + FATS * FAT_SECTORS;
constexpr uint32_t ROOT_ENTRIES = 224;
constexpr uint32_t ROOT_SECTORS = ROOT_ENTRIES * 32 / SECTOR_SIZE;
constexpr uint32_t DATA_START = ROOT_START + ROOT_SECTORS;
constexpr uint32_t CLUSTERS = SECTORS - DATA_START;  // numbered from 2
static_assert(CLUSTERS < 4085, "FAT12 has less than 4085 clusters");
static_assert((CLUSTERS + 2) * 3 / 2 + 1 <= FAT_SECTORS * SECTOR_SIZE, "the FAT must fit");

// 2026-01-01, the time counts the updates (2 s steps): computers and image viewers that cache by name, size and
// time see that the files changed
constexpr uint16_t FILE_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;
uint32_t generation = 0;
uint16_t file_time() {
    uint32_t seconds = (generation * 2) % 86400;
    return static_cast<uint16_t>(((seconds / 3600) << 11) | ((seconds / 60 % 60) << 5) | (seconds % 60 / 2));
}

constexpr int BMP_HEADER = 54;
constexpr int BMP_ROW = IMAGE_W * 3;
constexpr uint32_t BMP_SIZE = BMP_HEADER + BMP_ROW * IMAGE_H;

const char README[] =
    "FractalisPico USB drive\r\n"
    "\r\n"
    "VIEWS.TXT   the current view and the 10 saved views\r\n"
    "SCREEN.BMP  the picture on the screen\r\n"
    "\r\n"
    "The files show the state of the last update: Views > Update USB drive in the\r\n"
    "menu brings the drive up to date (it disappears for a few seconds).\r\n"
    "\r\n"
    "Change a line in VIEWS.TXT and save it, or copy a text file with such lines\r\n"
    "onto the drive. Lines that differ from VIEWS.TXT are taken over:\r\n"
    "  current          the device goes there\r\n"
    "  slot1 ... slot10 the view is stored in that slot (its picture comes when\r\n"
    "                   you go there)\r\n"
    "Then the drive updates itself.\r\n"
    "\r\n"
    "One view per line, the parts separated by spaces:\r\n"
    "  slot3 zoom 1.2889e+30 re -0.74528480285 im 0.11307472576 palette Ultra\r\n"
    "  bands 1x trap off shading off light top-left\r\n"
    "zoom     1 shows the whole set, 4 units across the width of the screen\r\n"
    "re, im   the center, up to 75 decimals\r\n"
    "target   optional, the zoom auto zoom dives to (e.g. a minibrot found)\r\n"
    "The look (palette to light) is optional, the names as in the menu, with -\r\n"
    "instead of spaces.\r\n";

struct File {
    char name[12];  // 8.3 without the dot
    uint8_t attributes;
    uint32_t first_cluster;  // 0 for an empty file
    uint32_t size;
};
enum { VIEWS, SCREEN, README_FILE, FILE_COUNT };
File files[FILE_COUNT] = {
    {"VIEWS   TXT", 0x20, 0, 0},
    {"SCREEN  BMP", 0x21, 0, BMP_SIZE},  // read only
    {"README  TXT", 0x21, 0, sizeof(README) - 1},
};

RowSource row_source = nullptr;
ViewsSource views_source = nullptr;
const char* const* line_keywords = nullptr;
int keyword_count = 0;
uint32_t volume_serial = 0;

// Changed only while the drive is away
char views_text[3584];

std::atomic<bool> started{false};
std::atomic<bool> is_present{false};
std::atomic<bool> changed{false};
std::atomic<bool> refresh_requested{false};
std::atomic<bool> eject_requested{false};
std::atomic<uint32_t> write_count{0};

// The refresh: away, new content, back
enum class Phase { PRESENT, AWAY, NEW_CONTENT, EJECTED };
Phase phase = Phase::PRESENT;
uint32_t away_since = 0;
constexpr uint32_t SWAP_MS = 300;    // no more USB access to the old content by then
constexpr uint32_t AWAY_MS = 3000;   // computers check every 1-2 s if a drive is still there
// The lines of a write are handed out once nothing was written for this long
constexpr uint32_t QUIET_MS = 800;
uint32_t seen_writes = 0;
uint32_t last_write_ms = 0;
bool writes_paused = true;

// Lines from the written data: assembled per write, then queued ('\0' after each)
SpinLock lock;
constexpr int LINE_MAX = 400;
char line[LINE_MAX];
int line_length = 0;
bool line_broken = false;  // too long or with binary data
uint64_t next_position = 0;  // byte position the next write continues the line at
char queue[3072];
int queue_head = 0, queue_used = 0;

uint32_t clusters_of(uint32_t size) {
    return (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
}

// The files one after the other from cluster 2
void lay_out() {
    uint32_t cluster = 2;
    for (File& f : files) {
        f.first_cluster = f.size ? cluster : 0;
        cluster += clusters_of(f.size);
    }
}

void make_content() {
    generation++;
    files[VIEWS].size = views_source ? static_cast<uint32_t>(std::max(0, views_source(views_text, sizeof(views_text))))
                                     : 0;
    lay_out();
}

uint16_t fat_entry(uint32_t n) {
    if (n == 0) return 0xFF8;  // media descriptor
    if (n == 1) return 0xFFF;
    for (const File& f : files) {
        uint32_t count = clusters_of(f.size);
        if (f.first_cluster && n >= f.first_cluster && n < f.first_cluster + count) {
            return n + 1 == f.first_cluster + count ? 0xFFF : static_cast<uint16_t>(n + 1);
        }
    }
    return 0;
}

void put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

void boot_sector(uint8_t* s) {
    static const uint8_t JUMP[] = {0xEB, 0x3C, 0x90};
    memcpy(s, JUMP, 3);
    memcpy(s + 3, "MSWIN4.1", 8);
    put16(s + 11, SECTOR_SIZE);
    s[13] = 1;  // sectors per cluster
    put16(s + 14, FAT_START);
    s[16] = FATS;
    put16(s + 17, ROOT_ENTRIES);
    put16(s + 19, SECTORS);
    s[21] = 0xF8;
    put16(s + 22, FAT_SECTORS);
    put16(s + 24, 32);  // sectors per track
    put16(s + 26, 64);  // heads
    s[36] = 0x80;       // drive number
    s[38] = 0x29;       // extended boot signature
    put32(s + 39, volume_serial);
    memcpy(s + 43, "FRACTALIS  ", 11);
    memcpy(s + 54, "FAT12   ", 8);
    s[62] = 0xEB;  // boot code: jump to itself
    s[63] = 0xFE;
    s[510] = 0x55;
    s[511] = 0xAA;
}

void fat_sector(uint32_t index, uint8_t* s) {
    // Two 12 bit entries in 3 bytes
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        uint32_t byte = index * SECTOR_SIZE + i;
        uint32_t pair = byte / 3;
        uint16_t e0 = fat_entry(2 * pair), e1 = fat_entry(2 * pair + 1);
        switch (byte % 3) {
            case 0: s[i] = static_cast<uint8_t>(e0); break;
            case 1: s[i] = static_cast<uint8_t>((e0 >> 8) | (e1 << 4)); break;
            default: s[i] = static_cast<uint8_t>(e1 >> 4); break;
        }
    }
}

void directory_sector(uint32_t index, uint8_t* s) {
    for (uint32_t i = 0; i < SECTOR_SIZE / 32; ++i) {
        uint32_t entry = index * (SECTOR_SIZE / 32) + i;
        uint8_t* e = s + 32 * i;
        if (entry == 0) {
            memcpy(e, "FRACTALIS  ", 11);
            e[11] = 0x08;  // volume label
        } else if (entry <= FILE_COUNT) {
            const File& f = files[entry - 1];
            memcpy(e, f.name, 11);
            e[11] = f.attributes;
            put16(e + 14, file_time());
            put16(e + 16, FILE_DATE);
            put16(e + 18, FILE_DATE);
            put16(e + 22, file_time());
            put16(e + 24, FILE_DATE);
            put16(e + 26, f.first_cluster);
            put32(e + 28, f.size);
        }
    }
}

// The cached row of SCREEN.BMP
uint16_t image_row[IMAGE_W];
int image_row_y = -1;

uint8_t bmp_byte(uint32_t offset) {
    if (offset < BMP_HEADER) {
        static uint8_t header[BMP_HEADER];
        if (offset == 0) {
            memset(header, 0, sizeof(header));
            header[0] = 'B';
            header[1] = 'M';
            put32(header + 2, BMP_SIZE);
            put32(header + 10, BMP_HEADER);
            put32(header + 14, 40);  // BITMAPINFOHEADER
            put32(header + 18, IMAGE_W);
            put32(header + 22, IMAGE_H);  // rows bottom up
            put16(header + 26, 1);
            put16(header + 28, 24);
            put32(header + 34, BMP_ROW * IMAGE_H);
            put32(header + 38, 2835);  // 72 dpi
            put32(header + 42, 2835);
            image_row_y = -1;  // a new read of the image: fresh rows
        }
        return header[offset];
    }
    uint32_t pixel = offset - BMP_HEADER;
    int y = IMAGE_H - 1 - static_cast<int>(pixel / BMP_ROW);
    uint32_t column = pixel % BMP_ROW;
    if (y != image_row_y) {
        if (row_source) {
            row_source(y, image_row);
        } else {
            memset(image_row, 0, sizeof(image_row));
        }
        image_row_y = y;
    }
    uint16_t v = image_row[column / 3];
    switch (column % 3) {
        case 0: { uint32_t b = v & 31; return static_cast<uint8_t>((b << 3) | (b >> 2)); }
        case 1: { uint32_t g = (v >> 5) & 63; return static_cast<uint8_t>((g << 2) | (g >> 4)); }
        default: { uint32_t r = v >> 11; return static_cast<uint8_t>((r << 3) | (r >> 2)); }
    }
}

void data_sector(uint32_t cluster, uint8_t* s) {
    for (int i = 0; i < FILE_COUNT; ++i) {
        const File& f = files[i];
        if (!f.first_cluster || cluster < f.first_cluster || cluster >= f.first_cluster + clusters_of(f.size)) continue;
        uint32_t start = (cluster - f.first_cluster) * SECTOR_SIZE;
        uint32_t n = std::min(SECTOR_SIZE, f.size - start);
        if (i == VIEWS) {
            memcpy(s, views_text + start, n);
        } else if (i == README_FILE) {
            memcpy(s, README + start, n);
        } else {
            for (uint32_t k = 0; k < n; ++k) s[k] = bmp_byte(start + k);
        }
        return;
    }
}

void fill_sector(uint32_t lba, uint8_t* s) {
    memset(s, 0, SECTOR_SIZE);
    if (lba == 0) {
        boot_sector(s);
    } else if (lba < ROOT_START) {
        fat_sector((lba - FAT_START) % FAT_SECTORS, s);
    } else if (lba < DATA_START) {
        directory_sector(lba - ROOT_START, s);
    } else if (lba < SECTORS) {
        data_sector(lba - DATA_START + 2, s);
    }
}

// ---- Lines in the written data (under the lock) ----

void queue_line() {
    if (line_length == 0 || line_broken) return;
    int start = 0;
    while (start < line_length && line[start] == ' ') start++;
    bool wanted = false;
    for (int k = 0; k < keyword_count && !wanted; ++k) {
        size_t n = strlen(line_keywords[k]);
        wanted = line_length - start >= static_cast<int>(n) && strncmp(line + start, line_keywords[k], n) == 0;
    }
    int length = line_length - start;
    if (!wanted || queue_used + length + 1 > static_cast<int>(sizeof(queue))) return;
    // A line of VIEWS.TXT written back as it was changes nothing (e.g. the file saved after changing another line)
    const char* text = views_text;
    const char* text_end = views_text + files[VIEWS].size;
    for (const char* p = text; p + length <= text_end; ++p) {
        if ((p == text || p[-1] == '\n') && memcmp(p, line + start, length) == 0
                && (p + length == text_end || p[length] == '\r' || p[length] == '\n')) {
            return;
        }
    }
    for (int i = 0; i <= length; ++i) {
        queue[(queue_head + queue_used) % sizeof(queue)] = i < length ? line[start + i] : '\0';
        queue_used++;
    }
}

void end_line() {
    queue_line();
    line_length = 0;
    line_broken = false;
}

void take_bytes(const uint8_t* data, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t c = data[i];
        if (c == '\n' || c == '\r' || c == 0) {
            end_line();
        } else if (c < 0x20 && c != '\t') {
            line_broken = true;  // binary
        } else if (line_length < LINE_MAX) {
            line[line_length++] = c == '\t' ? ' ' : static_cast<char>(c);
        } else {
            line_broken = true;
        }
    }
}

}  // namespace

void start(RowSource rows, ViewsSource views, const char* const* keywords, int count, uint32_t serial) {
    row_source = rows;
    views_source = views;
    line_keywords = keywords;
    keyword_count = count;
    volume_serial = serial;
    lock.init();
    make_content();
    phase = Phase::PRESENT;
    started = true;
    // In case the computer looked before: it reads the drive again
    changed = true;
    is_present = true;
}

void refresh() {
    refresh_requested = true;
}

bool present() {
    return is_present;
}

void update(uint32_t now_ms) {
    if (!started) return;
    uint32_t writes = write_count;
    if (writes != seen_writes) {
        seen_writes = writes;
        last_write_ms = now_ms;
        writes_paused = false;
    } else if (!writes_paused && now_ms - last_write_ms >= QUIET_MS) {
        writes_paused = true;
        // A last line without a line break
        Locked guard(lock);
        if (line_length > 0) end_line();
    }

    if (eject_requested.exchange(false)) phase = Phase::EJECTED;
    if (refresh_requested.exchange(false)) {
        is_present = false;
        phase = Phase::AWAY;
        away_since = now_ms;
    }
    switch (phase) {
        case Phase::AWAY:
            if (now_ms - away_since >= SWAP_MS) {
                make_content();
                phase = Phase::NEW_CONTENT;
            }
            break;
        case Phase::NEW_CONTENT:
            if (now_ms - away_since >= AWAY_MS) {
                phase = Phase::PRESENT;
                changed = true;
                is_present = true;
            }
            break;
        default:
            break;
    }
}

bool next_line(char* out, int size) {
    // Not while the computer is still writing (also not before update() noticed new writes)
    if (!started || !writes_paused || seen_writes != write_count) return false;
    Locked guard(lock);
    if (queue_used == 0) return false;
    int n = 0;
    while (queue_used > 0) {
        char c = queue[queue_head];
        queue_head = (queue_head + 1) % sizeof(queue);
        queue_used--;
        if (c == '\0') break;
        if (n < size - 1) out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

Status status() {
    if (!is_present) return Status::NOT_PRESENT;
    if (changed.exchange(false)) return Status::CHANGED;
    return Status::READY;
}

int32_t read(uint32_t lba, uint32_t offset, void* buffer, uint32_t size) {
    if (!is_present) return -1;
    // Sector by sector, through a buffer for partial ones (not on the stack: core1 has little)
    static uint8_t sector[SECTOR_SIZE];
    uint8_t* out = static_cast<uint8_t*>(buffer);
    for (uint32_t done = 0; done < size;) {
        uint32_t position = offset + done;
        uint32_t s = lba + position / SECTOR_SIZE, in = position % SECTOR_SIZE;
        uint32_t n = std::min(SECTOR_SIZE - in, size - done);
        if (in == 0 && n == SECTOR_SIZE) {
            fill_sector(s, out + done);
        } else {
            fill_sector(s, sector);
            memcpy(out + done, sector + in, n);
        }
        done += n;
    }
    return static_cast<int32_t>(size);
}

int32_t write(uint32_t lba, uint32_t offset, const uint8_t* buffer, uint32_t size) {
    if (!is_present) return -1;
    write_count++;
    // Only the data area can hold text, the file system itself is made up anyway
    if (lba >= DATA_START) {
        Locked guard(lock);
        uint64_t position = static_cast<uint64_t>(lba) * SECTOR_SIZE + offset;
        // Lines only continue in the sector that follows
        if (position != next_position) end_line();
        take_bytes(buffer, size);
        next_position = position + size;
    }
    return static_cast<int32_t>(size);
}

void eject() {
    is_present = false;
    eject_requested = true;
}

}  // namespace usb_drive
