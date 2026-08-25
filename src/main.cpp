/*
 * ArduinoGotchi - A real Tamagotchi emulator for Arduino ESP32
 *
 * Copyright (C) 2022 Gary Kwok - Arduino Uno Implementation
 * Copyright (C) 2022 Marcel Ochsendorf - ESP32 Plattform Support
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <U8g2lib.h>
#include <Wire.h>

#include "tamalib.h"
#include "hw.h"
#include "bitmaps.h"
#include "savestate.h"
#include <EEPROM.h>

#include "config_fix.h"
#ifndef PROGMEM
#define PROGMEM
#endif
#include "hpbd.h"

#if defined(ESP32)
#include "soc/soc_caps.h"
#if SOC_WIFI_SUPPORTED
#include <WiFi.h>
#endif
#include <esp_bt.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#endif


// ==========================================================================
//  Temporizador Pomodoro Settings & State
// ==========================================================================
enum PomodoroScreenState {
  POMO_MAIN,
  POMO_CONF_WORK,
  POMO_CONF_SHORT,
  POMO_CONF_LONG
};

static bool pomodoro_screen_active = false;
static PomodoroScreenState pomo_screen_state = POMO_MAIN;

static bool pomo_configured = false;
static bool pomo_active = false; // Timer running or paused
static uint8_t pomo_phase = 0;   // 0-7: Work1, ShortBreak1, Work2, ShortBreak2, Work3, ShortBreak3, Work4, LongBreak
static uint32_t pomo_seconds_left = 0;

static uint8_t pomo_work_time = 25;       // minutes
static uint8_t pomo_short_break = 5;      // minutes
static uint8_t pomo_long_break = 15;      // minutes

static uint8_t pomo_temp_work = 25;
static uint8_t pomo_temp_short = 5;
static uint8_t pomo_temp_long = 15;

static bool pomo_alert_active = false;
static char pomo_alert_msg1[40] = "";
static char pomo_alert_msg2[40] = "";

static unsigned long last_beep_time = 0;
static uint8_t beep_substep = 0;

#define EEPROM_MAX_SIZE (sizeof(SaveHeader) + sizeof(cpu_state_t) + 0x140) // 0x140 is MEMORY_SIZE

void savePomodoroConfig(uint8_t work, uint8_t s_break, uint8_t l_break);
bool loadPomodoroConfig(uint8_t &work, uint8_t &s_break, uint8_t &l_break);
void savePomodoroState();
void loadPomodoroState();
uint32_t get_pomo_phase_duration_seconds(uint8_t phase);
const char* get_pomo_phase_name(uint8_t phase);
void run_pomo_alarm();
void update_pomodoro_timer();

/***** Set display orientation, U8G2_MIRROR_VERTICAL is not supported *****/
#define U8G2_LAYOUT_NORMAL
// #define U8G2_LAYOUT_ROTATE_180
// #define U8G2_LAYOUT_MIRROR
/**************************************************************************/

#ifdef U8G2_LAYOUT_NORMAL
U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_R0);
#endif

#ifdef U8G2_LAYOUT_ROTATE_180
U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_R2);
#endif

#ifdef U8G2_LAYOUT_MIRROR
U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_MIRROR);
#endif

static bool display_enabled = false;

// Variables y prototipos para la máquina de estados de inactividad (Modo Sleep)
#define SLEEP_TIMEOUT_MS 60000 // 60 segundos de inactividad para suspender la pantalla
static bool screen_sleeping = false;
static unsigned long last_activity_time = 0;
static bool battery_screen_active = false;
static unsigned long battery_screen_start_time = 0;
static int battery_current_pct = 0;
static float battery_current_voltage = 0.0f;

static cpu_state_t cpuState;
static unsigned long lastSaveTimestamp = 0;

void wake_up_screen();
void go_to_sleep();
void measure_battery();


enum EstadoTamagotchi {
  CONFIGURACION,
  JUEGO
};
static EstadoTamagotchi estadoActual = CONFIGURACION;

#if defined(ESP32)
// Buttons mapping for ESP32-H2 SuperMini
#define PIN_BTN_L 11   // K1 (Select / Left) -> Botón 2 (GPIO 11)
#define PIN_BTN_M 12   // K2 (Confirm / Middle) -> Botón 3 (GPIO 12)
#define PIN_BTN_R 13   // K3 (Cancel / Right) -> Botón 4 (GPIO 13)
#define PIN_BTN_4 10   // K5 (Decrement / Fourth Button) -> Botón 1 (GPIO 10)
#define PIN_BTN_RST 5  // K4 (Reset) (GPIO 5)
#define PIN_BUZZER 3   // Buzzer (GPIO 3)
#define PIN_SDA 22     // OLED SDA (GPIO 22)
#define PIN_SCL 25     // OLED SCL (GPIO 25)
#define PIN_BATTERY 1  // Battery ADC (GPIO 1)
#define BUZZER_CHANNEL 0
#define TONE_CHANNEL 0
#elif defined(ESP8266)
#define PIN_BTN_L 12
#define PIN_BTN_M 13
#define PIN_BTN_R 15
#define PIN_BUZZER 2
#else
#define PIN_BTN_L 2
#define PIN_BTN_M 3
#define PIN_BTN_R 4
#define PIN_BUZZER 9
#endif

#if defined(ESP32)
static bool esp32_buzzer_active = false;

void esp32_noTone(uint8_t pin, uint8_t channel)
{
  if (esp32_buzzer_active) {
    noTone(pin);
    esp32_buzzer_active = false;
  }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

void esp32_tone(uint8_t pin, unsigned int frequency, unsigned long duration, uint8_t channel)
{
  tone(pin, frequency);
  esp32_buzzer_active = true;
}
#endif

void triggerEasterEgg1() {
  Serial.println(F("[EasterEgg] Running Easter Egg 1: Diseño de Interfaz"));
  
  if (display_enabled) {
    // Usamos el bucle de página oficial para el constructor de página _2_
    display.firstPage();
    do {
      // 1. Texto superior izquierdo: "holaa brooo" (fuente pequeña 5x7 en posición 0, 7)
      display.setFont(u8g2_font_5x7_tf);
      display.drawStr(0, 7, "holaa brooo");
      
      // 2. Texto central principal: "see you" (fuente helvB10 centrado)
      display.setFont(u8g2_font_helvB10_tf);
      int width1 = display.getStrWidth("see you");
      display.drawStr((128 - width1) / 2, 30, "see you");
      
      // 3. Texto inferior: "soon" (fuente helvB10 centrado)
      int width2 = display.getStrWidth("soon");
      display.drawStr((128 - width2) / 2, 47, "soon");
      
      // 4. Texto inferior derecho: "sjsjs(maybe)" (fuente pequeña 5x7)
      display.setFont(u8g2_font_5x7_tf);
      int width3 = display.getStrWidth("sjsjs(maybe)");
      display.drawStr(128 - width3 - 2, 62, "sjsjs(maybe)");
    } while (display.nextPage());
    
    // Hacer sonar el buzzer con la melodía festiva (manteniendo el comportamiento de audio)
#if defined(ESP32)
    esp32_tone(PIN_BUZZER, 523, 200, TONE_CHANNEL); // C5
    delay(200);
    esp32_tone(PIN_BUZZER, 659, 200, TONE_CHANNEL); // E5
    delay(200);
    esp32_tone(PIN_BUZZER, 784, 200, TONE_CHANNEL); // G5
    delay(200);
    esp32_tone(PIN_BUZZER, 1047, 400, TONE_CHANNEL); // C6
    delay(400);
    esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
#endif
    
    // Mantener expuesto en pantalla por 5 segundos en total (1s de audio + 4s adicionales)
    delay(4000);
  }
  last_activity_time = millis();
}

#define image_83f04b_width 40
#define image_83f04b_height 40

static const unsigned char image_83f04b_bits[] U8X8_PROGMEM = {
0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0xF8, 0x80, 0x07, 0x00, 
0x00, 0xE2, 0xE7, 0x27, 0x00, 
0x80, 0x07, 0x00, 0xF8, 0x00, 
0x80, 0x83, 0xFF, 0x60, 0x00, 
0x00, 0xF0, 0xFF, 0x07, 0x00, 
0x78, 0xF8, 0xFF, 0x07, 0x0F, 
0x7C, 0xFC, 0xFC, 0x9F, 0x1F, 
0x7E, 0xFE, 0xFC, 0x9F, 0x3F, 
0x07, 0xFE, 0xFC, 0x1F, 0x70, 
0x7F, 0xFE, 0x1C, 0x1F, 0x7F, 
0x7F, 0xFE, 0xFC, 0x9F, 0x7F, 
0x7C, 0xDE, 0xFF, 0x9D, 0x1F, 
0xF8, 0x3C, 0x00, 0x9E, 0x07, 
0x00, 0x78, 0xF5, 0x0F, 0x00, 
0x00, 0x70, 0xF7, 0x27, 0x00, 
0x80, 0xE1, 0xF8, 0xE3, 0x00, 
0xC0, 0x81, 0xFF, 0xE0, 0x01, 
0xE0, 0x07, 0x00, 0xD8, 0x03, 
0x60, 0x3E, 0x00, 0xBE, 0x03, 
0x60, 0xFE, 0xF7, 0xBF, 0x03, 
0xE0, 0xFF, 0x80, 0xFF, 0x03, 
0x80, 0x0F, 0x00, 0xF8, 0x00, 
0x00, 0x00, 0x06, 0x00, 0x00, 
0x00, 0x00, 0x0F, 0x00, 0x00, 
0x00, 0x00, 0x0F, 0x00, 0x00, 
0x00, 0x80, 0x07, 0x00, 0x00, 
0x00, 0x80, 0x03, 0x00, 0x00, 
0x00, 0x80, 0x03, 0x00, 0x00, 
0x00, 0x00, 0x07, 0x00, 0x00, 
0x00, 0x00, 0x0F, 0x00, 0x00, 
0x00, 0x00, 0x1E, 0x00, 0x00, 
0x00, 0x00, 0x1C, 0x00, 0x00, 
0x00, 0x00, 0x38, 0x00, 0x00, 
0x00, 0x00, 0x7C, 0x00, 0x00, 
0x00, 0x20, 0x7E, 0x02, 0x00, 
0x00, 0x20, 0x7F, 0x02, 0x00, 
0x00, 0x40, 0x3E, 0x01, 0x00, 
0x00, 0x80, 0x80, 0x00, 0x00, 
0x00, 0x00, 0x7F, 0x00, 0x00, 
};

void triggerEasterEgg2() {
  Serial.println(F("[EasterEgg] Running Easter Egg 2: yara + flowey"));
  
  if (display_enabled) {
    // Usamos el bucle de página oficial para el constructor de página _2_
    display.firstPage();
    do {
      // 1. Dibujar texto "yara" centrado arriba
      display.setFont(u8g2_font_inb21_mf);
      int text_x = (128 - display.getStrWidth("yara")) / 2;
      int text_y = 21;
      display.drawStr(text_x, text_y, "yara");

      // 2. Dibujar la flor Flowey (40x40 px) centrada abajo
      int flower_x = (128 - image_83f04b_width) / 2;
      int flower_y = 32;
      display.drawXBMP(flower_x, flower_y, image_83f04b_width, image_83f04b_height, image_83f04b_bits);
    } while (display.nextPage());
    
    // Hacer sonar el buzzer con la melodía festiva (manteniendo el comportamiento de audio)
#if defined(ESP32)
    esp32_tone(PIN_BUZZER, 523, 200, TONE_CHANNEL); // C5
    delay(200);
    esp32_tone(PIN_BUZZER, 659, 200, TONE_CHANNEL); // E5
    delay(200);
    esp32_tone(PIN_BUZZER, 784, 200, TONE_CHANNEL); // G5
    delay(200);
    esp32_tone(PIN_BUZZER, 1047, 400, TONE_CHANNEL); // C6
    delay(400);
    esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
#endif
    
    // Mantener expuesto en pantalla por 5 segundos en total (1s de audio + 4s adicionales)
    delay(4000);
  }
  last_activity_time = millis();
}

#define BITMAP3_WIDTH 40
#define BITMAP3_HEIGHT 40

static const unsigned char bitmap3_data[] U8X8_PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x81, 0x00, 0x00, 0x00, 0x01, 0xC3, 0x80, 0x00, 0x00, 0x01, 0xC3,
    0x80, 0x00, 0x00, 0x01, 0xF3, 0xF0, 0x00, 0x00, 0x01, 0xF3, 0xF0, 0x00,
    0x00, 0x01, 0xFF, 0xF0, 0x00, 0x00, 0x01, 0xFF, 0xF0, 0x00, 0x00, 0x01,
    0xFF, 0xF0, 0x00, 0x00, 0x01, 0xCE, 0x70, 0x00, 0x00, 0x01, 0xC4, 0x70,
    0x00, 0x00, 0x01, 0xFF, 0xF0, 0x00, 0x00, 0x01, 0xFF, 0xF0, 0x00, 0x00,
    0x01, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0xFF, 0x80, 0x00, 0x00, 0x01, 0xFF, 0x80, 0x00,
    0x00, 0x01, 0xFF, 0x80, 0x00, 0x00, 0x01, 0xFF, 0x80, 0x00, 0x00, 0x01,
    0xFF, 0x80, 0x00, 0x00, 0x07, 0xFF, 0x80, 0x00, 0x00, 0x0F, 0xFF, 0x80,
    0x00, 0x00, 0x0F, 0xFF, 0x80, 0x00, 0x00, 0x0F, 0xFF, 0x80, 0x00, 0x00,
    0x0F, 0xFF, 0x80, 0x00, 0x07, 0x8F, 0xFF, 0x80, 0x00, 0x07, 0xCF, 0xFF,
    0x80, 0x00, 0x07, 0xCF, 0xFF, 0x80, 0x00, 0x07, 0x0F, 0xFF, 0x80, 0x00,
    0x06, 0x0F, 0xFF, 0x80, 0x00, 0x07, 0x6F, 0xFF, 0x80, 0x00, 0x07, 0xFF,
    0xFF, 0x80, 0x00, 0x07, 0xFF, 0xFF, 0x80, 0x00
};

