// Minesweeper for the JC3248W535C.
// Tap a cell to dig; long-press (or switch the DIG/FLAG button) to flag. The first tap is
// always safe. Tapping a satisfied number digs its neighbours (chording). LEVEL cycles
// Easy / Medium / Hard; best times per level are saved in flash.
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>

#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define SCR_W 320
#define SCR_H 480

#define FIELD_Y 58
#define BTN_Y 440
#define BTN_H 36
#define MAX_C 12
#define MAX_R 14
#define LONG_MS 450

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

struct Level { const char *name; int cols, rows, mines, cell; };
const Level LEVELS[3] = {{"Easy", 8, 10, 10, 37}, {"Medium", 10, 12, 20, 30}, {"Hard", 12, 14, 34, 26}};
int lvl = 0;
int COLS, ROWS, MINES, CELL, OX;

uint8_t mine[MAX_R][MAX_C], open_[MAX_R][MAX_C], flag[MAX_R][MAX_C], nbr[MAX_R][MAX_C];
enum State { READY, PLAYING, WON, LOST };
State state = READY;
bool flagMode = false;
int flags = 0, opened = 0, boomR = -1, boomC = -1;
uint32_t startMs = 0, endMs = 0;
int bestTime[3] = {0, 0, 0};
bool newBest = false;
uint16_t C_BG, C_HID, C_HID_HI, C_HID_LO, C_OPEN, C_TXT, C_BTN, C_BTN_ON;
uint16_t NUMC[9];

// ---------------------------------------------------------------- logic
static void setLevel(int l) {
  lvl = l;
  COLS = LEVELS[l].cols; ROWS = LEVELS[l].rows; MINES = LEVELS[l].mines; CELL = LEVELS[l].cell;
  OX = (SCR_W - COLS * CELL) / 2;
}

static void newGame() {
  memset(mine, 0, sizeof mine); memset(open_, 0, sizeof open_);
  memset(flag, 0, sizeof flag); memset(nbr, 0, sizeof nbr);
  state = READY; flags = opened = 0; boomR = boomC = -1; newBest = false;
}

static int countAround(int r, int c, const uint8_t (*arr)[MAX_C]) {
  int n = 0;
  for (int dr = -1; dr <= 1; dr++)
    for (int dc = -1; dc <= 1; dc++) {
      int rr = r + dr, cc = c + dc;
      if ((dr || dc) && rr >= 0 && rr < ROWS && cc >= 0 && cc < COLS && arr[rr][cc]) n++;
    }
  return n;
}

// place mines after the first dig, never on or next to it
static void placeMines(int sr, int sc) {
  int placed = 0;
  while (placed < MINES) {
    int r = random(ROWS), c = random(COLS);
    if (mine[r][c] || (abs(r - sr) <= 1 && abs(c - sc) <= 1)) continue;
    mine[r][c] = 1; placed++;
  }
  for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) nbr[r][c] = countAround(r, c, mine);
}

static void finish(bool win) {
  state = win ? WON : LOST;
  endMs = millis();
  if (win) {
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) if (mine[r][c]) flag[r][c] = 1;
    flags = MINES;
    int t = (endMs - startMs) / 1000;
    if (!bestTime[lvl] || t < bestTime[lvl]) {
      bestTime[lvl] = t; newBest = true;
      char k[8]; snprintf(k, sizeof k, "best%d", lvl);
      prefs.putInt(k, t);
    }
  }
}

// flood-fill open, stopping at numbers; flags are never opened
static void openCell(int r, int c) {
  int stack[MAX_R * MAX_C][2], sp = 0;
  stack[sp][0] = r; stack[sp][1] = c; sp++;
  while (sp) {
    sp--;
    int cr = stack[sp][0], cc = stack[sp][1];
    if (open_[cr][cc] || flag[cr][cc]) continue;
    open_[cr][cc] = 1; opened++;
    if (mine[cr][cc]) { boomR = cr; boomC = cc; finish(false); return; }
    if (nbr[cr][cc] == 0)
      for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
          int rr = cr + dr, nc = cc + dc;
          if (rr >= 0 && rr < ROWS && nc >= 0 && nc < COLS && !open_[rr][nc] && sp < MAX_R * MAX_C - 1) { stack[sp][0] = rr; stack[sp][1] = nc; sp++; }
        }
  }
  if (opened == ROWS * COLS - MINES) finish(true);
}

