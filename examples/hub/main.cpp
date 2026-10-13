// Game hub / launcher for the JC3248W535C. Each game is a separate app in its own flash
// slot (ota_1..ota_N); tapping a tile makes that slot the boot partition and restarts.
// Games return here by holding the top-left corner (1.5 s) or the BOOT button (0.7 s).
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define SCR_W 320
#define SCR_H 480

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);

struct Game { const char *name; int slot; uint8_t r, g, b; };   // slot = ota_N
const Game GAMES[8] = {
  {"Tic-Tac-Toe", 1, 40, 60, 110}, {"2048", 2, 150, 110, 40},
  {"Snake", 3, 30, 100, 50},       {"Connect 4", 4, 30, 70, 150},
  {"Tetris", 5, 100, 50, 130},     {"Minesweeper", 6, 70, 76, 90},
  {"Memory", 7, 20, 110, 120},     {"Paint", 8, 140, 50, 80},
};
const int NGAMES = 8;
const int TILE_X0 = 10, TILE_Y0 = 68, TW = 146, TH = 96, TG = 6;

bool installed[NGAMES];
uint16_t C_BG, C_TXT, C_DIM;
uint16_t tileCol[NGAMES];
String subtitle[NGAMES];             // subtitle under each icon (best score / record)
int lastPlayed = -1;
Preferences hubPrefs;

// Read a game's saved best score straight from its own NVS namespace (read-only).
static String statFor(int i) {
  static const char *LV[3] = {"Easy", "Med", "Hard"};
  Preferences p;
  char b[40], k[8];
  const char *none = "No score";
  switch (i) {
    case 0:  if (!p.begin("ttt", true)) return none;
             snprintf(b, sizeof b, "X %d  O %d", p.getInt("x", 0), p.getInt("o", 0)); break;
    case 1:  if (!p.begin("g2048", true) || !p.getInt("best", 0)) return none;
             snprintf(b, sizeof b, "Best %d", p.getInt("best", 0)); break;
    case 2:  if (!p.begin("snake", true) || !p.getInt("best", 0)) return none;
             snprintf(b, sizeof b, "Best %d", p.getInt("best", 0)); break;
    case 3:  if (!p.begin("c4", true)) return none;
             snprintf(b, sizeof b, "R %d  Y %d", p.getInt("s1", 0), p.getInt("s2", 0)); break;
    case 4:  if (!p.begin("tetris", true) || !p.getInt("best", 0)) return none;
             snprintf(b, sizeof b, "Best %d", p.getInt("best", 0)); break;
    case 5: { if (!p.begin("mines", true)) return none;
              int l = constrain(p.getInt("lvl", 0), 0, 2); snprintf(k, sizeof k, "best%d", l);
              int t = p.getInt(k, 0); if (!t) return none;
              snprintf(b, sizeof b, "%s: %ds", LV[l], t); break; }
    case 6: { if (!p.begin("memory", true)) return none;
              int l = constrain(p.getInt("lvl", 0), 0, 2); snprintf(k, sizeof k, "bm%d", l);
              int m = p.getInt(k, 0); if (!m) return none;
              snprintf(b, sizeof b, "%s: %d mv", LV[l], m); break; }
    default: return "Draw & save";
  }
  p.end();
  return String(b);
}

static const esp_partition_t *slotPart(int slot) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot), NULL);
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static void tileRect(int i, int &x, int &y) { x = TILE_X0 + (i % 2) * (TW + TG); y = TILE_Y0 + (i / 2) * (TH + TG); }

