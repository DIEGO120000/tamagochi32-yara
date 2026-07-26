#ifndef CONFIG_FIX_H
#define CONFIG_FIX_H

/* Archivo de cabecera único para centralizar configuraciones y compatibilidad */

/* ==========================================
 * 1. Configuraciones Globales y Constantes
 * ========================================== */

// Serial baudrate
#ifndef SERIAL_BAUD
#define SERIAL_BAUD 74880
#endif

// Dimensiones de la pantalla OLED
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 128
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 64
#endif

// Nivel de voltaje cuando se presiona un botón
#ifndef BUTTON_VOLTAGE_LEVEL_PRESSED
#define BUTTON_VOLTAGE_LEVEL_PRESSED LOW
#endif

// Frecuencia por defecto para la nota C4 (262 Hz)
#ifndef NOTE_C4
#define NOTE_C4 262
#endif

/* ==========================================
 * 2. Lógica Específica de Arquitectura (ESP32 vs Otras)
 * ========================================== */

#if defined(ESP32)

  // Divisor de velocidad para emulación en ESP32
  #ifndef SPEED_DIVIDER
  #define SPEED_DIVIDER 2
  #endif

  // Framerate de la pantalla emulada
  #ifndef TAMA_DISPLAY_FRAMERATE
  #define TAMA_DISPLAY_FRAMERATE 8
  #endif

  // Intervalo de Deep Sleep (10 * 60 = 600 segundos = 10 minutos)
  #ifndef DEEPSLEEP_INTERVAL
  #define DEEPSLEEP_INTERVAL 600
  #endif

  // Activa el Deep Sleep
  #ifndef ENABLE_DEEPSLEEP
  #define ENABLE_DEEPSLEEP 1
  #endif

#else // Nano / ESP8266 / Otras arquitecturas

  #ifndef SPEED_DIVIDER
  #define SPEED_DIVIDER 1
  #endif

  #ifndef TAMA_DISPLAY_FRAMERATE
  #define TAMA_DISPLAY_FRAMERATE 3
  #endif

#endif

// Intervalo de auto guardado (en minutos)
#ifndef AUTO_SAVE_MINUTES
#define AUTO_SAVE_MINUTES 2
#endif

// Macros de funcionalidad
#ifndef ENABLE_AUTO_SAVE_STATUS
#define ENABLE_AUTO_SAVE_STATUS
#endif

#ifndef ENABLE_LOAD_STATE_FROM_EEPROM
#define ENABLE_LOAD_STATE_FROM_EEPROM
#endif

#ifndef ENABLE_TAMA_SOUND
#define ENABLE_TAMA_SOUND
#endif

#ifndef ENABLE_SERIAL_DEBUG_INPUT
#define ENABLE_SERIAL_DEBUG_INPUT
#endif

/* ==========================================
 * 3. Prototipos de Funciones (EEPROM / Hardware)
 * ========================================== */

// Incluimos los tipos necesarios para las funciones de savestate y hardware
#include "hal_types.h"
#include "cpu.h"
#include "hw.h"

#ifdef __cplusplus
// Funciones de EEPROM / Save State
void initEEPROM(void);
bool validEEPROM(void);
bool loadStateFromEEPROM(cpu_state_t* cpuState);
void eraseStateFromEEPROM(void);
void saveStateToEEPROM(cpu_state_t* cpuState);
void loadHardcodedState(cpu_state_t* cpuState);
#endif

#endif // CONFIG_FIX_H
