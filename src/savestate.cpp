#include <Arduino.h>
#include <EEPROM.h>
#include "cpu.h"
#include "savestate.h"
#include "hardcoded_state.h"
#include "config_fix.h"

// Calculate Fletcher16 checksum over the cpu_state_t and the memory array
uint16_t calculateStateChecksum(const cpu_state_t* state, const u4_t* memory_data, uint16_t memory_len) {
    // Zero-initialize to ensure all padding bytes are strictly 0
    cpu_state_t tempState;
    memset(&tempState, 0, sizeof(cpu_state_t));

    // Copy all fields manually to prevent uninitialized padding garbage
    tempState.pc = state->pc;
    tempState.x = state->x;
    tempState.y = state->y;
    tempState.a = state->a;
    tempState.b = state->b;
    tempState.np = state->np;
    tempState.sp = state->sp;
    tempState.flags = state->flags;
    tempState.tick_counter = state->tick_counter;
    tempState.clk_timer_timestamp = state->clk_timer_timestamp;
    tempState.prog_timer_timestamp = state->prog_timer_timestamp;
    tempState.prog_timer_enabled = state->prog_timer_enabled;
    tempState.prog_timer_data = state->prog_timer_data;
    tempState.prog_timer_rld = state->prog_timer_rld;
    tempState.call_depth = state->call_depth;
    tempState.memory = nullptr; // Zero volatile pointer
    memcpy(tempState.interrupts, state->interrupts, sizeof(tempState.interrupts));

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
    uint8_t min_ones = get_ram_nibble(state->memory, 64);
    uint8_t min_tens = get_ram_nibble(state->memory, 66);
    if (min_ones > 9) {
        Serial.print(F("[integrity] Clock minutes ones digit corrupt (> 9): "));
        Serial.println(min_ones);
        return false;
    }
    if (min_tens > 5) {
        Serial.print(F("[integrity] Clock minutes tens digit corrupt (> 5): "));
        Serial.println(min_tens);
        return false;
    }

    // Check hours tens/ones BCD digits and overall range (0 to 12)
    uint8_t hour_ones = get_ram_nibble(state->memory, 72);
    uint8_t hour_tens = get_ram_nibble(state->memory, 74);
    int hours = hour_tens * 10 + hour_ones;
    if (hours > 12) {
        Serial.print(F("[integrity] Clock hours out of valid 0-12 range: "));
        Serial.println(hours);
        return false;
    }
    if (hour_ones > 9) {
        Serial.println(F("[integrity] Clock hours ones digit corrupt (> 9)"));
        return false;
    }
    if (hour_tens > 1) {
        Serial.println(F("[integrity] Clock hours tens digit corrupt (> 1)"));
        return false;
    }

    // Check PM flag
    uint8_t pm_flag = get_ram_nibble(state->memory, 76);
    if (pm_flag > 1) {
        Serial.println(F("[integrity] Clock PM flag corrupt (> 1)"));
        return false;
    }

    return true;
}

#define EEPROM_MAX_SIZE (sizeof(SaveHeader) + sizeof(cpu_state_t) + MEMORY_SIZE)
#define SLOT_A_ADDR 0
#define SLOT_B_ADDR (EEPROM_MAX_SIZE + 16)
#define EEPROM_TOTAL_SIZE (SLOT_B_ADDR + EEPROM_MAX_SIZE)

void initEEPROM()
{
#if defined(ESP8266) || defined(ESP32)
    EEPROM.begin(EEPROM_TOTAL_SIZE);
#endif
}

bool validEEPROM()
{
    uint8_t magic = EEPROM.read(0);
    return (magic == EEPROM_MAGIC_LEGACY || magic == EEPROM_MAGIC_NEW);
}

void eraseStateFromEEPROM() {
    for (uint32_t i = 0; i < EEPROM_TOTAL_SIZE; i++) {
        EEPROM.write(i, 0);
    }
#if defined(ESP8266) || defined(ESP32)
    EEPROM.commit();
#endif
    Serial.println(F("EEPROM erased completely."));
}

void writeStateToAddr(uint32_t startAddr, cpu_state_t* cpuState)
{
    cpu_get_state(cpuState);
    uint16_t chk = calculateStateChecksum(cpuState, cpuState->memory, MEMORY_SIZE);

    // Prepare header
    SaveHeader header;
    header.magic = EEPROM_MAGIC_NEW;
    header.version = 1;
    header.state_size = sizeof(cpu_state_t);
    header.memory_size = MEMORY_SIZE;
    header.checksum = chk;

    // Write header
    EEPROM.put(startAddr, header);

    // Write cpu state
    EEPROM.put(startAddr + sizeof(SaveHeader), *cpuState);

    // Write memory
    for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
        uint32_t addr = startAddr + sizeof(SaveHeader) + sizeof(cpu_state_t) + i;
#if defined(ESP8266) || defined(ESP32)
        EEPROM.write(addr, cpuState->memory[i]);
#else
        EEPROM.update(addr, cpuState->memory[i]);
#endif
    }
}