static void icon(int i, int cx, int cy) {
  uint16_t W = RGB565_WHITE;
  switch (i) {
    case 0:   // tic-tac-toe
      for (int t = -1; t <= 0; t++) { gfx->drawFastVLine(cx - 9 + t, cy - 24, 48, W); gfx->drawFastVLine(cx + 9 + t, cy - 24, 48, W);
                                       gfx->drawFastHLine(cx - 24, cy - 9 + t, 48, W); gfx->drawFastHLine(cx - 24, cy + 9 + t, 48, W); }
      gfx->drawLine(cx - 22, cy - 22, cx - 12, cy - 12, RGB565_RED); gfx->drawLine(cx - 12, cy - 22, cx - 22, cy - 12, RGB565_RED);
      gfx->drawCircle(cx, cy, 5, RGB565_CYAN); gfx->drawCircle(cx, cy, 4, RGB565_CYAN);
      gfx->drawLine(cx + 12, cy + 12, cx + 22, cy + 22, RGB565_RED); gfx->drawLine(cx + 22, cy + 12, cx + 12, cy + 22, RGB565_RED);
      break;
    case 1:   // 2048
      gfx->fillRoundRect(cx - 24, cy - 20, 22, 22, 4, gfx->color565(238, 228, 218)); centerText("2", cx - 13, cy - 9, 2, gfx->color565(119, 110, 101));
      gfx->fillRoundRect(cx + 2, cy - 20, 22, 22, 4, gfx->color565(242, 177, 121));  centerText("8", cx + 13, cy - 9, 2, W);
      gfx->fillRoundRect(cx - 24, cy + 4, 22, 22, 4, gfx->color565(246, 124, 95));   centerText("4", cx - 13, cy + 15, 2, W);
      gfx->fillRoundRect(cx + 2, cy + 4, 22, 22, 4, gfx->color565(237, 194, 46));    centerText("16", cx + 13, cy + 15, 1, W);
      break;
    case 2:   // snake
      for (int k = 0; k < 6; k++) gfx->fillRoundRect(cx - 28 + k * 10, cy + ((k & 1) ? -8 : 6), 10, 10, 3, k == 5 ? RGB565_GREENYELLOW : gfx->color565(70, 200, 80));
      gfx->fillCircle(cx + 30, cy - 12, 4, RGB565_RED);
      break;
    case 3:   // connect four
      gfx->fillRoundRect(cx - 30, cy - 24, 60, 48, 6, gfx->color565(60, 100, 220));
      for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
        uint16_t col = (r == 2 && c < 3) ? ((c & 1) ? RGB565_YELLOW : RGB565_RED) : (r == 1 && c == 1 ? RGB565_RED : RGB565_BLACK);
        gfx->fillCircle(cx - 22 + c * 15, cy - 14 + r * 14, 5, col);
      }
      break;
    case 4:   // tetris
      { uint16_t a = RGB565_CYAN, b = gfx->color565(170, 60, 220), c2 = RGB565_ORANGE;
        for (int k = 0; k < 4; k++) gfx->fillRect(cx - 28 + k * 14, cy + 12, 13, 13, a);
        for (int k = 0; k < 3; k++) gfx->fillRect(cx - 14 + k * 14, cy - 2, 13, 13, b);
        gfx->fillRect(cx, cy - 16, 13, 13, b);
        gfx->fillRect(cx - 28, cy - 2, 13, 13, c2); gfx->fillRect(cx - 28, cy - 16, 13, 13, c2); }
      break;
    case 5:   // minesweeper mine
      gfx->fillCircle(cx, cy, 13, RGB565_BLACK);
      for (int a = 0; a < 4; a++) { float t = a * M_PI / 4; gfx->drawLine(cx - cosf(t) * 22, cy - sinf(t) * 22, cx + cosf(t) * 22, cy + sinf(t) * 22, RGB565_BLACK); }
      gfx->fillCircle(cx - 4, cy - 4, 3, W);
      break;
    case 6:   // memory cards
      gfx->fillRoundRect(cx - 30, cy - 20, 30, 42, 5, gfx->color565(60, 100, 220));
      centerText("?", cx - 15, cy + 1, 3, gfx->color565(150, 180, 250));
      gfx->fillRoundRect(cx, cy - 24, 30, 42, 5, W);
      gfx->fillCircle(cx + 15, cy - 3, 9, RGB565_RED);
      break;
    case 7:   // paint strokes
      for (int k = 0; k < 20; k++) gfx->fillCircle(cx - 28 + k * 3, cy + 8 - (int)(sinf(k * 0.35f) * 14), 4, gfx->color565(255 - k * 8, 90 + k * 6, 60));
      gfx->fillCircle(cx + 26, cy + 14, 5, RGB565_YELLOW);
      break;
  }
}

static uint16_t shade(int i, float f) {            // darker/lighter variant of a tile colour
  return gfx->color565(constrain((int)(GAMES[i].r * f), 0, 255), constrain((int)(GAMES[i].g * f), 0, 255), constrain((int)(GAMES[i].b * f), 0, 255));
}

