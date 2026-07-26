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
#if defined(ENABLE_AUTO_SAVE_STATUS) || defined(ENABLE_LOAD_STATE_FROM_EEPROM)
#include "savestate.h"
#include <EEPROM.h>
#endif

#include "config_fix.h"

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

enum EstadoTamagotchi {
  CONFIGURACION,
  JUEGO
};
static EstadoTamagotchi estadoActual = CONFIGURACION;

#if defined(ESP32)
// Buttons K1, K2, K3, K4 mapping for ESP32
#define PIN_BTN_L 25   // K1 (Select / Left)
#define PIN_BTN_M 26   // K2 (Confirm / Middle)
#define PIN_BTN_R 27   // K3 (Cancel / Right)
#define PIN_BTN_4 33   // K5 (Decrement / Fourth Button)
#define PIN_BTN_RST 5  // K4 (Reset)
#define PIN_BUZZER 15
#define BUZZER_CHANNEL 0
#define TONE_CHANNEL 15
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
void esp32_noTone(uint8_t pin, uint8_t channel)
{
  ledcWrite(channel, 0);
  ledcDetachPin(pin);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

void esp32_tone(uint8_t pin, unsigned int frequency, unsigned long duration, uint8_t channel)
{
  ledcAttachPin(pin, channel);
  ledcWriteTone(channel, frequency);
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
}

void displayTama();
static timestamp_t hal_get_timestamp(void);


/**** TamaLib Specific Variables ****/
static uint16_t current_freq = 0;
static bool_t matrix_buffer[LCD_HEIGHT][LCD_WIDTH / 8] = {{0}};
// static byte runOnceBool = 0;
static bool_t icon_buffer[ICON_NUM] = {0};
static cpu_state_t cpuState;
static unsigned long lastSaveTimestamp = 0;
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
  if (en)
  {

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

// Debounce de 300ms real para eliminar saltos de números
#define BTN_DEBOUNCE_MS 300

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

  if (pin == 26) {
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
  else if (pin == 27) {
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

  // Debounce de 300ms por pin
  static bool debounced_l = false;
  static bool debounced_m = false;
  static bool debounced_r = false;
  static unsigned long last_time_l = 0;
  static unsigned long last_time_m = 0;
  static unsigned long last_time_r = 0;

  unsigned long now = millis();

  // Lectura cruda
  bool raw_l = (digitalRead(PIN_BTN_L) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 25
  bool raw_m = (digitalRead(PIN_BTN_M) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 26
  bool raw_r = (digitalRead(PIN_BTN_R) == BUTTON_VOLTAGE_LEVEL_PRESSED); // GPIO 27

  // Debounce: cambiar estado solo si han pasado 300ms desde el último cambio
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

  bool btn_l = debounced_l;  // GPIO 25
  bool btn_m = debounced_m;  // GPIO 26
  bool btn_r = debounced_r;  // GPIO 27

  // Detección de flancos ascendentes (pulsación)
  bool pressed_l = btn_l && !prev_l;
  bool pressed_m = btn_m && !prev_m;
  bool pressed_r = btn_r && !prev_r;

  // Máquina de estados para secuencia: GPIO27 -> GPIO25 -> GPIO27 -> GPIO26 (< 3s)
  static int seq_step = 0;
  static unsigned long seq_start_time = 0;

  if (pressed_l || pressed_m || pressed_r) {
    unsigned long current_time = millis();
    if (seq_step > 0 && (current_time - seq_start_time > 3000)) {
      seq_step = 0;
    }

    if (seq_step == 0) {
      if (pressed_r) { // GPIO 27
        seq_step = 1;
        seq_start_time = current_time;
        Serial.println(F("[EasterEgg] Seq step 1: GPIO27 pressed"));
      }
    } else if (seq_step == 1) {
      if (pressed_l) { // GPIO 25
        seq_step = 2;
        Serial.println(F("[EasterEgg] Seq step 2: GPIO25 pressed"));
      } else {
        seq_step = pressed_r ? 1 : 0;
        if (seq_step == 1) seq_start_time = current_time;
      }
    } else if (seq_step == 2) {
      if (pressed_r) { // GPIO 27
        seq_step = 3;
        Serial.println(F("[EasterEgg] Seq step 3: GPIO27 pressed"));
      } else {
        seq_step = 0;
      }
    } else if (seq_step == 3) {
      if (pressed_m) { // GPIO 26
        seq_step = 0;
        Serial.println(F("[EasterEgg] Seq complete! Activating Easter Egg 1"));
        triggerEasterEgg1();
        // Limpiamos los flancos detectados y debounces para que no interfieran en el juego tras volver
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
          int clock_hours = cpuState.memory[37] * 10 + cpuState.memory[36];
          bool clock_is_pm = cpuState.memory[38] == 1;

          int h24 = (clock_hours % 12) + (clock_is_pm ? 12 : 0);
          h24 = (h24 + 1) % 24;
          clock_hours = h24 % 12;
          if (clock_hours == 0) clock_hours = 12;
          clock_is_pm = (h24 >= 12);

          cpuState.memory[36] = clock_hours % 10;
          cpuState.memory[37] = clock_hours / 10;
          cpuState.memory[38] = clock_is_pm ? 1 : 0;
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
          int clock_minutes = cpuState.memory[33] * 10 + cpuState.memory[32];
          clock_minutes = (clock_minutes + 1) % 60;
          cpuState.memory[32] = clock_minutes % 10;
          cpuState.memory[33] = clock_minutes / 10;
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
          int saved_h = cpuState.memory[37] * 10 + cpuState.memory[36];
          int saved_m = cpuState.memory[33] * 10 + cpuState.memory[32];
          bool saved_pm = cpuState.memory[38] == 1;
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

void displayTama()
{
  if (!display_enabled) return;
  display.clearBuffer(); // Limpieza de búfer forzada para evitar basura en pantalla (cuadros blancos)
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
  Serial.begin(SERIAL_BAUD);

  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_M, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
#if defined(ESP32)
  pinMode(PIN_BTN_4, INPUT_PULLUP);
  pinMode(PIN_BTN_RST, INPUT_PULLUP);
#endif

#if defined(ESP32)
  ledcSetup(BUZZER_CHANNEL, NOTE_C4, 8);
  Wire.begin(14, 12);
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

  Serial.println(F("Control nativo P1 cargado"));
}

// Tiempo de espera entre pasos de CPU para regular la velocidad de la emulación
// 80 microsegundos sincroniza el reloj y el crecimiento con el tiempo real (1s real = 1s emulado)
// y extiende el timeout nativo de inactividad de iconos a 4 segundos reales.
#define CPU_STEP_DELAY_US 80

void loop()
{
  static unsigned long last_cpu_step = 0;
  static unsigned long last_real_second = 0;
  unsigned long now = micros();

  if (now - last_cpu_step >= CPU_STEP_DELAY_US) {
    last_cpu_step = now;
    tamalib_mainloop_step_by_step();
  }

  unsigned long currentMillis = millis();
  if (currentMillis - last_real_second >= 1000) {
    last_real_second = currentMillis;
    tamalib_increment_second();
    Serial.println(F("Sincronización de segundos aplicada"));
  }

#ifdef ENABLE_AUTO_SAVE_STATUS
  // Auto-guardado periódico
  if ((millis() - lastSaveTimestamp) > (AUTO_SAVE_MINUTES * 60 * 1000))
  {
    lastSaveTimestamp = millis();
    saveStateToEEPROM(&cpuState);
  }
#endif
}