bool loadStateFromAddr(uint32_t startAddr, cpu_state_t* cpuState)
{
    uint8_t magic = EEPROM.read(startAddr);
    if (magic != EEPROM_MAGIC_NEW) {
        return false;
    }

    cpu_get_state(cpuState);
    u4_t *memTemp = cpuState->memory; // Keep track of current memory pointer

    // Read header
    SaveHeader header;
    EEPROM.get(startAddr, header);

    // Verify version and size
    if (header.version != 1 || header.state_size != sizeof(cpu_state_t) || header.memory_size != MEMORY_SIZE) {
        return false;
    }

    // Load state and memory temp
    cpu_state_t loadedState;
    EEPROM.get(startAddr + sizeof(SaveHeader), loadedState);
    
    // Allocate temp buffer to check memory checksum before overwriting current memory
    u4_t tempMemory[MEMORY_SIZE];
    for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
        tempMemory[i] = EEPROM.read(startAddr + sizeof(SaveHeader) + sizeof(cpu_state_t) + i);
    }

    // Calculate and verify checksum
    uint16_t calculatedChk = calculateStateChecksum(&loadedState, tempMemory, MEMORY_SIZE);
    if (calculatedChk != header.checksum) {
        return false;
    }

    // Perform bounds checks
    loadedState.memory = tempMemory; // Temporarily point loadedState's memory to our temp buffer for validation
    if (!isStateValid(&loadedState)) {
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
    return true;
}

bool loadStateFromEEPROM(cpu_state_t* cpuState)
{
    // 1. Check legacy format first
    uint8_t magic = EEPROM.read(0);
    if (magic == EEPROM_MAGIC_LEGACY) {
        Serial.println(F("Legacy save format detected. Loading..."));
        cpu_get_state(cpuState);
        u4_t *memTemp = cpuState->memory;
        EEPROM.get(1, *cpuState);
        for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
            memTemp[i] = EEPROM.read(1 + sizeof(cpu_state_t) + i);
        }
        cpuState->memory = memTemp;

        if (!isStateValid(cpuState)) {
            Serial.println(F("Legacy state failed validation! Discarding."));
            eraseStateFromEEPROM();
            return false;
        }

        Serial.println(F("Legacy state loaded successfully. Migrating to new double-buffered format..."));
        cpu_set_state(cpuState);
        saveStateToEEPROM(cpuState); // Migrates to Slot A and Slot B
        return true;
    }

    // 2. Try Slot A (Primary)
    Serial.println(F("Trying to load state from Slot A (Primary)..."));
    if (loadStateFromAddr(SLOT_A_ADDR, cpuState)) {
        Serial.println(F("Slot A loaded successfully."));
        return true;
    }

    // 3. Try Slot B (Backup)
    Serial.println(F("Slot A failed! Trying to load state from Slot B (Backup)..."));
    if (loadStateFromAddr(SLOT_B_ADDR, cpuState)) {
        Serial.println(F("Slot B loaded successfully. Recovering Slot A..."));
        writeStateToAddr(SLOT_A_ADDR, cpuState);
#if defined(ESP8266) || defined(ESP32)
        EEPROM.commit();
#endif
        return true;
    }

    // 4. Both failed!
    Serial.println(F("Both save slots failed checksum or validation! Erasing EEPROM..."));
    eraseStateFromEEPROM();
    return false;
}

void saveStateToEEPROM(cpu_state_t* cpuState)
{
    cpu_get_state(cpuState);
    uint16_t chk = calculateStateChecksum(cpuState, cpuState->memory, MEMORY_SIZE);

    // Read Slot A header to check if state hasn't changed
    SaveHeader slotAHeader;
    EEPROM.get(SLOT_A_ADDR, slotAHeader);
    if (slotAHeader.magic == EEPROM_MAGIC_NEW && 
        slotAHeader.version == 1 &&
        slotAHeader.state_size == sizeof(cpu_state_t) &&
        slotAHeader.memory_size == MEMORY_SIZE &&
        slotAHeader.checksum == chk) {
#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START
        Serial.println(F("Save skipped: State unchanged (checksum match)."));
#endif
        return;
    }

    // Write to Slot A first
    writeStateToAddr(SLOT_A_ADDR, cpuState);
#if defined(ESP8266) || defined(ESP32)
    EEPROM.commit();
#endif

    // Write to Slot B second
    writeStateToAddr(SLOT_B_ADDR, cpuState);
#if defined(ESP8266) || defined(ESP32)
    EEPROM.commit();
#endif

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START
    Serial.print(F("Saved successfully to Slot A and Slot B (Checksum: 0x"));
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
