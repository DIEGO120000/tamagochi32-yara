#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

// Definición de pines I2C para ESP32
#define I2C_SDA 14
#define I2C_SCL 12

// Inicialización de la pantalla SSD1306 128x64 usando I2C por hardware completo (_F_)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

void setup() {
  // Inicializar puerto serie para diagnóstico
  Serial.begin(74880);
  Serial.println("Inicializando pantalla con diseño final de interfaz...");

  // Inicializar bus I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // Inicializar biblioteca U8g2
  u8g2.begin();
}

void loop() {
  // Limpiar el búfer interno de pantalla
  u8g2.clearBuffer();

  // 1. Texto superior izquierdo: "holaa brooo" (fuente pequeña 5x7 en posición 0, 7)
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 7, "holaa brooo");

  // 2. Texto central principal: "see you" (fuente helvB10 centrado)
  u8g2.setFont(u8g2_font_helvB10_tf);
  const char* texto_central = "see you";
  int x_central = (128 - u8g2.getStrWidth(texto_central)) / 2;
  u8g2.drawStr(x_central, 30, texto_central);

  // 3. Texto inferior: "soon" (fuente helvB10 centrado)
  const char* texto_inferior = "soon";
  int x_inferior = (128 - u8g2.getStrWidth(texto_inferior)) / 2;
  u8g2.drawStr(x_inferior, 47, texto_inferior);

  // 4. Texto inferior derecho: "sjsjs(maybe)" (fuente pequeña 5x7)
  u8g2.setFont(u8g2_font_5x7_tf);
  const char* texto_derecha = "sjsjs(maybe)";
  // Calcular la posición x exacta para alinear al borde derecho (dejando 2px de margen)
  int x_derecha = 128 - u8g2.getStrWidth(texto_derecha) - 2;
  // Posición y = 62 para dejar 2px de margen abajo en una pantalla de 64 de alto
  int y_derecha = 62; 
  u8g2.drawStr(x_derecha, y_derecha, texto_derecha);

  // Enviar el búfer completo a la pantalla física
  u8g2.sendBuffer();

  // Retraso para no saturar el loop
  delay(2000);
}