void triggerEasterEgg3() {
  Serial.println(F("[EasterEgg] Running Easter Egg 3: Save me"));
  
  if (display_enabled) {
    display.firstPage();
    do {
      display.setFont(u8g2_font_5x7_tf);
      display.drawStr(0, 7, "Save me, because I");
      display.drawStr(0, 15, "might not be there,");
      display.drawStr(0, 23, "but I am here.");
      display.drawBitmap(44, 24, 5, 40, bitmap3_data);
    } while (display.nextPage());
    
    // Hacer sonar el buzzer con la melodía festiva (manteniendo el comportamiento de audio)
#if defined(ESP32)
    esp32_tone(PIN_BUZZER, 523, 200, TONE_CHANNEL); // C5
    delay(200);
    esp32_tone(PIN_BUZZER, 659, 200, TONE_CHANNEL); // E5
    delay(200);
    esp32_tone(PIN_BUZZER, 784, 200, TONE_CHANNEL); // G5
    delay(200);
    esp32_tone(PIN_BUZZER, 1047, 400, TONE_CHANNEL); // C6
    delay(400);
    esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
#endif
    
    // Mantener expuesto en pantalla por 5 segundos en total
    delay(4000);
  }
  last_activity_time = millis();
}

// ================== CẤU HÌNH CÁC NỐT NHẠC (Easter Egg 4) ==================
#define EGG4_NOTE_C4  262
#define EGG4_NOTE_D4  294
#define EGG4_NOTE_E4  330
#define EGG4_NOTE_F4  349
#define EGG4_NOTE_G4  392
#define EGG4_NOTE_A4  440
#define EGG4_NOTE_AS4 466

#define EGG4_NOTE_C5  523
#define EGG4_NOTE_D5  587
#define EGG4_NOTE_E5  659
#define EGG4_NOTE_F5  698
#define EGG4_NOTE_G5  784
#define EGG4_NOTE_A5  880
#define EGG4_NOTE_C6  1047

static const int melody_egg4[] PROGMEM = {
  // Câu 1: Hap-py Birth-day to You
  EGG4_NOTE_C4, 8, EGG4_NOTE_C4, 8, EGG4_NOTE_D4, 4, EGG4_NOTE_C4, 4, EGG4_NOTE_F4, 4, EGG4_NOTE_E4, 2,

  // Câu 2: Hap-py Birth-day to You
  EGG4_NOTE_C4, 8, EGG4_NOTE_C4, 8, EGG4_NOTE_D4, 4, EGG4_NOTE_C4, 4, EGG4_NOTE_G4, 4, EGG4_NOTE_F4, 2,

  // Câu 3: Hap-py Birth-day Dear Friend
  EGG4_NOTE_C4, 8, EGG4_NOTE_C4, 8, EGG4_NOTE_C5, 4, EGG4_NOTE_A4, 4, EGG4_NOTE_F4, 4, EGG4_NOTE_E4, 4, EGG4_NOTE_D4, 2,

  // Câu 4: Hap-py Birth-day to You
  EGG4_NOTE_AS4, 8, EGG4_NOTE_AS4, 8, EGG4_NOTE_A4, 4, EGG4_NOTE_F4, 4, EGG4_NOTE_G4, 4, EGG4_NOTE_F4, 2
};

static const uint8_t* const frames_egg4[] PROGMEM = {
  frame0, frame1, frame2, frame3, frame4, frame5, frame6, frame7, frame8, frame9,
  frame10, frame11, frame12, frame13, frame14, frame15, frame16, frame17, frame18, frame19,
  frame20, frame21, frame22, frame23, frame24, frame25, frame26, frame27, frame28, frame29,
  frame30, frame31, frame32, frame33, frame34, frame35, frame36, frame37, frame38, frame39,
  frame40, frame41, frame42, frame43, frame44, frame45, frame46, frame47, frame48, frame49,
  frame50, frame51, frame52, frame53, frame54, frame55, frame56, frame57, frame58, frame59,
  frame60, frame61, frame62, frame63, frame64, frame65, frame66, frame67, frame68, frame69,
  frame70, frame71, frame72, frame73, frame74, frame75, frame76, frame77, frame78, frame79,
  frame80, frame81, frame82, frame83, frame84, frame85, frame86, frame87, frame88, frame89,
  frame90, frame91, frame92, frame93, frame94, frame95, frame96, frame97, frame98, frame99,
  frame100, frame101, frame102, frame103, frame104, frame105, frame106, frame107, frame108, frame109,
  frame110, frame111, frame112, frame113, frame114, frame115, frame116, frame117, frame118, frame119,
  frame120, frame121
};

void triggerEasterEgg4() {
  Serial.println(F("[EasterEgg] Running Easter Egg 4: Happy Birthday Animation"));
  
  if (display_enabled) {
    // Play start sound
#if defined(ESP32)
    esp32_tone(PIN_BUZZER, EGG4_NOTE_E5, 100, TONE_CHANNEL);
    delay(120);
    esp32_tone(PIN_BUZZER, EGG4_NOTE_G5, 100, TONE_CHANNEL);
    delay(120);
    esp32_tone(PIN_BUZZER, EGG4_NOTE_C6, 250, TONE_CHANNEL);
    delay(300);
    esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
    delay(100);
#endif

    int noteIndex = 0;
    int notes = sizeof(melody_egg4) / sizeof(melody_egg4[0]) / 2;
    int tempo = 140;
    
    unsigned long noteStartMs = millis();
    int currentNoteDuration = 0;
    bool notePlaying = false;
    
    uint8_t frameIdx = 0;
    unsigned long lastFrameMs = millis();
    
    while (noteIndex < notes * 2 || notePlaying) {
      unsigned long now = millis();
      
      // Update Song Note
      if (!notePlaying && noteIndex < notes * 2) {
        int freq = pgm_read_word(&melody_egg4[noteIndex]);
        int divider = pgm_read_word(&melody_egg4[noteIndex + 1]);
        noteIndex += 2;
        
        if (divider > 0) {
          currentNoteDuration = (60000UL * 4) / tempo / divider;
        } else {
          currentNoteDuration = (60000UL * 4) / tempo / abs(divider);
          currentNoteDuration = currentNoteDuration * 1.5;
        }
        
#if defined(ESP32)
        if (freq > 0) {
          esp32_tone(PIN_BUZZER, freq, 0, TONE_CHANNEL);
        } else {
          esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
        }
#endif
        noteStartMs = now;
        notePlaying = true;
      }
      
      // Stop note after 70% duration (staccato)
      if (notePlaying) {
        unsigned long elapsed = now - noteStartMs;
        if (elapsed >= (unsigned long)currentNoteDuration) {
          notePlaying = false;
        } else if (elapsed >= (unsigned long)(currentNoteDuration * 0.7)) {
#if defined(ESP32)
          esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
#endif
        }
      }
      
      // Update Animation Frame (80ms interval)
      if (now - lastFrameMs >= 80) {
        lastFrameMs = now;
        
        display.firstPage();
        do {
          const uint8_t* ptr = (const uint8_t*)pgm_read_ptr(&frames_egg4[frameIdx]);
          display.drawBitmap(0, 0, 16, 64, ptr);
        } while (display.nextPage());
        
        frameIdx = (frameIdx + 1) % 122;
      }
      
      delay(5);
    }
    
#if defined(ESP32)
    esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
#endif
  }
  
  last_activity_time = millis();
}

