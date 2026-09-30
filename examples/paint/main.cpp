// Touch paint app for the JC3248W535C.
// Draw with your finger; top toolbar row = colours (2nd swatch is the eraser),
// second row = brush size -/+, CLR, SAVE. SAVE writes /paint/paintN.bmp to the SD card.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>

#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define SCR_W 320
#define SCR_H 480

#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

#define CANVAS_H 400          // drawing area; toolbar below
#define TB_Y CANVAS_H         // toolbar top
#define SW 40                 // swatch / button size

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
SPIClass sdSpi(HSPI);
bool sdOk = false;

const uint16_t PALETTE[8] = {RGB565_BLACK, RGB565_WHITE, RGB565_RED, RGB565_ORANGE,
                             RGB565_YELLOW, RGB565_GREEN, RGB565_BLUE, RGB565_PURPLE};
int colorIdx = 0;
int brush = 4;                // radius in px
String msg = "";
uint32_t msgUntil = 0;

static bool readTouch(int &x, int &y) {
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

// ---------------------------------------------------------------- toolbar
static void drawToolbar() {
  gfx->fillRect(0, TB_Y, SCR_W, SCR_H - TB_Y, 0x2104);  // dark grey
  for (int i = 0; i < 8; i++) {
    gfx->fillRect(i * SW + 3, TB_Y + 3, SW - 6, SW - 6, PALETTE[i]);
    if (i == colorIdx) gfx->drawRect(i * SW + 1, TB_Y + 1, SW - 2, SW - 2, i == 1 ? RGB565_RED : RGB565_WHITE);
    if (i == 1) { gfx->setTextColor(RGB565_BLACK); gfx->setTextSize(1); gfx->setCursor(i * SW + 8, TB_Y + 16); gfx->print("ERASE"); }
  }
  int y2 = TB_Y + SW;
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->drawRect(3, y2 + 3, SW - 6, SW - 6, RGB565_WHITE);      gfx->setCursor(12, y2 + 9);  gfx->print("-");
  gfx->drawRect(SW + 3, y2 + 3, SW - 6, SW - 6, RGB565_WHITE); gfx->setCursor(SW + 12, y2 + 9); gfx->print("+");
  gfx->fillCircle(2 * SW + 20, y2 + 20, brush, colorIdx == 1 ? RGB565_DARKGREY : PALETTE[colorIdx]);
  gfx->setTextSize(2);
  gfx->drawRect(3 * SW + 3, y2 + 3, SW * 2 - 6, SW - 6, RGB565_WHITE);      gfx->setCursor(3 * SW + 16, y2 + 12); gfx->print("CLR");
  gfx->drawRect(5 * SW + 3, y2 + 3, SW * 3 - 6, SW - 6, RGB565_GREEN);     gfx->setCursor(5 * SW + 32, y2 + 12); gfx->print("SAVE");
  if (millis() < msgUntil) { gfx->setTextSize(1); gfx->setTextColor(RGB565_YELLOW); gfx->setCursor(4, SCR_H - 10); gfx->print(msg); }
}

static void note(const String &m) { msg = m; msgUntil = millis() + 3000; }

// ---------------------------------------------------------------- BMP save
static bool saveBmp(String &nameOut) {
  if (!sdOk) return false;
  SD.mkdir("/paint");
  int n = 1;
  char path[40];
  do { snprintf(path, sizeof path, "/paint/paint%d.bmp", n++); } while (SD.exists(path));
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;

  const uint32_t w = SCR_W, h = CANVAS_H, dataSize = w * h * 2, hdr = 14 + 40 + 12;
  uint8_t head[hdr] = {0};
  auto le32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) head[o + i] = (v >> (8 * i)) & 0xFF; };
  head[0] = 'B'; head[1] = 'M';
  le32(2, hdr + dataSize); le32(10, hdr);
  le32(14, 40); le32(18, w); le32(22, (uint32_t)(-(int32_t)h));  // negative height = top-down
  head[26] = 1; head[28] = 16;
  le32(30, 3);                    // BI_BITFIELDS
  le32(34, dataSize);
  le32(54, 0xF800); le32(58, 0x07E0); le32(62, 0x001F);  // RGB565 masks
  f.write(head, hdr);
  f.write((const uint8_t *)gfx->getFramebuffer(), dataSize);
  f.close();
  nameOut = String(path);
  return true;
}

// ---------------------------------------------------------------- toolbar taps
static void toolbarTap(int x, int y) {
  if (y < TB_Y + SW) {
    int i = constrain(x / SW, 0, 7);
    colorIdx = i;
  } else {
    if (x < SW) brush = max(1, brush - 1);
    else if (x < 2 * SW) brush = min(20, brush + 1);
    else if (x >= 3 * SW && x < 5 * SW) { gfx->fillRect(0, 0, SCR_W, CANVAS_H, RGB565_WHITE); note("Cleared"); }
    else if (x >= 5 * SW) {
      String name;
      note(saveBmp(name) ? "Saved " + name : (sdOk ? "Save failed" : "No SD card"));
    }
  }
  drawToolbar();
  gfx->flush();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C paint boot");
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSpi, 40000000);
  Serial.println(sdOk ? "SD OK" : "SD mount FAILED (save disabled)");

  gfx->fillScreen(RGB565_WHITE);
  drawToolbar();
  gfx->flush();
}

void loop() {
  static bool down = false, inToolbar = false, dirty = false, hadMsg = false;
  static int px, py, misses;
  static uint32_t lastFlush = 0;
  int x, y;

  if (readTouch(x, y)) {
    misses = 0;
    if (!down) {
      down = true;
      inToolbar = y >= TB_Y;
      if (inToolbar) toolbarTap(x, y);
      else { px = x; py = y; }
    }
    if (!inToolbar && y < CANVAS_H) {
      uint16_t c = PALETTE[colorIdx];
      int r = colorIdx == 1 ? brush * 2 : brush;   // eraser is a bit wider
      int dx = x - px, dy = y - py;
      int steps = max(1, max(abs(dx), abs(dy)) / max(1, r / 2));
      for (int i = 0; i <= steps; i++) gfx->fillCircle(px + dx * i / steps, py + dy * i / steps, r, c);
      px = x; py = y;
      dirty = true;
    }
  } else if (down && ++misses >= 3) {
    down = false;
  }

  bool msgShown = millis() < msgUntil;
  if (hadMsg && !msgShown) { drawToolbar(); dirty = true; }
  hadMsg = msgShown;

  if (dirty && millis() - lastFlush >= 25) {
    gfx->flush();
    lastFlush = millis();
    dirty = false;
  }
  delay(3);
}
