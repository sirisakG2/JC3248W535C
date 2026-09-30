#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

// Guition JC3248W535C: AXS15231B 320x480 QSPI LCD + capacitive touch (I2C 0x3B)
#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_INT 3
#define TOUCH_ADDR 0x3B

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, 320, 480);
Arduino_Canvas *gfx = new Arduino_Canvas(320, 480, panel, 0, 0, 0);

bool readTouch(int &x, int &y) {
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, 8};
  uint8_t b[8];
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 8);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 8) != 8) return false;
  for (int i = 0; i < 8; i++) b[i] = Wire.read();
  if (b[1] == 0 || b[1] > 4) return false;
  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("JC3248W535C boot");
  Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());

  if (!gfx->begin(40000000)) Serial.println("gfx->begin(40000000) FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 320, 160, RGB565_RED);
  gfx->fillRect(0, 160, 320, 160, RGB565_GREEN);
  gfx->fillRect(0, 320, 320, 160, RGB565_BLUE);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, 20);
  gfx->println("Hello JC3248");
  gfx->flush();

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(TOUCH_ADDR);
  Serial.printf("Touch 0x%02X: %s\n", TOUCH_ADDR, Wire.endTransmission() == 0 ? "found" : "NOT found");
}

void loop() {
  static bool dirty = false;
  static uint32_t lastFlush = 0;
  int x, y;
  if (readTouch(x, y)) {
    gfx->fillCircle(x, y, 6, RGB565_WHITE);
    dirty = true;
  }
  if (dirty && millis() - lastFlush >= 30) {
    gfx->flush();
    lastFlush = millis();
    dirty = false;
  }
  delay(5);
}