static void dig(int r, int c) {
  if (state == WON || state == LOST || flag[r][c]) return;
  if (state == READY) { placeMines(r, c); state = PLAYING; startMs = millis(); }
  if (open_[r][c]) {                                   // chord: number satisfied by flags -> dig the rest
    if (nbr[r][c] && countAround(r, c, flag) == nbr[r][c])
      for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
          int rr = r + dr, cc = c + dc;
          if ((dr || dc) && rr >= 0 && rr < ROWS && cc >= 0 && cc < COLS && !open_[rr][cc] && !flag[rr][cc] && state == PLAYING) openCell(rr, cc);
        }
    return;
  }
  openCell(r, c);
}

static void toggleFlag(int r, int c) {
  if (state == WON || state == LOST || open_[r][c]) return;
  if (state == READY) return;                          // nothing to flag before the first dig
  flag[r][c] ^= 1;
  flags += flag[r][c] ? 1 : -1;
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static void drawMine(int x, int y, int s, uint16_t bg) {
  int cx = x + s / 2, cy = y + s / 2, r = s / 4;
  gfx->fillRect(x, y, s - 1, s - 1, bg);
  gfx->fillCircle(cx, cy, r, RGB565_BLACK);
  gfx->drawLine(cx - r - 3, cy, cx + r + 3, cy, RGB565_BLACK);
  gfx->drawLine(cx, cy - r - 3, cx, cy + r + 3, RGB565_BLACK);
  gfx->fillCircle(cx - r / 3, cy - r / 3, 2, RGB565_WHITE);
}

static void drawFlag(int x, int y, int s) {
  int cx = x + s / 2;
  gfx->fillRect(cx - 1, y + s / 4, 3, s / 2, RGB565_BLACK);
  gfx->fillRect(cx - s / 4, y + s * 3 / 4 - 2, s / 2, 4, RGB565_BLACK);
  gfx->fillTriangle(cx, y + s / 5, cx, y + s / 2, cx - s / 4 - 2, y + s * 3 / 10 + 2, RGB565_RED);
}

static void drawBtn(int x, int w, const char *label, bool on = false) {
  gfx->fillRoundRect(x, BTN_Y, w, BTN_H, 8, on ? C_BTN_ON : C_BTN);
  centerText(label, x + w / 2, BTN_Y + BTN_H / 2, 2, C_TXT);
}

static void render() {
  gfx->fillScreen(C_BG);

  // header: mines left | status | timer
  char b[24];
  snprintf(b, sizeof b, "%03d", max(0, MINES - flags));
  gfx->fillRoundRect(8, 8, 74, 36, 6, RGB565_BLACK);
  centerText(b, 45, 26, 3, RGB565_RED);
  uint32_t secs = state == READY ? 0 : ((state == PLAYING ? millis() : endMs) - startMs) / 1000;
  snprintf(b, sizeof b, "%03u", (unsigned)min<uint32_t>(secs, 999));
  gfx->fillRoundRect(238, 8, 74, 36, 6, RGB565_BLACK);
  centerText(b, 275, 26, 3, RGB565_RED);
  const char *st = state == LOST ? "BOOM!" : state == WON ? "YOU WIN!" : LEVELS[lvl].name;
  centerText(st, 160, 20, 2, state == LOST ? RGB565_RED : state == WON ? RGB565_GREEN : C_TXT);
  if (state == WON) snprintf(b, sizeof b, newBest ? "New best!" : "Best %ds", bestTime[lvl]);
  else if (bestTime[lvl]) snprintf(b, sizeof b, "Best %ds", bestTime[lvl]);
  else b[0] = 0;
  centerText(b, 160, 40, 1, C_TXT);

  // field
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      int x = OX + c * CELL, y = FIELD_Y + r * CELL;
      bool showMine = state == LOST && mine[r][c] && !flag[r][c];
      if (open_[r][c] || showMine) {
        gfx->fillRect(x, y, CELL - 1, CELL - 1, (r == boomR && c == boomC) ? RGB565_RED : C_OPEN);
        if (mine[r][c]) drawMine(x, y, CELL, (r == boomR && c == boomC) ? RGB565_RED : C_OPEN);
        else if (nbr[r][c]) {
          char n[2] = {(char)('0' + nbr[r][c]), 0};
          centerText(n, x + CELL / 2, y + CELL / 2, CELL >= 30 ? 3 : 2, NUMC[nbr[r][c]]);
        }
      } else {
        gfx->fillRect(x, y, CELL - 1, CELL - 1, C_HID);
        gfx->drawFastHLine(x, y, CELL - 1, C_HID_HI); gfx->drawFastVLine(x, y, CELL - 1, C_HID_HI);
        gfx->drawFastHLine(x, y + CELL - 2, CELL - 1, C_HID_LO); gfx->drawFastVLine(x + CELL - 2, y, CELL - 1, C_HID_LO);
        if (flag[r][c]) {
          drawFlag(x, y, CELL);
          if (state == LOST && !mine[r][c]) { gfx->drawLine(x + 3, y + 3, x + CELL - 5, y + CELL - 5, RGB565_RED); gfx->drawLine(x + CELL - 5, y + 3, x + 3, y + CELL - 5, RGB565_RED); }
        }
      }
    }

  drawBtn(8, 98, flagMode ? "FLAG" : "DIG", flagMode);
  drawBtn(111, 98, LEVELS[lvl].name);
  drawBtn(214, 98, "NEW");
  gfx->flush();
}