void displayTama();

// ==========================================================================
//  Temporizador Pomodoro Helpers Implementations
// ==========================================================================
void savePomodoroConfig(uint8_t work, uint8_t s_break, uint8_t l_break) {
  uint8_t magic = EEPROM.read(EEPROM_MAX_SIZE);
  uint8_t w = EEPROM.read(EEPROM_MAX_SIZE + 1);
  uint8_t sb = EEPROM.read(EEPROM_MAX_SIZE + 2);
  uint8_t lb = EEPROM.read(EEPROM_MAX_SIZE + 3);
  if (magic == 0x55 && w == work && sb == s_break && lb == l_break) {
    return; // Configuración idéntica, saltar escritura
  }

  EEPROM.write(EEPROM_MAX_SIZE, 0x55); // Magic byte
  EEPROM.write(EEPROM_MAX_SIZE + 1, work);
  EEPROM.write(EEPROM_MAX_SIZE + 2, s_break);
  EEPROM.write(EEPROM_MAX_SIZE + 3, l_break);
  EEPROM.commit();
}

bool loadPomodoroConfig(uint8_t &work, uint8_t &s_break, uint8_t &l_break) {
  uint8_t magic = EEPROM.read(EEPROM_MAX_SIZE);
  if (magic == 0x55) {
    uint8_t w = EEPROM.read(EEPROM_MAX_SIZE + 1);
    uint8_t sb = EEPROM.read(EEPROM_MAX_SIZE + 2);
    uint8_t lb = EEPROM.read(EEPROM_MAX_SIZE + 3);
    if (w > 0 && w < 60 && sb > 0 && sb < 60 && lb > 0 && lb < 60) {
      work = w;
      s_break = sb;
      l_break = lb;
      return true;
    }
  }
  return false;
}

void savePomodoroState() {
  if (!pomo_configured) return;
  uint8_t current_active_val = pomo_active ? 1 : 0;
  uint8_t current_phase_val = pomo_phase;
  uint32_t current_seconds_left_val = pomo_seconds_left;

  uint8_t stored_active_val = EEPROM.read(EEPROM_MAX_SIZE + 4);
  uint8_t stored_phase_val = EEPROM.read(EEPROM_MAX_SIZE + 5);
  uint32_t stored_seconds_left_val = 0;
  EEPROM.get(EEPROM_MAX_SIZE + 6, stored_seconds_left_val);

  if (stored_active_val == current_active_val &&
      stored_phase_val == current_phase_val &&
      stored_seconds_left_val == current_seconds_left_val) {
    return; // Estado de Pomodoro idéntico, saltar escritura
  }

  EEPROM.write(EEPROM_MAX_SIZE + 4, current_active_val);
  EEPROM.write(EEPROM_MAX_SIZE + 5, current_phase_val);
  EEPROM.put(EEPROM_MAX_SIZE + 6, current_seconds_left_val);
  EEPROM.commit();
  Serial.print(F("[Pomodoro] Estado de ejecucion guardado: active="));
  Serial.print(pomo_active);
  Serial.print(F(", phase="));
  Serial.print(pomo_phase);
  Serial.print(F(", seconds_left="));
  Serial.println(pomo_seconds_left);
}

void loadPomodoroState() {
  if (!pomo_configured) return;
  uint8_t active = EEPROM.read(EEPROM_MAX_SIZE + 4);
  if (active == 0 || active == 1) {
    pomo_active = (active == 1);
  } else {
    pomo_active = false;
  }
  
  uint8_t phase = EEPROM.read(EEPROM_MAX_SIZE + 5);
  if (phase < 8) {
    pomo_phase = phase;
  } else {
    pomo_phase = 0;
  }
  
  uint32_t sec_left = 0;
  EEPROM.get(EEPROM_MAX_SIZE + 6, sec_left);
  uint32_t max_dur = get_pomo_phase_duration_seconds(pomo_phase);
  if (sec_left <= max_dur) {
    pomo_seconds_left = sec_left;
  } else {
    pomo_seconds_left = max_dur;
  }
  
  Serial.print(F("[Pomodoro] Estado de ejecucion cargado: active="));
  Serial.print(pomo_active);
  Serial.print(F(", phase="));
  Serial.print(pomo_phase);
  Serial.print(F(", seconds_left="));
  Serial.println(pomo_seconds_left);
}

uint32_t get_pomo_phase_duration_seconds(uint8_t phase) {
  if (phase % 2 == 0) {
    return (uint32_t)pomo_work_time * 60;
  } else if (phase == 7) {
    return (uint32_t)pomo_long_break * 60;
  } else {
    return (uint32_t)pomo_short_break * 60;
  }
}

const char* get_pomo_phase_name(uint8_t phase) {
  switch(phase) {
    case 0: return "Trabajo 1/4";
    case 1: return "Descanso Corto 1";
    case 2: return "Trabajo 2/4";
    case 3: return "Descanso Corto 2";
    case 4: return "Trabajo 3/4";
    case 5: return "Descanso Corto 3";
    case 6: return "Trabajo 4/4";
    case 7: return "Descanso Largo";
    default: return "";
  }
}

void run_pomo_alarm() {
  if (!pomo_alert_active) {
    return;
  }
  
  unsigned long now = millis();
  // Casio beep cycle:
  // 4 fast beeps (50ms ON, 50ms OFF), then a longer silence (500ms) before repeating.
  // 8 steps:
  // Step 0: beep 1 ON (50ms)
  // Step 1: beep 1 OFF (50ms)
  // Step 2: beep 2 ON (50ms)
  // Step 3: beep 2 OFF (50ms)
  // Step 4: beep 3 ON (50ms)
  // Step 5: beep 3 OFF (50ms)
  // Step 6: beep 4 ON (50ms)
  // Step 7: beep 4 OFF (500ms)
  uint32_t step_durations[] = { 50, 50, 50, 50, 50, 50, 50, 500 };
  uint32_t duration = step_durations[beep_substep];
  
  if (now - last_beep_time >= duration) {
    beep_substep = (beep_substep + 1) % 8;
    last_beep_time = now;
    if (beep_substep % 2 == 0) {
      // Even steps are sound (using 4096 Hz to match the loudest Tamagotchi tone, which is also an authentic Casio alarm frequency)
      esp32_tone(PIN_BUZZER, 4096, step_durations[beep_substep], TONE_CHANNEL);
    } else {
      // Odd steps are silence
      esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
    }
  }
}

void update_pomodoro_timer() {
  if (!pomo_configured) {
    return;
  }
  
  if (pomo_active) {
    if (pomo_seconds_left > 0) {
      pomo_seconds_left--;
      if (pomo_seconds_left % 60 == 0) {
        savePomodoroState();
      }
    }
    
    if (pomo_seconds_left == 0) {
      uint8_t old_phase = pomo_phase;
      pomo_phase = (pomo_phase + 1) % 8;
      pomo_seconds_left = get_pomo_phase_duration_seconds(pomo_phase);
      savePomodoroState();
      
      if (old_phase % 2 == 0) {
        if (pomo_phase == 7) {
          strcpy(pomo_alert_msg1, "Se acabo el trabajo.");
          strcpy(pomo_alert_msg2, "Empieza descanso largo");
        } else {
          strcpy(pomo_alert_msg1, "Se acabo el trabajo.");
          strcpy(pomo_alert_msg2, "Empieza descanso corto");
        }
      } else {
        strcpy(pomo_alert_msg1, "Se acabo el descanso.");
        strcpy(pomo_alert_msg2, "Empieza el trabajo!");
      }
      
      pomo_alert_active = true;
      beep_substep = 0;
      last_beep_time = millis();
      esp32_tone(PIN_BUZZER, 4096, 50, TONE_CHANNEL); // Start the first beep immediately
      
      if (screen_sleeping) {
        wake_up_screen();
      }
      displayTama();
      
      Serial.print(F("[Pomodoro] Transitioned from phase "));
      Serial.print(old_phase);
      Serial.print(F(" to "));
      Serial.println(pomo_phase);
    } else {
      if (pomodoro_screen_active && !pomo_alert_active && !screen_sleeping) {
        displayTama();
      }
    }
  }
}

void wake_up_screen() {
  if (screen_sleeping) {
    screen_sleeping = false;
    if (display_enabled) {
      display.setPowerSave(0); // Encender pantalla (despertar panel OLED)
      displayTama();           // Redibujar inmediatamente
    }
    Serial.println(F("[SleepMode] Pantalla encendida (Wake-up)"));
  }
  last_activity_time = millis();
}

void go_to_sleep() {
  if (!screen_sleeping) {
#ifdef ENABLE_AUTO_SAVE_STATUS
    saveStateToEEPROM(&cpuState);
    savePomodoroState();
#endif
    screen_sleeping = true;
    if (display_enabled) {
      display.setPowerSave(1); // Apagar pantalla (bajo consumo)
    }
    Serial.println(F("[SleepMode] Pantalla apagada por inactividad (Sleep)"));
  }
}
static timestamp_t hal_get_timestamp(void);


/**** TamaLib Specific Variables ****/
static uint16_t current_freq = 0;
static bool_t matrix_buffer[LCD_HEIGHT][LCD_WIDTH / 8] = {{0}};
// static byte runOnceBool = 0;
static bool_t icon_buffer[ICON_NUM] = {0};
static long last_interaction = 0;
/************************************/

