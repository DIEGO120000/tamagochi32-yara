#include <Arduino.h>
#include <EEPROM.h>
#include "cpu.h"
#include "savestate.h"
#include "hardcoded_state.h"
#include "config_fix.h"

// Calculate Fletcher16 checksum over the cpu_state_t and the memory array
uint16_t calculateStateChecksum(const cpu_state_t* state, const u4_t* memory_data, uint16_t memory_len) {
    // Copy to temporary state to zero out volatile pointer before checksumming
    cpu_state_t tempState = *state;
    tempState.memory = nullptr;

    uint16_t sum1 = 0;
    uint16_t sum2 = 0;

    // Checksum the struct
    const uint8_t* pState = (const uint8_t*)&tempState;
    for (size_t i = 0; i < sizeof(cpu_state_t); ++i) {
        sum1 = (sum1 + pState[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }

    // Checksum the memory
    for (size_t i = 0; i < memory_len; ++i) {
        sum1 = (sum1 + memory_data[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }

    return (sum2 << 8) | sum1;
}

// Perform strict logical validation of cpu_state_t and memory variables
bool isStateValid(const cpu_state_t* state) {
    if (state == nullptr) return false;

    // 1. Program counter and general register bounds checking
    if (state->pc > 8191) {
        Serial.println(F("[integrity] PC out of bounds (> 8191)"));
        return false;
    }
    if (state->x > 4095) {
        Serial.println(F("[integrity] Register X out of bounds (> 4095)"));
        return false;
    }
    if (state->y > 4095) {
        Serial.println(F("[integrity] Register Y out of bounds (> 4095)"));
        return false;
    }
    if (state->a > 15) {
        Serial.println(F("[integrity] Register A out of bounds (> 15)"));
        return false;
    }
    if (state->b > 15) {
        Serial.println(F("[integrity] Register B out of bounds (> 15)"));
        return false;
    }
    if (state->np > 31) {
        Serial.println(F("[integrity] Register NP out of bounds (> 31)"));
        return false;
    }
    if (state->flags > 15) {
        Serial.println(F("[integrity] Flags out of bounds (> 15)"));
        return false;
    }
    if (state->call_depth > 100) {
        Serial.println(F("[integrity] Call depth unreasonable (> 100)"));
        return false;
    }

    // 2. Interrupts checks
    for (int i = 0; i < 6; i++) {
        if (state->interrupts[i].factor_flag_reg > 15 || 
            state->interrupts[i].mask_reg > 15 ||
            state->interrupts[i].triggered > 1) {
            Serial.println(F("[integrity] Interrupt state registers corrupt"));
            return false;
        }
    }

    // 3. Memory clock bounds checks
    if (state->memory == nullptr) {
        Serial.println(F("[integrity] Memory pointer is null"));
        return false;
    }

    // Check minutes tens/ones BCD digits
    if (state->memory[32] > 9) {
        Serial.println(F("[integrity] Clock minutes ones digit corrupt (> 9)"));
        return false;
    }
    if (state->memory[33] > 5) {
        Serial.println(F("[integrity] Clock minutes tens digit corrupt (> 5)"));
        return false;
    }

    // Check hours tens/ones BCD digits and overall range (0 to 12)
    int hours = state->memory[37] * 10 + state->memory[36];
    if (hours > 12) {
        Serial.print(F("[integrity] Clock hours out of valid 0-12 range: "));
        Serial.println(hours);
        return false;
    }
    if (state->memory[36] > 9) {
        Serial.println(F("[integrity] Clock hours ones digit corrupt (> 9)"));
        return false;
    }
    if (state->memory[37] > 1) {
        Serial.println(F("[integrity] Clock hours tens digit corrupt (> 1)"));
        return false;
    }

    // Check PM flag
    if (state->memory[38] > 1) {
        Serial.println(F("[integrity] Clock PM flag corrupt (> 1)"));
        return false;
    }

    return true;
}

#define EEPROM_MAX_SIZE (sizeof(SaveHeader) + sizeof(cpu_state_t) + MEMORY_SIZE)

void initEEPROM()
{
#if defined(ESP8266) || defined(ESP32)
    EEPROM.begin(EEPROM_MAX_SIZE);
#endif
}

bool validEEPROM()
{
    uint8_t magic = EEPROM.read(0);
    return (magic == EEPROM_MAGIC_LEGACY || magic == EEPROM_MAGIC_NEW);
}

void eraseStateFromEEPROM() {
    for (uint32_t i = 0; i < EEPROM.length(); i++) {
        EEPROM.write(i, 0);
    }
#if defined(ESP8266) || defined(ESP32)
    EEPROM.commit();
#endif
    Serial.println(F("EEPROM erased completely."));
}

bool loadStateFromEEPROM(cpu_state_t* cpuState)
{
    if (!validEEPROM()) {
        Serial.println(F("EEPROM magic number invalid."));
        return false;
    }

    uint8_t magic = EEPROM.read(0);
    cpu_get_state(cpuState);
    u4_t *memTemp = cpuState->memory; // Keep track of current memory pointer

    if (magic == EEPROM_MAGIC_LEGACY) {
        Serial.println(F("Legacy save format detected. Loading..."));
        
        // Load legacy format
        EEPROM.get(1, *cpuState);
        for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
            memTemp[i] = EEPROM.read(1 + sizeof(cpu_state_t) + i);
        }
        cpuState->memory = memTemp; // Restore local heap memory pointer

        // Validate state
        if (!isStateValid(cpuState)) {
            Serial.println(F("Legacy state failed validation! Discarding."));
            eraseStateFromEEPROM();
            return false;
        }

        Serial.println(F("Legacy state loaded successfully. Migrating to new checksummed format..."));
        cpu_set_state(cpuState);
        saveStateToEEPROM(cpuState); // Migrates to new layout
        return true;
    } 
    else if (magic == EEPROM_MAGIC_NEW) {
        Serial.println(F("New versioned/checksummed save format detected."));
        
        // Read header
        SaveHeader header;
        EEPROM.get(0, header);

        // Verify version and size
        if (header.version != 1) {
            Serial.print(F("Unsupported save version: "));
            Serial.println(header.version);
            return false;
        }
        if (header.state_size != sizeof(cpu_state_t)) {
            Serial.print(F("State size mismatch: EEPROM size "));
            Serial.print(header.state_size);
            Serial.print(F(", current code size "));
            Serial.println(sizeof(cpu_state_t));
            // In the future, we could add version migration code here if needed.
            return false;
        }
        if (header.memory_size != MEMORY_SIZE) {
            Serial.print(F("Memory size mismatch: EEPROM size "));
            Serial.print(header.memory_size);
            Serial.print(F(", current code size "));
            Serial.println(MEMORY_SIZE);
            return false;
        }

        // Load state and memory temp
        cpu_state_t loadedState;
        EEPROM.get(sizeof(SaveHeader), loadedState);
        
        // Allocate temp buffer to check memory checksum before overwriting current memory
        u4_t tempMemory[MEMORY_SIZE];
        for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
            tempMemory[i] = EEPROM.read(sizeof(SaveHeader) + sizeof(cpu_state_t) + i);
        }

        // Calculate and verify checksum
        uint16_t calculatedChk = calculateStateChecksum(&loadedState, tempMemory, MEMORY_SIZE);
        if (calculatedChk != header.checksum) {
            Serial.print(F("EEPROM checksum mismatch! Calculated: 0x"));
            Serial.print(calculatedChk, HEX);
            Serial.print(F(", Expected: 0x"));
            Serial.println(header.checksum, HEX);
            eraseStateFromEEPROM();
            return false;
        }

        // Perform bounds checks
        loadedState.memory = tempMemory; // Temporarily point loadedState's memory to our temp buffer for validation
        if (!isStateValid(&loadedState)) {
            Serial.println(F("Loaded state failed integrity validation! Discarding."));
            eraseStateFromEEPROM();
            return false;
        }

        // Copy valid data to target state
        *cpuState = loadedState;
        cpuState->memory = memTemp; // Restore local pointer
        
        // Copy validated memory data to active emulator memory
        for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
            memTemp[i] = tempMemory[i];
        }

        cpu_set_state(cpuState);

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START    
        Serial.print(F("Loaded "));
        Serial.print(EEPROM_MAX_SIZE);
        Serial.println(F(" bytes"));
#endif
        return true;
    }

    return false;
}

void saveStateToEEPROM(cpu_state_t* cpuState)
{
    cpu_get_state(cpuState);

    // Calculate checksum
    uint16_t chk = calculateStateChecksum(cpuState, cpuState->memory, MEMORY_SIZE);

    // Prepare header
    SaveHeader header;
    header.magic = EEPROM_MAGIC_NEW;
    header.version = 1;
    header.state_size = sizeof(cpu_state_t);
    header.memory_size = MEMORY_SIZE;
    header.checksum = chk;

    // Write header
    EEPROM.put(0, header);

    // Write cpu state
    EEPROM.put(sizeof(SaveHeader), *cpuState);

    // Write memory
    for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
        uint32_t addr = sizeof(SaveHeader) + sizeof(cpu_state_t) + i;
#if defined(ESP8266) || defined(ESP32)
        EEPROM.write(addr, cpuState->memory[i]);
#else
        EEPROM.update(addr, cpuState->memory[i]);
#endif
    }

#if defined(ESP8266) || defined(ESP32)
    EEPROM.commit();
#endif

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START    
    Serial.print(F("Saved "));
    Serial.print(EEPROM_MAX_SIZE);
    Serial.print(F(" bytes (Checksum: 0x"));
    Serial.print(chk, HEX);
    Serial.println(F(")"));
#endif
}

void loadHardcodedState(cpu_state_t* cpuState)
{
  cpu_get_state(cpuState);
  u4_t *memTemp = cpuState->memory;
  uint16_t i;
  uint8_t *cpuS = (uint8_t *)cpuState;
  for (i = 0; i < sizeof(cpu_state_t); i++)
  {
    cpuS[i] = pgm_read_byte_near(hardcodedState + i);
  }
  for (i = 0; i < MEMORY_SIZE; i++)
  {
    memTemp[i] = pgm_read_byte_near(hardcodedState + sizeof(cpu_state_t) + i);
  }
  cpuState->memory = memTemp;
  cpu_set_state(cpuState);
  Serial.println("Hardcoded");
}
