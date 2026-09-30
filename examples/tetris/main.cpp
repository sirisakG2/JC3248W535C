// Tetris for the JC3248W535C. 10x20 field, 7-bag randomiser, ghost piece, next preview,
// levels (speed ramps every 10 lines). On-screen buttons: < ROT > / DOWN DROP PAUSE.
// < > DOWN auto-repeat while held. Tap the field to start / restart. Best score saved in flash.
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

#define COLS 10
#define ROWS 20
#define CELL 18
#define FX 8
#define FY 8

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

// ---------------------------------------------------------------- pieces
// Base cells (x,y) inside an n x n box; rotation turns them clockwise inside the box.
const int8_t SHAPE[7][4][2] = {
  {{0,1},{1,1},{2,1},{3,1}},  // I
  {{0,0},{1,0},{0,1},{1,1}},  // O
  {{1,0},{0,1},{1,1},{2,1}},  // T
  {{1,0},{2,0},{0,1},{1,1}},  // S
  {{0,0},{1,0},{1,1},{2,1}},  // Z
  {{0,0},{0,1},{1,1},{2,1}},  // J
  {{2,0},{0,1},{1,1},{2,1}},  // L
};
const int8_t BOX[7] = {4, 2, 3, 3, 3, 3, 3};
uint16_t COLOR[8];           // [1..7] piece colours, [0] unused
uint16_t C_BG, C_FIELD, C_GRID, C_TXT, C_BTN, C_GHOST;

static void cellsOf(int t, int rot, int (*out)[2]) {
  int n = BOX[t];
  for (int i = 0; i < 4; i++) {
    int x = SHAPE[t][i][0], y = SHAPE[t][i][1];
    for (int r = 0; r < rot; r++) { int nx = n - 1 - y, ny = x; x = nx; y = ny; }
    out[i][0] = x; out[i][1] = y;
  }
}

// ---------------------------------------------------------------- state
enum State { READY, PLAYING, PAUSED, DEAD };
State state = READY;
uint8_t field[ROWS][COLS];   // 0 empty, else piece type + 1
int curT, curRot, curX, curY, nextT;
int bag[7], bagPos = 7;
int score = 0, best = 0, lines = 0, level = 1;
uint32_t lastDrop = 0;
int flashRows[4], nFlash = 0;