static void hal_halt(void)
{
  // Serial.println("Halt!");
}

static void hal_log(log_level_t level, char *buff, ...)
{
  Serial.println(buff);
}

static void hal_sleep_until(timestamp_t ts)
{
  // Deep sleep deshabilitado para evitar interferencia con botones de configuración
  (void)ts;
}

static timestamp_t hal_get_timestamp(void)
{
  return millis() * (1000 / SPEED_DIVIDER);
}

static void hal_update_screen(void)
{
  displayTama();
}

static void hal_set_lcd_matrix(u8_t x, u8_t y, bool_t val)
{
  uint8_t mask;
  if (val)
  {
    mask = 0b10000000 >> (x % 8);
    matrix_buffer[y][x / 8] = matrix_buffer[y][x / 8] | mask;
  }
  else
  {
    mask = 0b01111111;
    for (byte i = 0; i < (x % 8); i++)
    {
      mask = (mask >> 1) | 0b10000000;
    }
    matrix_buffer[y][x / 8] = matrix_buffer[y][x / 8] & mask;
  }
}

static void hal_set_lcd_icon(u8_t icon, bool_t val)
{
  icon_buffer[icon] = val;
}

static void hal_set_frequency(u32_t freq)
{
  current_freq = freq;
}

static void hal_play_frequency(bool_t en)
{
#ifdef ENABLE_TAMA_SOUND
  // Si la alarma de Pomodoro está activa, ignoramos los sonidos del Tamagotchi
  // para evitar que interfieran con la señal y provoquen un tono distorsionado ("con gripe").
  if (pomo_alert_active) {
    return;
  }

  if (en)
  {
    wake_up_screen();

#if defined(ESP32)
    esp32_tone(PIN_BUZZER, current_freq, 500, BUZZER_CHANNEL);
#else
    tone(PIN_BUZZER, current_freq);
#endif
  }
  else
  {
#if defined(ESP32)
    esp32_noTone(PIN_BUZZER, BUZZER_CHANNEL);
#else
    noTone(PIN_BUZZER);
#ifdef ENABLE_TAMA_SOUND_ACTIVE_LOW
    digitalWrite(PIN_BUZZER, HIGH);
#endif
#endif
  }
#endif
}

// Debounce de 50ms real para eliminar saltos de números y permitir clics consecutivos rápidos
#define BTN_DEBOUNCE_MS 50

// Sequence player: reproduce secuencias de botones virtuales hacia la ROM
static int seq_state = 0;       // 0 = idle
static unsigned long seq_time = 0;
static int seq_type = 0;        // 1 = enter config, 2 = save/exit config

void start_sequence(int type) {
  seq_type = type;
  seq_state = 1;
  seq_time = millis();
}

// ==========================================================================
//  Lógica de Ignorancia Forzada (Escudo 200ms) y Auto-Repetición para Reloj
// ==========================================================================
static bool btn26_wasPressed = false;
static bool btn27_wasPressed = false;
static bool btn26_ignoreNextPulse = false;
static bool btn27_ignoreNextPulse = false;
static unsigned long btn26_lastPulseTime = 0;
static unsigned long btn27_lastPulseTime = 0;
static unsigned long btn26_pressStartTime = 0;
static unsigned long btn27_pressStartTime = 0;
static unsigned long btn26_lastRepeatTime = 0;
static unsigned long btn27_lastRepeatTime = 0;
static bool btn26_inAutoRepeat = false;
static bool btn27_inAutoRepeat = false;

static bool getButtonPress(int pin) {
  unsigned long now = millis();
  bool current = (digitalRead(pin) == BUTTON_VOLTAGE_LEVEL_PRESSED);

  if (pin == PIN_BTN_M) {
    if (btn26_ignoreNextPulse && (now - btn26_lastPulseTime >= 200)) {
      btn26_ignoreNextPulse = false;
    }

    if (current) {
      if (btn26_ignoreNextPulse) {
        return false; // Ignora cualquier señal de 'presionado' durante los 200ms
      }

      if (!btn26_wasPressed) {
        btn26_wasPressed = true;
        btn26_pressStartTime = now;
        btn26_lastRepeatTime = now;
        btn26_inAutoRepeat = false;
        
        // Activar escudo
        btn26_ignoreNextPulse = true;
        btn26_lastPulseTime = now;
        return true; // Registra pulsación
      } else {
        // Auto-Repetición
        unsigned long holdDuration = now - btn26_pressStartTime;
        if (holdDuration >= 2000) {
          if (!btn26_inAutoRepeat) {
            btn26_inAutoRepeat = true;
            btn26_lastRepeatTime = now;
            return true;
          } else {
            if (now - btn26_lastRepeatTime >= 150) {
              btn26_lastRepeatTime = now;
              return true;
            }
          }
        }
      }
    } else {
      btn26_wasPressed = false;
    }
  }
  else if (pin == PIN_BTN_R) {
    if (btn27_ignoreNextPulse && (now - btn27_lastPulseTime >= 200)) {
      btn27_ignoreNextPulse = false;
    }

    if (current) {
      if (btn27_ignoreNextPulse) {
        return false; // Ignora cualquier señal de 'presionado' durante los 200ms
      }

      if (!btn27_wasPressed) {
        btn27_wasPressed = true;
        btn27_pressStartTime = now;
        btn27_lastRepeatTime = now;
        btn27_inAutoRepeat = false;
        
        // Activar escudo
        btn27_ignoreNextPulse = true;
        btn27_lastPulseTime = now;
        return true; // Registra pulsación
      } else {
        // Auto-Repetición
        unsigned long holdDuration = now - btn27_pressStartTime;
        if (holdDuration >= 2000) {
          if (!btn27_inAutoRepeat) {
            btn27_inAutoRepeat = true;
            btn27_lastRepeatTime = now;
            return true;
          } else {
            if (now - btn27_lastRepeatTime >= 150) {
              btn27_lastRepeatTime = now;
              return true;
            }
          }
        }
      }
    } else {
      btn27_wasPressed = false;
    }
  }

  return false;
}