static void drawTile(int i) {
  int x, y;
  tileRect(i, x, y);
  gfx->fillRoundRect(x, y, TW, TH, 12, tileCol[i]);
  gfx->fillRoundRect(x, y + TH - 24, TW, 24, 12, shade(i, 0.55f));          // darker label strip
  gfx->fillRect(x, y + TH - 24, TW, 12, shade(i, 0.55f));
  if (i == lastPlayed) {                                                    // last played = white outline
    gfx->drawRoundRect(x, y, TW, TH, 12, RGB565_WHITE);
    gfx->drawRoundRect(x + 1, y + 1, TW - 2, TH - 2, 11, RGB565_WHITE);
  }
  icon(i, x + TW / 2, y + 26);
  centerText(subtitle[i].c_str(), x + TW / 2, y + TH - 36, 2, gfx->color565(245, 245, 245));
  centerText(GAMES[i].name, x + TW / 2, y + TH - 12, 2, installed[i] ? C_TXT : C_DIM);
  if (!installed[i]) centerText("not installed", x + TW / 2, y + 10, 1, RGB565_YELLOW);
}

static void render() {
  gfx->fillScreen(C_BG);
  centerText("GAMES", SCR_W / 2, 26, 4, C_TXT);
  centerText("In a game: hold the top-left corner to return", SCR_W / 2, 54, 1, C_DIM);
  for (int i = 0; i < NGAMES; i++) drawTile(i);
  gfx->flush();
}

// ---------------------------------------------------------------- touch + launch
static bool readTouch(int &x, int &y) {
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, 8};
  uint8_t d[8];
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 8);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 8) != 8) return false;
  for (int i = 0; i < 8; i++) d[i] = Wire.read();
  if (d[1] == 0 || d[1] > 4) return false;
  x = ((d[2] & 0x0F) << 8) | d[3];
  y = ((d[4] & 0x0F) << 8) | d[5];
  return true;
}

static int tileAt(int x, int y) {
  for (int i = 0; i < NGAMES; i++) {
    int tx, ty;
    tileRect(i, tx, ty);
    if (x >= tx && x < tx + TW && y >= ty && y < ty + TH) return i;
  }
  return -1;
}

static void launch(int i) {
  const esp_partition_t *p = slotPart(GAMES[i].slot);
  if (!p || !installed[i]) return;
  int x, y;
  tileRect(i, x, y);
  for (int f = 1; f <= 5; f++) {                          // tile grows to fill the screen
    float t = f / 5.0f;
    int cx = x + (int)((0 - x) * t), cy = y + (int)((0 - y) * t);
    int cw = TW + (int)((SCR_W - TW) * t), ch = TH + (int)((SCR_H - TH) * t);
    gfx->fillRoundRect(cx, cy, cw, ch, 12, tileCol[i]);
    gfx->drawRoundRect(cx, cy, cw, ch, 12, RGB565_WHITE);
    gfx->flush();
  }
  icon(i, SCR_W / 2, SCR_H / 2 - 20);
  centerText(GAMES[i].name, SCR_W / 2, SCR_H / 2 + 50, 3, C_TXT);
  centerText("Loading...", SCR_W / 2, SCR_H / 2 + 90, 2, C_DIM);
  gfx->flush();
  hubPrefs.putInt("last", i);
  if (esp_ota_set_boot_partition(p) == ESP_OK) { delay(150); esp_restart(); }
  Serial.println("set_boot_partition failed");
  render();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("JC3248W535C game hub boot");
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(14, 18, 30);
  C_TXT = RGB565_WHITE;
  C_DIM = gfx->color565(130, 140, 160);
  for (int i = 0; i < NGAMES; i++) {
    tileCol[i] = gfx->color565(GAMES[i].r, GAMES[i].g, GAMES[i].b);
    const esp_partition_t *p = slotPart(GAMES[i].slot);
    esp_app_desc_t d;
    installed[i] = p && esp_ota_get_partition_description(p, &d) == ESP_OK;
    subtitle[i] = statFor(i);
    Serial.printf("slot %d %-12s %s\n", GAMES[i].slot, GAMES[i].name, installed[i] ? "installed" : "MISSING");
  }
  hubPrefs.begin("hub", false);
  lastPlayed = hubPrefs.getInt("last", -1);
  render();
}

void loop() {
  static bool touching = false;
  static int misses;
  int x, y;
  if (readTouch(x, y)) {
    misses = 0;
    if (!touching) { touching = true; int t = tileAt(x, y); if (t >= 0) launch(t); }
  } else if (touching && ++misses >= 6) {
    touching = false;
  }
  delay(5);
}
