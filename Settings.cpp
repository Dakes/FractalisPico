#include "Settings.hpp"
#include "hardware/flash.h"
#include "pico/flash.h"
#include <cstdio>
#include <cstring>
#include <new>

extern char __flash_binary_end;  // set by the linker

namespace settings {

namespace {

constexpr uint32_t MAGIC = 0x46524354;  // "FRCT"
// Increment when the layout of Settings changes, older records are then ignored
constexpr uint16_t VERSION = 2;  // fields added at the end keep the version, older records are just shorter
constexpr int SECTORS = 4;
constexpr uint32_t STORAGE_SIZE = SECTORS * FLASH_SECTOR_SIZE;
constexpr uint32_t STORAGE_OFFSET = PICO_FLASH_SIZE_BYTES - STORAGE_SIZE;
constexpr int SLOTS = STORAGE_SIZE / FLASH_PAGE_SIZE;
constexpr int SLOTS_PER_SECTOR = FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE;
constexpr uint32_t FLASH_TIMEOUT_MS = 100;

struct Record {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t checksum;  // over sequence and settings
    Settings settings;
};
static_assert(sizeof(Record) <= FLASH_PAGE_SIZE, "a record must fit into a flash page");

bool scanned = false;
int next_slot = 0;
uint32_t next_sequence = 0;

const uint8_t* slot_address(int slot) {
    return reinterpret_cast<const uint8_t*>(XIP_BASE + STORAGE_OFFSET + slot * FLASH_PAGE_SIZE);
}

// FNV-1a
constexpr uint32_t FNV_START = 2166136261u;
uint32_t fnv(uint32_t hash, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

uint32_t checksum(const Record& r) {
    uint32_t hash = fnv(FNV_START, &r.sequence, sizeof(r.sequence));
    return fnv(hash, &r.settings, r.size <= sizeof(r.settings) ? r.size : sizeof(r.settings));
}

bool is_record(const Record& r) {
    return r.magic == MAGIC && r.checksum == checksum(r);
}

bool is_blank(int slot) {
    const uint8_t* bytes = slot_address(slot);
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
        if (bytes[i] != 0xFF) return false;
    }
    return true;
}

// Saved views: in front of the settings, two sectors per slot (a header page and the thumbnail)
constexpr uint32_t VIEW_SLOT_SIZE = 2 * FLASH_SECTOR_SIZE;
constexpr uint32_t VIEWS_OFFSET = STORAGE_OFFSET - VIEW_SLOTS * VIEW_SLOT_SIZE;
constexpr uint32_t VIEW_MAGIC = 0x46525657;  // "FRVW"
constexpr uint16_t VIEW_VERSION = 1;
constexpr uint32_t THUMBNAIL_BYTES = THUMBNAIL_W * THUMBNAIL_H * sizeof(uint16_t);

struct ViewRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t has_thumbnail;
    uint32_t checksum;  // over the view and the thumbnail
    uint32_t reserved;
    View view;
};
static_assert(sizeof(ViewRecord) <= FLASH_PAGE_SIZE, "the view record must fit into a flash page");
// The record page and the thumbnail, whole flash pages
constexpr uint32_t VIEW_BYTES = FLASH_PAGE_SIZE + THUMBNAIL_BYTES;
static_assert(VIEW_BYTES % FLASH_PAGE_SIZE == 0 && VIEW_BYTES <= VIEW_SLOT_SIZE, "a view must fit into its slot");

// Checked slots: 0 = not yet, 1 = valid, -1 = empty or broken
int8_t view_checked[VIEW_SLOTS] = {};

// The storage must not overlap the program
bool storage_free() {
    return reinterpret_cast<uintptr_t>(&__flash_binary_end) <= XIP_BASE + VIEWS_OFFSET;
}

// Finds the newest record. Returns its slot, -1 if there is none.
int scan() {
    int newest = -1;
    uint32_t newest_sequence = 0;
    for (int slot = 0; slot < SLOTS; ++slot) {
        Record r;
        memcpy(&r, slot_address(slot), sizeof(r));
        if (!is_record(r)) continue;
        if (newest < 0 || static_cast<int32_t>(r.sequence - newest_sequence) > 0) {
            newest = slot;
            newest_sequence = r.sequence;
        }
    }
    next_slot = (newest + 1) % SLOTS;
    next_sequence = newest_sequence + 1;
    scanned = true;
    return newest;
}

struct Write {
    uint32_t offset;
    bool erase;
    const uint8_t* page;
};

// Runs with interrupts disabled and the other core parked outside the flash
void write_page(void* param) {
    const Write* w = static_cast<const Write*>(param);
    if (w->erase) flash_range_erase(w->offset & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
    flash_range_program(w->offset, w->page, FLASH_PAGE_SIZE);
}

const uint8_t* view_address(int slot) {
    return reinterpret_cast<const uint8_t*>(XIP_BASE + VIEWS_OFFSET + slot * VIEW_SLOT_SIZE);
}

const ViewRecord& view_record(int slot) {
    return *reinterpret_cast<const ViewRecord*>(view_address(slot));
}

uint32_t view_checksum(const ViewRecord& r, const uint8_t* thumbnail) {
    uint32_t hash = fnv(FNV_START, &r.view, sizeof(r.view));
    return r.has_thumbnail ? fnv(hash, thumbnail, THUMBNAIL_BYTES) : hash;
}

bool view_valid(int slot) {
    if (slot < 0 || slot >= VIEW_SLOTS) return false;
    if (view_checked[slot] == 0) {
        const ViewRecord& r = view_record(slot);
        bool valid = r.magic == VIEW_MAGIC && r.version == VIEW_VERSION
                     && r.checksum == view_checksum(r, view_address(slot) + FLASH_PAGE_SIZE);
        view_checked[slot] = valid ? 1 : -1;
    }
    return view_checked[slot] > 0;
}

struct AreaWrite {
    uint32_t offset;
    uint32_t erase_size;
    const uint8_t* data;
    uint32_t size;
};

// Like write_page()
void write_area(void* param) {
    const AreaWrite* w = static_cast<const AreaWrite*>(param);
    flash_range_erase(w->offset, w->erase_size);
    flash_range_program(w->offset, w->data, w->size);
}

}  // namespace

bool load(Settings& out) {
    int newest = scan();
    if (newest < 0) return false;
    Record r;
    memcpy(&r, slot_address(newest), sizeof(r));
    if (r.version != VERSION || r.size > sizeof(Settings)) return false;
    // Fields that didn't exist yet stay 0
    memset(&out, 0, sizeof(out));
    memcpy(&out, &r.settings, r.size);
    return true;
}

bool save(const Settings& settings) {
    if (!storage_free()) {
        printf("Settings not saved: the program reaches into the settings storage\n");
        return false;
    }
    if (!scanned) scan();

    // Only blank slots can be programmed. If the next one isn't (e.g. a save was cut off by a power loss), continue
    // in the next sector. That never erases the newest record, it is in the sector before.
    int slot = next_slot;
    if (slot % SLOTS_PER_SECTOR != 0 && !is_blank(slot))
        slot = (slot / SLOTS_PER_SECTOR + 1) * SLOTS_PER_SECTOR % SLOTS;

    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    Record r = {MAGIC, VERSION, static_cast<uint16_t>(sizeof(Settings)), next_sequence, 0, settings};
    r.checksum = checksum(r);
    memcpy(page, &r, sizeof(r));

    Write w = {STORAGE_OFFSET + slot * FLASH_PAGE_SIZE, slot % SLOTS_PER_SECTOR == 0, page};
    int result = flash_safe_execute(write_page, &w, FLASH_TIMEOUT_MS);
    next_slot = (slot + 1) % SLOTS;
    next_sequence++;
    if (result != PICO_OK) {
        printf("Saving the settings failed: flash access error %d\n", result);
        return false;
    }
    if (memcmp(slot_address(slot), page, FLASH_PAGE_SIZE) != 0) {
        printf("Saving the settings failed: slot %d reads back wrong\n", slot);
        return false;
    }
    printf("Settings saved in slot %d%s\n", slot, w.erase ? " (sector erased)" : "");
    return true;
}

const View* view(int slot) {
    return view_valid(slot) ? &view_record(slot).view : nullptr;
}

const uint16_t* thumbnail(int slot) {
    if (!view_valid(slot) || !view_record(slot).has_thumbnail) return nullptr;
    return reinterpret_cast<const uint16_t*>(view_address(slot) + FLASH_PAGE_SIZE);
}

bool store_view(int slot, const View& view, void (*draw_thumbnail)(uint16_t* out)) {
    if (slot < 0 || slot >= VIEW_SLOTS) return false;
    if (!storage_free()) {
        printf("View not stored: the program reaches into the storage\n");
        return false;
    }
    // The flash is programmed from RAM: the record page with the thumbnail behind it
    uint8_t* buffer = new (std::nothrow) uint8_t[VIEW_BYTES];
    if (!buffer) {
        printf("View not stored: not enough memory\n");
        return false;
    }
    memset(buffer, 0xFF, VIEW_BYTES);
    ViewRecord r = {VIEW_MAGIC, VIEW_VERSION, draw_thumbnail != nullptr, 0, 0, view};
    if (draw_thumbnail) draw_thumbnail(reinterpret_cast<uint16_t*>(buffer + FLASH_PAGE_SIZE));
    r.checksum = view_checksum(r, buffer + FLASH_PAGE_SIZE);
    memcpy(buffer, &r, sizeof(r));

    AreaWrite w = {VIEWS_OFFSET + slot * VIEW_SLOT_SIZE, VIEW_SLOT_SIZE, buffer, VIEW_BYTES};
    int result = flash_safe_execute(write_area, &w, FLASH_TIMEOUT_MS);
    view_checked[slot] = 0;
    bool stored = result == PICO_OK && memcmp(view_address(slot), buffer, VIEW_BYTES) == 0;
    delete[] buffer;
    if (stored)
        printf("View stored in slot %d\n", slot + 1);
    else
        printf("Storing the view in slot %d failed (%d)\n", slot + 1, result);
    return stored;
}

}  // namespace settings