// ==========================================================================
//  hal_handler - Máquina de Estados de Control Nativo P1
// ==========================================================================
static int hal_handler(void)
{
  // Flancos anteriores para detección de rising edge
  static bool prev_l = false;  // GPIO 25
  static bool prev_m = false;  // GPIO 26
  static bool prev_r = false;  // GPIO 27
  static bool prev_4 = false;  // GPIO 33

  // Debounce por pin
  static bool debounced_l = false;
  static bool debounced_m = false;
  static bool debounced_r = false;
  static bool debounced_4 = false;
  static unsigned long last_time_l = 0;
  static unsigned long last_time_m = 0;
  static unsigned long last_time_r = 0;
  static unsigned long last_time_4 = 0;

  unsigned long now = millis();

  // Lectura cruda
  bool raw_l = (digitalRead(PIN_BTN_L) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 25
  bool raw_m = (digitalRead(PIN_BTN_M) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 26
  bool raw_r = (digitalRead(PIN_BTN_R) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 27
  bool raw_4 = (digitalRead(PIN_BTN_4) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 33

  // Registrar actividad por pulsación física
  if (raw_l || raw_m || raw_r || raw_4) {
    if (screen_sleeping) {
      wake_up_screen();
    } else {
      last_activity_time = millis();
    }
  }

  // Debounce: cambiar estado solo si han pasado BTN_DEBOUNCE_MS desde el último cambio
  if (raw_l != debounced_l && (now - last_time_l >= BTN_DEBOUNCE_MS)) {
    debounced_l = raw_l;
    last_time_l = now;
  }
  if (raw_m != debounced_m && (now - last_time_m >= BTN_DEBOUNCE_MS)) {
    debounced_m = raw_m;
    last_time_m = now;
  }
  if (raw_r != debounced_r && (now - last_time_r >= BTN_DEBOUNCE_MS)) {
    debounced_r = raw_r;
    last_time_r = now;
  }
  if (raw_4 != debounced_4 && (now - last_time_4 >= BTN_DEBOUNCE_MS)) {
    debounced_4 = raw_4;
    last_time_4 = now;
  }

  bool btn_l = debounced_l;  // GPIO 25
  bool btn_m = debounced_m;  // GPIO 26
  bool btn_r = debounced_r;  // GPIO 27
  bool btn_4 = debounced_4;  // GPIO 33

  // Detección de combinación simultánea: Botón 3 (GPIO 27) y Botón 4 (GPIO 33)
  if (btn_r && btn_4) {
    if (!battery_screen_active) {
      measure_battery(); // Tomar la lectura analógica una sola vez al entrar en la pantalla
      battery_screen_active = true;
      battery_screen_start_time = millis();
      displayTama();
    }
    prev_l = btn_l; prev_m = btn_m; prev_r = btn_r; prev_4 = btn_4;
    return 0;
  }

  // Detección de flancos ascendentes (pulsación)
  bool pressed_l = btn_l && !prev_l;
  bool pressed_m = btn_m && !prev_m;
  bool pressed_r = btn_r && !prev_r;
  bool pressed_4 = btn_4 && !prev_4;

  // ------------------------------------------------------------------
  //  Secuencia secreta de formateo de EEPROM y reseteo de partida:
  //  Orden: 1 -> 2 -> 3 -> 4 -> 4 -> 3 -> 2 -> 1 en un tiempo <= 5 seg
  // ------------------------------------------------------------------
  static int reset_seq_step = 0;
  static unsigned long reset_seq_start_time = 0;

  if (pressed_4 || pressed_l || pressed_m || pressed_r) {
    unsigned long current_time = millis();

    if (reset_seq_step > 0 && (current_time - reset_seq_start_time > 5000)) {
      reset_seq_step = 0;
    }

    if (reset_seq_step == 0) {
      if (pressed_4) { // Botón 1 (GPIO 10)
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
        Serial.println(F("[RESET_SEQ] Paso 1: Boton 1 pulsado"));
      }
    } else if (reset_seq_step == 1) {
      if (pressed_l) { // Botón 2 (GPIO 11)
        reset_seq_step = 2;
        Serial.println(F("[RESET_SEQ] Paso 2: Boton 2 pulsado"));
      } else if (pressed_4) {
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
      } else {
        reset_seq_step = 0;
      }
    } else if (reset_seq_step == 2) {
      if (pressed_m) { // Botón 3 (GPIO 12)
        reset_seq_step = 3;
        Serial.println(F("[RESET_SEQ] Paso 3: Boton 3 pulsado"));
      } else if (pressed_4) {
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
      } else {
        reset_seq_step = 0;
      }
    } else if (reset_seq_step == 3) {
      if (pressed_r) { // Botón 4 (GPIO 13)
        reset_seq_step = 4;
        Serial.println(F("[RESET_SEQ] Paso 4: Boton 4 pulsado"));
      } else if (pressed_4) {
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
      } else {
        reset_seq_step = 0;
      }
    } else if (reset_seq_step == 4) {
      if (pressed_r) { // Botón 4 (GPIO 13)
        reset_seq_step = 5;
        Serial.println(F("[RESET_SEQ] Paso 5: Boton 4 pulsado"));
      } else if (pressed_4) {
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
      } else {
        reset_seq_step = 0;
      }
    } else if (reset_seq_step == 5) {
      if (pressed_m) { // Botón 3 (GPIO 12)
        reset_seq_step = 6;
        Serial.println(F("[RESET_SEQ] Paso 6: Boton 3 pulsado"));
      } else if (pressed_4) {
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
      } else {
        reset_seq_step = 0;
      }
    } else if (reset_seq_step == 6) {
      if (pressed_l) { // Botón 2 (GPIO 11)
        reset_seq_step = 7;
        Serial.println(F("[RESET_SEQ] Paso 7: Boton 2 pulsado"));
      } else if (pressed_4) {
        reset_seq_step = 1;
        reset_seq_start_time = current_time;
      } else {
        reset_seq_step = 0;
      }
    } else if (reset_seq_step == 7) {
      if (pressed_4) { // Botón 1 (GPIO 10)
        reset_seq_step = 0;
        Serial.println(F("[RESET_SEQ] Secuencia de reseteo completada (1->2->3->4->4->3->2->1 en <= 5s)"));
        Serial.println(F("[RESET_SEQ] Formateando EEPROM y reiniciando sistema a huevo limpio..."));
        eraseStateFromEEPROM();
#if defined(ESP32)
        ESP.restart();
#else
        tamalib_reset();
        estadoActual = CONFIGURACION;
        wake_up_screen();
        displayTama();
#endif
        return 0;
      } else {
        reset_seq_step = 0;
      }
    }
  }

  // ------------------------------------------------------------------
  //  Manejo de Alerta/Notificación Pomodoro (Interrupción Prioritaria)
  // ------------------------------------------------------------------
  if (pomo_alert_active) {
    if (pressed_l || pressed_m || pressed_r || pressed_4) {
      pomo_alert_active = false;
      esp32_noTone(PIN_BUZZER, TONE_CHANNEL);
      displayTama(); // Redibujar pantalla inmediatamente para quitar aviso
    }
    prev_l = btn_l; prev_m = btn_m; prev_r = btn_r; prev_4 = btn_4;
    return 0;
  }

  // ------------------------------------------------------------------
  //  Manejo de Pantallas Pomodoro (Activo)
  // ------------------------------------------------------------------
  if (pomodoro_screen_active) {
    // GPIO 33 (btn_4): Salir de Pomodoro y volver a Tamagotchi normal
    if (pressed_4) {
      pomodoro_screen_active = false;
      displayTama(); // Redibujar Tamagotchi
      prev_l = btn_l; prev_m = btn_m; prev_r = btn_r; prev_4 = btn_4;
      return 0;
    }

    if (pomo_screen_state == POMO_CONF_WORK) {
      if (getButtonPress(PIN_BTN_R)) {
        pomo_temp_work++;
        if (pomo_temp_work > 59) pomo_temp_work = 1;
        displayTama();
      }
      if (getButtonPress(PIN_BTN_M)) {
        pomo_temp_work--;
        if (pomo_temp_work < 1) pomo_temp_work = 59;
        displayTama();
      }
      if (pressed_l) {
        pomo_screen_state = POMO_CONF_SHORT;
        displayTama();
      }
    }
    else if (pomo_screen_state == POMO_CONF_SHORT) {
      if (getButtonPress(PIN_BTN_R)) {
        pomo_temp_short++;
        if (pomo_temp_short > 59) pomo_temp_short = 1;
        displayTama();
      }
      if (getButtonPress(PIN_BTN_M)) {
        pomo_temp_short--;
        if (pomo_temp_short < 1) pomo_temp_short = 59;
        displayTama();
      }
      if (pressed_l) {
        pomo_screen_state = POMO_CONF_LONG;
        displayTama();
      }
    }
    else if (pomo_screen_state == POMO_CONF_LONG) {
      if (getButtonPress(PIN_BTN_R)) {
        pomo_temp_long++;
        if (pomo_temp_long > 59) pomo_temp_long = 1;
        displayTama();
      }
      if (getButtonPress(PIN_BTN_M)) {
        pomo_temp_long--;
        if (pomo_temp_long < 1) pomo_temp_long = 59;
        displayTama();
      }
      if (pressed_l) {
        pomo_work_time = pomo_temp_work;
        pomo_short_break = pomo_temp_short;
        pomo_long_break = pomo_temp_long;
        pomo_configured = true;
        savePomodoroConfig(pomo_work_time, pomo_short_break, pomo_long_break);
        
        // Reiniciar estado del temporizador
        pomo_phase = 0;
        pomo_seconds_left = get_pomo_phase_duration_seconds(0);
        pomo_active = false;
        savePomodoroState(); // Guardamos el estado reiniciado
        
        pomo_screen_state = POMO_MAIN;
        displayTama();
      }
    }
    else {
      // Main Pomodoro Screen
      // GPIO 26 (pressed_m): Play / Pause
      if (pressed_m) {
        pomo_active = !pomo_active;
        savePomodoroState(); // Guardamos el estado al pausar/reanudar
        displayTama();
      }
      // GPIO 25 (pressed_l): Entrar a reconfiguración
      if (pressed_l) {
        pomo_temp_work = pomo_work_time;
        pomo_temp_short = pomo_short_break;
        pomo_temp_long = pomo_long_break;
        pomo_screen_state = POMO_CONF_WORK;
        displayTama();
      }
    }

    prev_l = btn_l; prev_m = btn_m; prev_r = btn_r; prev_4 = btn_4;
    return 0;
  }

  // GPIO 33 (btn_4): Activa Pomodoro desde el juego/estado normal
  if (pressed_4) {
    pomodoro_screen_active = true;
    if (!pomo_configured) {
      pomo_temp_work = 25;
      pomo_temp_short = 5;
      pomo_temp_long = 15;
      pomo_screen_state = POMO_CONF_WORK;
    } else {
      pomo_screen_state = POMO_MAIN;
    }
    displayTama();
    prev_l = btn_l; prev_m = btn_m; prev_r = btn_r; prev_4 = btn_4;
    return 0;
  }

  // Máquina de estados para secuencia de Easter Egg 1: GPIO27 -> GPIO25 -> GPIO27 -> GPIO26 (< 3s)
  static int seq_step = 0;
  static unsigned long seq_start_time = 0;

  // Máquina de estados para secuencia de Easter Egg 2: GPIO27 -> GPIO27 -> GPIO25 -> GPIO25 -> GPIO26 -> GPIO26 (< 5s)
  static int seq2_step = 0;
  static unsigned long seq2_start_time = 0;

  // Máquina de estados para secuencia de Easter Egg 3: GPIO26 -> GPIO26 -> GPIO25 -> GPIO25 -> GPIO27 -> GPIO27 -> GPIO26 (< 5s)
  static int seq3_step = 0;
  static unsigned long seq3_start_time = 0;

  // Máquina de estados para secuencia de Easter Egg 4: GPIO25 -> GPIO26 -> GPIO27 -> GPIO27 -> GPIO26 -> GPIO25 -> GPIO26 (< 5s)
  static int seq4_step = 0;
  static unsigned long seq4_start_time = 0;

  if (pressed_l || pressed_m || pressed_r) {
    unsigned long current_time = millis();
    
    // Reset de tiempo para secuencia 1
    if (seq_step > 0 && (current_time - seq_start_time > 3000)) {
      seq_step = 0;
    }
    // Reset de tiempo para secuencia 2
    if (seq2_step > 0 && (current_time - seq2_start_time > 5000)) {
      seq2_step = 0;
    }
    // Reset de tiempo para secuencia 3
    if (seq3_step > 0 && (current_time - seq3_start_time > 5000)) {
      seq3_step = 0;
    }
    // Reset de tiempo para secuencia 4
    if (seq4_step > 0 && (current_time - seq4_start_time > 5000)) {
      seq4_step = 0;
    }

    // --- Máquina para Secuencia 1 ---
    if (seq_step == 0) {
      if (pressed_r) { // GPIO 27
        seq_step = 1;
        seq_start_time = current_time;
        Serial.println(F("[EasterEgg1] Seq step 1: GPIO27 pressed"));
      }
    } else if (seq_step == 1) {
      if (pressed_l) { // GPIO 25
        seq_step = 2;
        Serial.println(F("[EasterEgg1] Seq step 2: GPIO25 pressed"));
      } else {
        seq_step = pressed_r ? 1 : 0;
        if (seq_step == 1) seq_start_time = current_time;
      }
    } else if (seq_step == 2) {
      if (pressed_r) { // GPIO 27
        seq_step = 3;
        Serial.println(F("[EasterEgg1] Seq step 3: GPIO27 pressed"));
      } else {
        seq_step = 0;
      }
    } else if (seq_step == 3) {
      if (pressed_m) { // GPIO 26
        seq_step = 0;
        Serial.println(F("[EasterEgg1] Seq complete! Activating Easter Egg 1"));
        triggerEasterEgg1();
        // Limpiamos los flancos detectados y debounces
        debounced_l = raw_l = false;
        debounced_m = raw_m = false;
        debounced_r = raw_r = false;
        btn_l = btn_m = btn_r = false;
        pressed_l = pressed_m = pressed_r = false;
      } else {
        seq_step = pressed_r ? 1 : 0;
        if (seq_step == 1) seq_start_time = current_time;
      }
    }

    // --- Máquina para Secuencia 2 ---
    if (seq2_step == 0) {
      if (pressed_r) { // GPIO 27
        seq2_step = 1;
        seq2_start_time = current_time;
        Serial.println(F("[EasterEgg2] Seq step 1: GPIO27 pressed"));
      }
    } else if (seq2_step == 1) {
      if (pressed_r) { // GPIO 27
        seq2_step = 2;
        Serial.println(F("[EasterEgg2] Seq step 2: GPIO27 pressed"));
      } else if (pressed_l || pressed_m) {
        seq2_step = 0;
      }
    } else if (seq2_step == 2) {
      if (pressed_l) { // GPIO 25
        seq2_step = 3;
        Serial.println(F("[EasterEgg2] Seq step 3: GPIO25 pressed"));
      } else if (pressed_r) {
        seq2_step = 2;
        seq2_start_time = current_time;
      } else {
        seq2_step = 0;
      }
    } else if (seq2_step == 3) {
      if (pressed_l) { // GPIO 25
        seq2_step = 4;
        Serial.println(F("[EasterEgg2] Seq step 4: GPIO25 pressed"));
      } else {
        seq2_step = pressed_r ? 1 : 0;
        if (seq2_step == 1) seq2_start_time = current_time;
      }
    } else if (seq2_step == 4) {
      if (pressed_m) { // GPIO 26
        seq2_step = 5;
        Serial.println(F("[EasterEgg2] Seq step 5: GPIO26 pressed"));
      } else {
        seq2_step = pressed_r ? 1 : 0;
        if (seq2_step == 1) seq2_start_time = current_time;
      }
    } else if (seq2_step == 5) {
      if (pressed_m) { // GPIO 26
        seq2_step = 0;
        Serial.println(F("[EasterEgg2] Seq complete! Activating Easter Egg 2"));
        triggerEasterEgg2();
        // Limpiamos los flancos detectados y debounces
        debounced_l = raw_l = false;
        debounced_m = raw_m = false;
        debounced_r = raw_r = false;
        btn_l = btn_m = btn_r = false;
        pressed_l = pressed_m = pressed_r = false;
      } else {
        seq2_step = pressed_r ? 1 : 0;
        if (seq2_step == 1) seq2_start_time = current_time;
      }
    }

    // --- Máquina para Secuencia 3 ---
    if (seq3_step == 0) {
      if (pressed_m) { // GPIO 26
        seq3_step = 1;
        seq3_start_time = current_time;
        Serial.println(F("[EasterEgg3] Seq step 1: GPIO26 pressed"));
      }
    } else if (seq3_step == 1) {
      if (pressed_m) { // GPIO 26
        seq3_step = 2;
        Serial.println(F("[EasterEgg3] Seq step 2: GPIO26 pressed"));
      } else if (pressed_l || pressed_r) {
        seq3_step = 0;
      }
    } else if (seq3_step == 2) {
      if (pressed_l) { // GPIO 25
        seq3_step = 3;
        Serial.println(F("[EasterEgg3] Seq step 3: GPIO25 pressed"));
      } else if (pressed_m) {
        seq3_step = 2;
        seq3_start_time = current_time;
      } else {
        seq3_step = 0;
      }
    } else if (seq3_step == 3) {
      if (pressed_l) { // GPIO 25
        seq3_step = 4;
        Serial.println(F("[EasterEgg3] Seq step 4: GPIO25 pressed"));
      } else {
        seq3_step = pressed_m ? 1 : 0;
        if (seq3_step == 1) seq3_start_time = current_time;
      }
    } else if (seq3_step == 4) {
      if (pressed_r) { // GPIO 27
        seq3_step = 5;
        Serial.println(F("[EasterEgg3] Seq step 5: GPIO27 pressed"));
      } else {
        seq3_step = pressed_m ? 1 : 0;
        if (seq3_step == 1) seq3_start_time = current_time;
      }
    } else if (seq3_step == 5) {
      if (pressed_r) { // GPIO 27
        seq3_step = 6;
        Serial.println(F("[EasterEgg3] Seq step 6: GPIO27 pressed"));
      } else {
        seq3_step = pressed_m ? 1 : 0;
        if (seq3_step == 1) seq3_start_time = current_time;
      }
    } else if (seq3_step == 6) {
      if (pressed_m) { // GPIO 26
        seq3_step = 0;
        Serial.println(F("[EasterEgg3] Seq complete! Activating Easter Egg 3"));
        triggerEasterEgg3();
        // Limpiamos los flancos detectados y debounces
        debounced_l = raw_l = false;
        debounced_m = raw_m = false;
        debounced_r = raw_r = false;
        btn_l = btn_m = btn_r = false;
        pressed_l = pressed_m = pressed_r = false;
      } else {
        seq3_step = pressed_m ? 1 : 0;
        if (seq3_step == 1) seq3_start_time = current_time;
      }
    }

    // --- Máquina para Secuencia 4 ---
    if (seq4_step == 0) {
      if (pressed_l) { // GPIO 25
        seq4_step = 1;
        seq4_start_time = current_time;
        Serial.println(F("[EasterEgg4] Seq step 1: GPIO25 pressed"));
      }
    } else if (seq4_step == 1) {
      if (pressed_m) { // GPIO 26
        seq4_step = 2;
        Serial.println(F("[EasterEgg4] Seq step 2: GPIO26 pressed"));
      } else if (pressed_l) {
        seq4_step = 1;
        seq4_start_time = current_time;
      } else {
        seq4_step = 0;
      }
    } else if (seq4_step == 2) {
      if (pressed_r) { // GPIO 27
        seq4_step = 3;
        Serial.println(F("[EasterEgg4] Seq step 3: GPIO27 pressed"));
      } else if (pressed_l) {
        seq4_step = 1;
        seq4_start_time = current_time;
      } else {
        seq4_step = 0;
      }
    } else if (seq4_step == 3) {
      if (pressed_r) { // GPIO 27
        seq4_step = 4;
        Serial.println(F("[EasterEgg4] Seq step 4: GPIO27 pressed"));
      } else if (pressed_l) {
        seq4_step = 1;
        seq4_start_time = current_time;
      } else {
        seq4_step = 0;
      }
    } else if (seq4_step == 4) {
      if (pressed_m) { // GPIO 26
        seq4_step = 5;
        Serial.println(F("[EasterEgg4] Seq step 5: GPIO26 pressed"));
      } else if (pressed_l) {
        seq4_step = 1;
        seq4_start_time = current_time;
      } else {
        seq4_step = 0;
      }
    } else if (seq4_step == 5) {
      if (pressed_l) { // GPIO 25
        seq4_step = 6;
        Serial.println(F("[EasterEgg4] Seq step 6: GPIO25 pressed"));
      } else {
        seq4_step = 0;
      }
    } else if (seq4_step == 6) {
      if (pressed_m) { // GPIO 26
        seq4_step = 0;
        Serial.println(F("[EasterEgg4] Seq complete! Activating Easter Egg 4"));
        triggerEasterEgg4();
        // Limpiamos los flancos detectados y debounces
        debounced_l = raw_l = false;
        debounced_m = raw_m = false;
        debounced_r = raw_r = false;
        btn_l = btn_m = btn_r = false;
        pressed_l = pressed_m = pressed_r = false;
      } else if (pressed_l) {
        seq4_step = 1;
        seq4_start_time = current_time;
      } else {
        seq4_step = 0;
      }
    }
  }

  // ------------------------------------------------------------------
  // Sequence Player: reproduce pulsaciones virtuales hacia la ROM
  // mientras corre, NO se procesan inputs del usuario
  // ------------------------------------------------------------------
  if (seq_state > 0)
  {
    if (millis() - seq_time > 150) {
      seq_state++;
      seq_time = millis();
    }

    if (seq_type == 1) // Entrar al menú de reloj en la ROM
    {
      switch (seq_state) {
        case 1: hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);  break;
        case 2: hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED); break;
        case 3:
          hw_set_button(BTN_LEFT, BTN_STATE_PRESSED);
          hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
          break;
        case 4:
          hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);
          hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
          break;
        case 5: seq_state = 0; break;
      }
    }
    else if (seq_type == 2) // Salir del menú de reloj en la ROM
    {
      switch (seq_state) {
        case 1: hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);   break;
        case 2: hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);  break;
        case 3: hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);  break;
        case 4: hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED); break;
        case 5: seq_state = 0; break;
      }
    }

    prev_l = btn_l; prev_m = btn_m; prev_r = btn_r;
    return 0;
  }

  // ==================================================================
  //  SWITCH DE ESTADOS
  // ==================================================================
  switch (estadoActual)
  {
    case CONFIGURACION:
    {
      // Blindar al emulador: no pasa ninguna pulsación de juego
      hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);
      hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
      hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);

      // GPIO 27 (btn_r): AUMENTAR HORAS (con máquina de estados)
      if (getButtonPress(PIN_BTN_R))
      {
        cpu_get_state(&cpuState);
        if (cpuState.memory != nullptr)
        {
          int clock_hours = get_ram_nibble(cpuState.memory, 74) * 10 + get_ram_nibble(cpuState.memory, 72);
          bool clock_is_pm = get_ram_nibble(cpuState.memory, 76) == 1;

          int h24 = (clock_hours % 12) + (clock_is_pm ? 12 : 0);
          h24 = (h24 + 1) % 24;
          clock_hours = h24 % 12;
          if (clock_hours == 0) clock_hours = 12;
          clock_is_pm = (h24 >= 12);

          set_ram_nibble(cpuState.memory, 72, clock_hours % 10);
          set_ram_nibble(cpuState.memory, 74, clock_hours / 10);
          set_ram_nibble(cpuState.memory, 76, clock_is_pm ? 1 : 0);
          cpu_set_state(&cpuState);

          Serial.print(F("[P1 CONFIG] Horas -> "));
          Serial.print(clock_hours);
          Serial.println(clock_is_pm ? " PM" : " AM");
        }
      }

      // GPIO 26 (btn_m): AUMENTAR MINUTOS (con máquina de estados)
      if (getButtonPress(PIN_BTN_M))
      {
        cpu_get_state(&cpuState);
        if (cpuState.memory != nullptr)
        {
          int clock_minutes = get_ram_nibble(cpuState.memory, 66) * 10 + get_ram_nibble(cpuState.memory, 64);
          clock_minutes = (clock_minutes + 1) % 60;
          set_ram_nibble(cpuState.memory, 64, clock_minutes % 10);
          set_ram_nibble(cpuState.memory, 66, clock_minutes / 10);
          cpu_set_state(&cpuState);

          Serial.print(F("[P1 CONFIG] Minutos -> "));
          Serial.println(clock_minutes);
        }
      }

      // GPIO 25 (btn_l): ACEPTAR - Setea la hora y sale al Modo Juego
      if (btn_l && !prev_l)
      {
        cpu_get_state(&cpuState);
        cpu_set_state(&cpuState);
        cpu_sync_ref_timestamp();

        if (cpuState.memory != nullptr) {
          int saved_h = get_ram_nibble(cpuState.memory, 74) * 10 + get_ram_nibble(cpuState.memory, 72);
          int saved_m = get_ram_nibble(cpuState.memory, 66) * 10 + get_ram_nibble(cpuState.memory, 64);
          bool saved_pm = get_ram_nibble(cpuState.memory, 76) == 1;
          Serial.print(F("[P1 CONFIG] Hora final guardada: "));
          Serial.print(saved_h);
          Serial.print(":");
          if (saved_m < 10) Serial.print("0");
          Serial.print(saved_m);
          Serial.println(saved_pm ? " PM" : " AM");
        }

        saveStateToEEPROM(&cpuState);
        EEPROM.commit();
        Serial.println(F("[P1 CONFIG] Estado guardado en EEPROM. Cambiando a Modo JUEGO."));

        estadoActual = JUEGO;
        start_sequence(2); // Salir del menú de reloj en la ROM
      }
      break;
    }

    case JUEGO:
    {
      // Reset variables de configuración para el siguiente ingreso
      btn26_wasPressed = false;
      btn26_ignoreNextPulse = false;
      btn26_lastPulseTime = 0;
      btn26_inAutoRepeat = false;
      btn27_wasPressed = false;
      btn27_ignoreNextPulse = false;
      btn27_lastPulseTime = 0;
      btn27_inAutoRepeat = false;

      // GPIO 27 (btn_r): A (BTN_LEFT) -> Navegar iconos de izquierda a derecha
      if (btn_r) hw_set_button(BTN_LEFT, BTN_STATE_PRESSED);
      else hw_set_button(BTN_LEFT, BTN_STATE_RELEASED);

      // GPIO 26 (btn_m): B (BTN_MIDDLE) -> Entrar / Seleccionar o Salir (confirmar)
      if (btn_m) hw_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
      else hw_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);

      // GPIO 25 (btn_l): C (BTN_RIGHT) -> Elegir opción (o cancelar/volver)
      if (btn_l) hw_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
      else hw_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
      break;
    }
  }

  prev_l = btn_l;
  prev_m = btn_m;
  prev_r = btn_r;
  prev_4 = btn_4;
  return 0;
}

