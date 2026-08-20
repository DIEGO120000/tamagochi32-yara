#ifndef _SAVESTATE_H_
#define _SAVESTATE_H_

#include <stdint.h>
#include "cpu.h"

#define EEPROM_MAGIC_LEGACY 0x2A
#define EEPROM_MAGIC_NEW    0x2B
#define EEPROM_MAGIC_NUMBER EEPROM_MAGIC_LEGACY

#pragma pack(push, 1)
struct SaveHeader {
    uint8_t magic;          // EEPROM_MAGIC_NEW (0x2B) or EEPROM_MAGIC_LEGACY (0x2A)
    uint8_t version;        // Save state version (e.g. 1)
    uint16_t state_size;    // sizeof(cpu_state_t)
    uint16_t memory_size;   // MEMORY_SIZE
    uint16_t checksum;      // Fletcher16 checksum of cpuState and memory
};
#pragma pack(pop)

void initEEPROM();

bool validEEPROM();

bool loadStateFromEEPROM(cpu_state_t* cpuState);

void eraseStateFromEEPROM();

void saveStateToEEPROM(cpu_state_t* cpuState);

void loadHardcodedState(cpu_state_t* cpuState);

inline uint8_t get_ram_nibble(const uint8_t* memory, uint16_t n) {
    if ((n & 1) == 0) {
        return (memory[n >> 1] >> 4) & 0x0F;
    } else {
        return memory[n >> 1] & 0x0F;
    }
}

inline void set_ram_nibble(uint8_t* memory, uint16_t n, uint8_t val) {
    if ((n & 1) == 0) {
        memory[n >> 1] = (memory[n >> 1] & 0x0F) | ((val & 0x0F) << 4);
    } else {
        memory[n >> 1] = (memory[n >> 1] & 0xF0) | (val & 0x0F);
    }
}

#endif /* _SAVESTATE_H_ */