static int nextFromBag() {
  if (bagPos >= 7) {                                   // refill and shuffle
    for (int i = 0; i < 7; i++) bag[i] = i;
    for (int i = 6; i > 0; i--) { int j = random(i + 1); int t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
    bagPos = 0;
  }
  return bag[bagPos++];
}

static bool valid(int t, int rot, int px, int py) {
  int c[4][2];
  cellsOf(t, rot, c);
  for (int i = 0; i < 4; i++) {
    int x = px + c[i][0], y = py + c[i][1];
    if (x < 0 || x >= COLS || y >= ROWS) return false;
    if (y >= 0 && field[y][x]) return false;
  }
  return true;
}

static int dropMs() { return max(90, 800 - (level - 1) * 70); }

static bool spawn() {
  curT = nextT;
  nextT = nextFromBag();
  curRot = 0;
  curX = (COLS - BOX[curT]) / 2;
  curY = curT == 0 ? -1 : 0;
  return valid(curT, curRot, curX, curY);
}

static void newGame() {
  memset(field, 0, sizeof field);
  score = 0; lines = 0; level = 1;
  bagPos = 7;
  nextT = nextFromBag();
  spawn();
  state = PLAYING;
  lastDrop = millis();
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static void drawCell(int x, int y, int px, int py, int size, uint16_t col) {
  gfx->fillRect(px + x * size, py + y * size, size - 1, size - 1, col);
}

struct Btn { int x, y, w, h; const char *label; };
const Btn BTNS[6] = {
  {8, 382, 98, 44, "<"},     {111, 382, 98, 44, "ROT"},  {214, 382, 98, 44, ">"},
  {8, 432, 98, 44, "v"},     {111, 432, 98, 44, "DROP"}, {214, 432, 98, 44, "PAUSE"},
};
enum { B_LEFT, B_ROT, B_RIGHT, B_DOWN, B_DROP, B_PAUSE, B_FIELD = 100, B_NONE = -1 };

static int buttonAt(int x, int y) {
  for (int i = 0; i < 6; i++)
    if (x >= BTNS[i].x && x < BTNS[i].x + BTNS[i].w && y >= BTNS[i].y && y < BTNS[i].y + BTNS[i].h) return i;
  if (y < 378) return B_FIELD;
  return B_NONE;
}

static void drawSideValue(const char *label, int value, int y) {
  char b[16];
  gfx->setTextSize(1);
  gfx->setTextColor(C_GRID);
  gfx->setCursor(200, y);
  gfx->print(label);
  snprintf(b, sizeof b, "%d", value);
  gfx->setTextSize(2);
  gfx->setTextColor(C_TXT);
  gfx->setCursor(200, y + 12);
  gfx->print(b);
}

static void render() {
  gfx->fillScreen(C_BG);
  gfx->fillRect(FX - 2, FY - 2, COLS * CELL + 3, ROWS * CELL + 3, C_GRID);
  gfx->fillRect(FX, FY, COLS * CELL - 1, ROWS * CELL - 1, C_FIELD);

  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (field[r][c]) drawCell(c, r, FX, FY, CELL, COLOR[field[r][c]]);

  for (int i = 0; i < nFlash; i++) gfx->fillRect(FX, FY + flashRows[i] * CELL, COLS * CELL - 1, CELL - 1, RGB565_WHITE);

  if (state == PLAYING || state == PAUSED) {
    int c[4][2];
    int gy = curY;                                        // ghost: where it would land
    while (valid(curT, curRot, curX, gy + 1)) gy++;
    cellsOf(curT, curRot, c);
    if (gy != curY)
      for (int i = 0; i < 4; i++) {
        int y = gy + c[i][1];
        if (y >= 0) gfx->drawRect(FX + (curX + c[i][0]) * CELL, FY + y * CELL, CELL - 1, CELL - 1, COLOR[curT + 1]);
      }
    for (int i = 0; i < 4; i++) {
      int y = curY + c[i][1];
      if (y >= 0) drawCell(curX + c[i][0], y, FX, FY, CELL, COLOR[curT + 1]);
    }
  }

  // side panel
  drawSideValue("SCORE", score, 8);
  drawSideValue("LEVEL", level, 52);
  drawSideValue("LINES", lines, 96);
  drawSideValue("BEST", best, 140);
  gfx->setTextSize(1);
  gfx->setTextColor(C_GRID);
  gfx->setCursor(200, 190);
  gfx->print("NEXT");
  gfx->fillRect(200, 204, 108, 70, C_FIELD);
  int nc[4][2];
  cellsOf(nextT, 0, nc);
  int size = 16, minX = 9, maxX = -9, minY = 9, maxY = -9;
  for (int i = 0; i < 4; i++) { minX = min(minX, nc[i][0]); maxX = max(maxX, nc[i][0]); minY = min(minY, nc[i][1]); maxY = max(maxY, nc[i][1]); }
  int ox = 200 + (108 - (maxX - minX + 1) * size) / 2, oy = 204 + (70 - (maxY - minY + 1) * size) / 2;
  for (int i = 0; i < 4; i++) drawCell(nc[i][0] - minX, nc[i][1] - minY, ox, oy, size, COLOR[nextT + 1]);

  // buttons
  for (int i = 0; i < 6; i++) {
    const Btn &b = BTNS[i];
    gfx->fillRoundRect(b.x, b.y, b.w, b.h, 8, C_BTN);
    const char *label = (i == B_PAUSE && state == PAUSED) ? "PLAY" : b.label;
    centerText(label, b.x + b.w / 2, b.y + b.h / 2, i == B_LEFT || i == B_RIGHT || i == B_DOWN ? 3 : 2, C_TXT);
  }

  if (state != PLAYING) {
    gfx->fillRoundRect(FX + 10, 150, COLS * CELL - 20, 84, 10, C_BG);
    gfx->drawRoundRect(FX + 10, 150, COLS * CELL - 20, 84, 10, C_TXT);
    int cx = FX + COLS * CELL / 2;
    if (state == DEAD) { centerText("GAME OVER", cx, 174, 2, C_TXT); centerText("Tap field", cx, 205, 2, C_TXT); }
    else if (state == PAUSED) { centerText("PAUSED", cx, 192, 2, C_TXT); }
    else { centerText("TETRIS", cx, 174, 3, C_TXT); centerText("Tap to start", cx, 208, 1, C_TXT); }
  }
  gfx->flush();
}

// ---------------------------------------------------------------- game actions
static void clearLines() {
  nFlash = 0;
  for (int r = 0; r < ROWS; r++) {
    bool full = true;
    for (int c = 0; c < COLS; c++) if (!field[r][c]) { full = false; break; }
    if (full) flashRows[nFlash++] = r;
  }
  if (!nFlash) return;
  render();                        // flash the rows briefly
  delay(140);
  for (int i = 0; i < nFlash; i++) {
    int r = flashRows[i];
    for (int y = r; y > 0; y--) memcpy(field[y], field[y - 1], COLS);
    memset(field[0], 0, COLS);
  }
  static const int PTS[5] = {0, 100, 300, 500, 800};
  score += PTS[nFlash] * level;
  lines += nFlash;
  level = lines / 10 + 1;
  nFlash = 0;
}

static void lockPiece() {
  int c[4][2];
  cellsOf(curT, curRot, c);
  for (int i = 0; i < 4; i++) {
    int y = curY + c[i][1], x = curX + c[i][0];
    if (y >= 0) field[y][x] = curT + 1;
  }
  clearLines();
  if (!spawn()) {
    state = DEAD;
    if (score > best) { best = score; prefs.putInt("best", best); }
  }
  lastDrop = millis();
}

static bool tryMove(int dx, int dy) {
  if (!valid(curT, curRot, curX + dx, curY + dy)) return false;
  curX += dx; curY += dy;
  return true;
}

static void rotate() {
  int nr = (curRot + 1) % 4;
  static const int KICKS[5] = {0, -1, 1, -2, 2};       // simple wall kicks
  for (int k : KICKS)
    if (valid(curT, nr, curX + k, curY)) { curRot = nr; curX += k; return; }
}

static void act(int btn) {
  if (btn == B_FIELD) { if (state == READY || state == DEAD) newGame(); render(); return; }
  if (btn == B_PAUSE) {
    if (state == PLAYING) state = PAUSED;
    else if (state == PAUSED) { state = PLAYING; lastDrop = millis(); }
    render();
    return;
  }
  if (state != PLAYING) return;
  switch (btn) {
    case B_LEFT: tryMove(-1, 0); break;
    case B_RIGHT: tryMove(1, 0); break;
    case B_ROT: rotate(); break;
    case B_DOWN: if (tryMove(0, 1)) { score += 1; lastDrop = millis(); } else lockPiece(); break;
    case B_DROP: { int n = 0; while (tryMove(0, 1)) n++; score += 2 * n; lockPiece(); break; }
  }
  render();
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C tetris boot");
  randomSeed(esp_random());
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(16, 20, 32);
  C_FIELD = gfx->color565(8, 10, 18);
  C_GRID = gfx->color565(90, 100, 130);
  C_TXT = RGB565_WHITE;
  C_BTN = gfx->color565(40, 56, 92);
  const uint8_t cols[8][3] = {{0,0,0},{0,220,230},{240,220,0},{170,60,220},{60,210,80},{230,50,50},{50,90,240},{245,150,30}};
  for (int i = 0; i < 8; i++) COLOR[i] = gfx->color565(cols[i][0], cols[i][1], cols[i][2]);

  prefs.begin("tetris", false);
  best = prefs.getInt("best", 0);
  nextT = nextFromBag();
  spawn();
  render();
}

void loop() {
  static int held = B_NONE, misses = 0;
  static bool touching = false;
  static uint32_t pressT = 0, repT = 0, lastAct = 0;
  int x, y;

  if (readTouch(x, y)) {
    misses = 0;
    int b = buttonAt(x, y);
    uint32_t now = millis();
    if (!touching) {                                    // a new press only counts after a full lift
      touching = true;
      held = b; pressT = repT = now;
      if (b != B_NONE && now - lastAct > 90) { lastAct = now; act(b); }
    } else if (b == held && (b == B_LEFT || b == B_RIGHT || b == B_DOWN) && now - pressT > 320) {
      uint32_t gap = b == B_DOWN ? 90 : 130;            // hold to repeat, slower than before
      if (now - repT >= gap) { repT = now; act(b); }
    }                                                   // sliding onto another button does nothing
  } else if (touching && ++misses >= 6) {               // ~6 empty polls = finger really lifted
    touching = false;
    held = B_NONE;
  }

  if (state == PLAYING && millis() - lastDrop >= (uint32_t)dropMs()) {
    lastDrop = millis();
    if (!tryMove(0, 1)) lockPiece();
    render();
  }
  delay(3);
}