static hal_t hal = {
    .halt = &hal_halt,
    .log = &hal_log,
    .sleep_until = &hal_sleep_until,
    .get_timestamp = &hal_get_timestamp,
    .update_screen = &hal_update_screen,
    .set_lcd_matrix = &hal_set_lcd_matrix,
    .set_lcd_icon = &hal_set_lcd_icon,
    .set_frequency = &hal_set_frequency,
    .play_frequency = &hal_play_frequency,
    .handler = &hal_handler,
};

void drawTriangle(uint8_t x, uint8_t y)
{
  // display.drawLine(x,y,x+6,y);
  display.drawLine(x + 1, y + 1, x + 5, y + 1);
  display.drawLine(x + 2, y + 2, x + 4, y + 2);
  display.drawLine(x + 3, y + 3, x + 3, y + 3);
}

void drawTamaRow(uint8_t tamaLCD_y, uint8_t ActualLCD_y, uint8_t thick)
{
  uint8_t i;
  for (i = 0; i < LCD_WIDTH; i++)
  {
    uint8_t mask = 0b10000000;
    mask = mask >> (i % 8);
    if ((matrix_buffer[tamaLCD_y][i / 8] & mask) != 0)
    {
      display.drawBox(i + i + i + 16, ActualLCD_y, 2, thick);
    }
  }
}