// ---------------------------------------------------------------- touch
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

static bool cellAt(int x, int y, int &r, int &c) {
  if (x < OX || y < FIELD_Y) return false;
  c = (x - OX) / CELL; r = (y - FIELD_Y) / CELL;
  return r < ROWS && c < COLS;
}

static void handleTap(int x, int y) {
  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (x < 108) flagMode = !flagMode;
    else if (x < 211) { setLevel((lvl + 1) % 3); prefs.putInt("lvl", lvl); newGame(); }
    else newGame();
    render();
    return;
  }
  int r, c;
  if (!cellAt(x, y, r, c)) return;
  if (flagMode) toggleFlag(r, c); else dig(r, c);
  render();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C minesweeper boot");
  randomSeed(esp_random());
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(60, 64, 72);
  C_HID = gfx->color565(150, 156, 168);
  C_HID_HI = gfx->color565(210, 214, 224);
  C_HID_LO = gfx->color565(95, 100, 112);
  C_OPEN = gfx->color565(205, 208, 216);
  C_TXT = RGB565_WHITE;
  C_BTN = gfx->color565(40, 56, 92);
  C_BTN_ON = gfx->color565(190, 90, 40);
  const uint8_t nc[9][3] = {{0,0,0},{0,0,255},{0,128,0},{220,0,0},{0,0,128},{128,0,0},{0,128,128},{0,0,0},{100,100,100}};
  for (int i = 0; i < 9; i++) NUMC[i] = gfx->color565(nc[i][0], nc[i][1], nc[i][2]);

  prefs.begin("mines", false);
  for (int i = 0; i < 3; i++) { char k[8]; snprintf(k, sizeof k, "best%d", i); bestTime[i] = prefs.getInt(k, 0); }
  setLevel(constrain(prefs.getInt("lvl", 0), 0, 2));
  newGame();
  render();
}

void loop() {
  static bool touching = false, longFired = false;
  static int sx, sy, lx, ly, misses;
  static uint32_t t0, lastSec = 0;
  int x, y;

  if (readTouch(x, y)) {
    misses = 0;
    if (!touching) { touching = true; longFired = false; sx = lx = x; sy = ly = y; t0 = millis(); }
    lx = x; ly = y;
    if (!longFired && millis() - t0 > LONG_MS && abs(lx - sx) < 15 && abs(ly - sy) < 15) {
      int r, c;
      if (cellAt(sx, sy, r, c)) {                       // long-press = flag (or unflag)
        longFired = true;
        toggleFlag(r, c);
        render();
      }
    }
  } else if (touching && ++misses >= 6) {              // finger lifted: a short, still touch is a tap
    touching = false;
    if (!longFired && millis() - t0 < LONG_MS + 200 && abs(lx - sx) < 20 && abs(ly - sy) < 20) handleTap(sx, sy);
  }

  if (state == PLAYING && millis() / 1000 != lastSec) {   // refresh the timer once a second
    lastSec = millis() / 1000;
    render();
  }
  delay(4);
}
