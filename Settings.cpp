#include "Settings.hpp"
#include "hardware/flash.h"
#include "pico/flash.h"
#include <cstdio>
#include <cstring>

extern char __flash_binary_end;  // set by the linker

namespace settings {

namespace {

constexpr uint32_t MAGIC = 0x46524354;  // "FRCT"
// Increment when the layout of Settings changes, older records are then ignored
constexpr uint16_t VERSION = 2;
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
uint32_t checksum(const Record& r) {
    uint32_t hash = 2166136261u;
    auto add = [&hash](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    };
    add(&r.sequence, sizeof(r.sequence));
    add(&r.settings, sizeof(r.settings));
    return hash;
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

// The storage must not overlap the program
bool storage_free() {
    return reinterpret_cast<uintptr_t>(&__flash_binary_end) <= XIP_BASE + STORAGE_OFFSET;
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

}  // namespace

bool load(Settings& out) {
    int newest = scan();
    if (newest < 0) return false;
    Record r;
    memcpy(&r, slot_address(newest), sizeof(r));
    if (r.version != VERSION || r.size != sizeof(Settings)) return false;
    out = r.settings;
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

}  // namespace settings