void drawTamaSelection(uint8_t y)
{
  uint8_t i;
  for (i = 0; i < 7; i++)
  {
    if (icon_buffer[i])
      drawTriangle(i * 16 + 5, y);
    display.drawXBMP(i * 16 + 4, y + 6, 16, 9, bitmaps + i * 18);
  }
  if (icon_buffer[7])
  {
    drawTriangle(7 * 16 + 5, y);
    display.drawXBMP(7 * 16 + 4, y + 6, 16, 9, bitmaps + 7 * 18);
  }
}

void measure_battery() {
  // 1. Leer voltaje real de la batería usando lectura analógica estándar de Arduino
  uint32_t sum_raw = 0;
  for (int i = 0; i < 50; i++) {
    sum_raw += analogRead(PIN_BATTERY);
    delay(1);
  }
  float raw_avg = sum_raw / 50.0;
  
  // Rango 0-3.3V (3300 mV) con resolución de 12 bits (4095)
  float mv_avg = (raw_avg / 4095.0) * 3300.0;
  
  // Multiplicador de 2.40 para compensar el divisor de tensión y el efecto de carga del ADC
  battery_current_voltage = (mv_avg / 1000.0) * 2.40;

  // Mapear voltaje a porcentaje de LiPo (3.3V a 4.2V)
  battery_current_pct = (int)((battery_current_voltage - 3.3) / (4.2 - 3.3) * 100.0);
  if (battery_current_pct > 100) battery_current_pct = 100;
  if (battery_current_pct < 0) battery_current_pct = 0;

  Serial.print(F("[BATTERY] Raw ADC: "));
  Serial.print(raw_avg);
  Serial.print(F(", Millivolts: "));
  Serial.print(mv_avg);
  Serial.print(F(", Calibrated Voltage: "));
  Serial.print(battery_current_voltage);
  Serial.print(F("V, Pct: "));
  Serial.print(battery_current_pct);
  Serial.println(F("%"));
}

void draw_battery_monitor_screen() {
  int pct = battery_current_pct;
  float voltage = battery_current_voltage;

  // 2. Calcular autonomía estimada en horas y minutos
  // Standby (2 mA) y Activo (40 mA) con batería de 1800 mAh
  uint32_t capacity_rem = (pct * 1800) / 100; // mAh restantes
  
  uint32_t active_mins = (capacity_rem * 60) / 40;
  uint32_t standby_mins = (capacity_rem * 60) / 2;

  // 3. Dibujar interfaz gráfica
  display.firstPage();
  do {
    // Dibujar marco general de pantalla
    display.drawFrame(0, 0, 128, 64);
    
    // Dibujar título "BATERIA TAMAGOCHI"
    display.setFont(u8g2_font_5x7_tf);
    display.drawStr(5, 10, "BATERIA TAMAGOCHI");
    display.drawHLine(5, 12, 118);

    // Dibujar icono de batería horizontal
    display.drawFrame(12, 20, 34, 18); // Cuerpo
    display.drawBox(46, 25, 3, 8);      // Polo +
    
    // Dibujar las 4 franjas internas (cada una representa 25%)
    if (pct >= 25) display.drawBox(15, 23, 5, 12);
    if (pct >= 50) display.drawBox(22, 23, 5, 12);
    if (pct >= 75) display.drawBox(29, 23, 5, 12);
    if (pct >= 100) display.drawBox(36, 23, 5, 12);

    // Dibujar porcentaje
    display.setFont(u8g2_font_helvB10_tf);
    char pct_buf[10];
    sprintf(pct_buf, "%d%%", pct);
    display.drawStr(60, 34, pct_buf);

    // Dibujar autonomía estimada
    display.setFont(u8g2_font_5x7_tf);
    char act_buf[35];
    char std_buf[35];
    sprintf(act_buf, "Uso Activo: ~%dh %02dm", active_mins / 60, active_mins % 60);
    sprintf(std_buf, "En Reposo : ~%dh %02dm", standby_mins / 60, standby_mins % 60);
    display.drawStr(12, 48, act_buf);
    display.drawStr(12, 58, std_buf);
  } while (display.nextPage());
}

void displayTama()
{
  if (!display_enabled) return;
  if (screen_sleeping) return;
  display.clearBuffer(); // Limpieza de búfer forzada para evitar basura en pantalla (cuadros blancos)

  if (battery_screen_active) {
    draw_battery_monitor_screen();
    return;
  }

  if (pomo_alert_active) {
    display.firstPage();
    do {
      // Draw alert screen
      display.drawFrame(5, 5, 118, 54);
      display.setFont(u8g2_font_helvB10_tf);
      const char* notif_str = "NOTIFICACION";
      int notif_width = display.getStrWidth(notif_str);
      int notif_x = 5 + (118 - notif_width) / 2; // Center text in the 118px frame starting at x=5
      display.drawStr(notif_x, 22, notif_str);
      display.setFont(u8g2_font_5x7_tf);
      display.drawStr(12, 38, pomo_alert_msg1);
      display.drawStr(12, 48, pomo_alert_msg2);
    } while (display.nextPage());
    return;
  }

  if (pomodoro_screen_active) {
    display.firstPage();
    do {
      if (pomo_screen_state == POMO_CONF_WORK) {
        // Screen 1: Work Time
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 15, "Pomodoro Timer");
        display.drawHLine(5, 18, 118);
        display.setFont(u8g2_font_5x7_tf);
        display.drawStr(5, 30, "Tiempo de trabajo:");
        char buf[30];
        sprintf(buf, "%d min", pomo_temp_work);
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 45, buf);
      }
      else if (pomo_screen_state == POMO_CONF_SHORT) {
        // Screen 2: Short Break
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 15, "Pomodoro Timer");
        display.drawHLine(5, 18, 118);
        display.setFont(u8g2_font_5x7_tf);
        display.drawStr(5, 30, "Descanso corto:");
        char buf[30];
        sprintf(buf, "%d min", pomo_temp_short);
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 45, buf);
      }
      else if (pomo_screen_state == POMO_CONF_LONG) {
        // Screen 3: Long Break
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 15, "Pomodoro Timer");
        display.drawHLine(5, 18, 118);
        display.setFont(u8g2_font_5x7_tf);
        display.drawStr(5, 30, "Descanso largo:");
        char buf[30];
        sprintf(buf, "%d min", pomo_temp_long);
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 45, buf);
      }
      else {
        // Main Pomodoro Screen
        display.setFont(u8g2_font_helvB10_tf);
        display.drawStr(5, 14, "POMODORO");
        display.drawHLine(5, 16, 118);
        
        // Phase name and Play/Pause indicator removed to prevent visual overlap
        
        // Time left
        char time_buf[10];
        sprintf(time_buf, "%02d:%02d", pomo_seconds_left / 60, pomo_seconds_left % 60);
        display.setFont(u8g2_font_inb21_mf);
        int w = display.getStrWidth(time_buf);
        display.drawStr((128 - w) / 2, 50, time_buf);
        
        // Progress indicators (4 circles at the bottom)
        for (int i = 0; i < 4; i++) {
          int cx = 34 + i * 20;
          int cy = 58;
          display.drawCircle(cx, cy, 3);
          
          bool completed = false;
          if (i == 0 && pomo_phase > 1) completed = true;
          if (i == 1 && pomo_phase > 3) completed = true;
          if (i == 2 && pomo_phase > 5) completed = true;
          if (completed) {
            display.drawDisc(cx, cy, 1);
          } else if ((pomo_phase / 2) == i) {
            if ((millis() / 500) % 2 == 0) {
              display.drawDisc(cx, cy, 1);
            }
          }
        }
      }
    } while (display.nextPage());
    return;
  }

  uint8_t j;
  display.firstPage();
#ifdef U8G2_LAYOUT_ROTATE_180
  drawTamaSelection(49);
  display.nextPage();

  for (j = 11; j < LCD_HEIGHT; j++)
  {
    drawTamaRow(j, j + j + j, 2);
  }
  display.nextPage();

  for (j = 5; j <= 10; j++)
  {
    if (j == 5)
    {
      drawTamaRow(j, j + j + j + 1, 1);
    }
    else
    {
      drawTamaRow(j, j + j + j, 2);
    }
  }
  display.nextPage();

  for (j = 0; j <= 5; j++)
  {
    if (j == 5)
    {
      drawTamaRow(j, j + j + j, 1);
    }
    else
    {
      drawTamaRow(j, j + j + j, 2);
    }
  }
  display.nextPage();
#else
  for (j = 0; j < LCD_HEIGHT; j++)
  {
    if (j != 5)
      drawTamaRow(j, j + j + j, 2);
    if (j == 5)
    {
      drawTamaRow(j, j + j + j, 1);
      display.nextPage();
      drawTamaRow(j, j + j + j + 1, 1);
    }
    if (j == 10)
      display.nextPage();
  }
  display.nextPage();
  drawTamaSelection(49);
  display.nextPage();
#endif
}

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START
void dumpStateToSerial()
{
  uint16_t i, count = 0;
  char tmp[10];
  cpu_get_state(&cpuState);
  u4_t *memTemp = cpuState.memory;
  uint8_t *cpuS = (uint8_t *)&cpuState;

  Serial.println("");
  Serial.println("static const uint8_t hardcodedState[] PROGMEM = {");
  for (i = 0; i < sizeof(cpu_state_t); i++, count++)
  {
    sprintf(tmp, "0x%02X,", cpuS[i]);
    Serial.print(tmp);
    if ((count % 16) == 15)
      Serial.println("");
  }
  for (i = 0; i < MEMORY_SIZE; i++, count++)
  {
    sprintf(tmp, "0x%02X,", memTemp[i]);
    Serial.print(tmp);
    if ((count % 16) == 15)
      Serial.println("");
  }
  Serial.println("};");
  /*
    Serial.println("");
    Serial.println("static const uint8_t bitmaps[] PROGMEM = {");
    for(i=0;i<144;i++) {
      sprintf(tmp, "0x%02X,", bitmaps_raw[i]);
      Serial.print(tmp);
      if ((i % 18)==17) Serial.println("");
    }
    Serial.println("};");  */
}
#endif

uint8_t reverseBits(uint8_t num)
{
  uint8_t reverse_num = 0;
  uint8_t i;
  for (i = 0; i < 8; i++)
  {
    if ((num & (1 << i)))
      reverse_num |= 1 << ((8 - 1) - i);
  }
  return reverse_num;
}





void setup()
{
#if defined(ESP32)
  setCpuFrequencyMhz(96); // Ajustar a 96MHz (frecuencia óptima de bus para el ESP32-H2)
#if SOC_WIFI_SUPPORTED
  WiFi.mode(WIFI_OFF);
#endif
  btStop();
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Resetear y liberar GPIO 1 (PIN_BATTERY) del periférico de la flash (FSPICS0) para forzarlo a modo GPIO analógico
  gpio_reset_pin((gpio_num_t)PIN_BATTERY);
  gpio_set_direction((gpio_num_t)PIN_BATTERY, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_BATTERY, GPIO_FLOATING);
#endif
  Serial.begin(SERIAL_BAUD);
  delay(1000); // Dar tiempo al transceptor USB-Serie para estabilizarse y no perder logs
  last_activity_time = millis();

  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_M, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
#if defined(ESP32)
  pinMode(PIN_BTN_4, INPUT_PULLUP);
  pinMode(PIN_BTN_RST, INPUT_PULLUP);
#endif

#if defined(ESP32)
  Wire.begin(PIN_SDA, PIN_SCL);
#endif

  if (!display.begin()) {
    Serial.println(F("Error: Pantalla no detectada. Continuando sin video..."));
    display_enabled = false;
  } else {
    display_enabled = true;
  }

  tamalib_register_hal(&hal);
  tamalib_set_framerate(TAMA_DISPLAY_FRAMERATE);
  tamalib_init(1000000);

#if defined(ENABLE_AUTO_SAVE_STATUS) || defined(ENABLE_LOAD_STATE_FROM_EEPROM)
  initEEPROM();
#endif

#ifdef ENABLE_LOAD_STATE_FROM_EEPROM
  Serial.println("Intentando cargar estado de EEPROM...");
  if (loadStateFromEEPROM(&cpuState)) {
    Serial.println("¡Estado cargado con éxito!");
    estadoActual = JUEGO;
  } else {
    Serial.println("Error: No se pudo cargar el estado (usando defaults).");
    estadoActual = CONFIGURACION;
  }
#elif ENABLE_LOAD_HARCODED_STATE_WHEN_START
  loadHardcodedState(&cpuState);
  estadoActual = JUEGO;
#else
  estadoActual = CONFIGURACION;
#endif

#ifdef ENABLE_DUMP_STATE_TO_SERIAL_WHEN_START
  dumpStateToSerial();
#endif

  pomo_configured = loadPomodoroConfig(pomo_work_time, pomo_short_break, pomo_long_break);
  if (pomo_configured) {
    loadPomodoroState();
    Serial.println(F("[Pomodoro] Config loaded successfully."));
  } else {
    Serial.println(F("[Pomodoro] No config found. Assistant will run on first use."));
  }

  Serial.println(F("Control nativo P1 cargado"));
}

// Tiempo de espera entre pasos de CPU para regular la velocidad de la emulación
// 80 microsegundos sincroniza el reloj y el crecimiento con el tiempo real (1s real = 1s emulado)
// y extiende el timeout nativo de inactividad de iconos a 4 segundos reales.
#define CPU_STEP_DELAY_US 80

void loop()
{
  static unsigned long last_cpu_step = 0;

  // Control de inactividad para suspender la pantalla
  if (!screen_sleeping && (millis() - last_activity_time >= SLEEP_TIMEOUT_MS)) {
    go_to_sleep();
  }

  // Control de salida del monitor de batería (después de 3.5 segundos)
  if (battery_screen_active) {
    static unsigned long last_battery_measure = 0;
    if (millis() - last_battery_measure >= 500) {
      last_battery_measure = millis();
      measure_battery();
      displayTama();
    }
    if (millis() - battery_screen_start_time >= 3500) {
      battery_screen_active = false;
      displayTama();
    }
  }

#if defined(ESP32)
  // Arquitectura de Burst Emulation (Light Sleep en ráfagas)
  // Entramos en Light Sleep solo si la pantalla está suspendida y no hay alarma de Pomodoro activa
  if (screen_sleeping && !pomo_alert_active) {
    // 1. Configurar botones para despertar la CPU (GPIO Interrupts)
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_L, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_M, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_R, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_4, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_RST, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    // 2. Configurar despertador por temporizador (1 segundo = 1,000,000 microsegundos)
    esp_sleep_enable_timer_wakeup(1000000ULL);

    // 3. Iniciar Light Sleep
    esp_light_sleep_start();
  }
#endif

  // Alarma Casio del Pomodoro (no bloqueante)
  run_pomo_alarm();

  static unsigned long last_real_second = 0;
  unsigned long now = micros();

  // Procesamiento acumulado (catch-up)
  if (now - last_cpu_step >= CPU_STEP_DELAY_US) {
    unsigned long elapsed = now - last_cpu_step;
    unsigned long steps = elapsed / CPU_STEP_DELAY_US;
    
    // Limitar ráfaga para evitar desbordamiento del watchdog (máx. 2 segundos = 25,000 pasos)
    if (steps > 25000) {
      steps = 25000;
    }
    
    for (unsigned long i = 0; i < steps; i++) {
      tamalib_mainloop_step_by_step();
    }
    last_cpu_step += steps * CPU_STEP_DELAY_US;
  }

  unsigned long currentMillis = millis();
  if (currentMillis - last_real_second >= 1000) {
    unsigned long elapsed_ms = currentMillis - last_real_second;
    unsigned long seconds_to_add = elapsed_ms / 1000;
    
    // Limitar segundos acumulados por seguridad
    if (seconds_to_add > 10) {
      seconds_to_add = 10;
    }
    
    for (unsigned long i = 0; i < seconds_to_add; i++) {
      tamalib_increment_second();
      // Temporizador de fondo Pomodoro
      update_pomodoro_timer();
    }
    last_real_second += seconds_to_add * 1000;
#ifdef ENABLE_SERIAL_DEBUG_INPUT
    Serial.println(F("Sincronización de segundos aplicada"));
#endif
  }

#ifdef ENABLE_AUTO_SAVE_STATUS
  // Auto-guardado periódico (cada 5 minutos de forma ininterrumpida)
  if ((millis() - lastSaveTimestamp) > (AUTO_SAVE_MINUTES * 60 * 1000))
  {
    lastSaveTimestamp = millis();
    saveStateToEEPROM(&cpuState);
    savePomodoroState();
  }
#endif
}