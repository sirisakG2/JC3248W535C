// 2048 for the JC3248W535C. Swipe to slide tiles; equal tiles merge.
// NEW restarts, UNDO reverts the last move. Board, score and best are saved in flash.
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
#define SWIPE_MIN 40

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

const int GX = 10, GY = 120, GSZ = 300, GAP = 8, TILE = (GSZ - 5 * GAP) / 4;  // 65px tiles
const int BTN_Y = 440, BTN_H = 36;

int grid[4][4];              // tile values (0 = empty)
int prevGrid[4][4];
int score = 0, prevScore = 0, best = 0;
bool canUndo = false, over = false, won = false, keepGoing = false;
int newR = -1, newC = -1;    // last spawned tile (highlighted)

uint16_t C_BG, C_GRID, C_EMPTY, C_TXT_DARK, C_TXT_LIGHT, C_BTN, C_BOX;
uint16_t tileColor[12];      // index = log2(value), 1..11

// ---------------------------------------------------------------- logic
static void spawn() {
  int er[16], ec[16], n = 0;
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (!grid[r][c]) { er[n] = r; ec[n] = c; n++; }
  if (!n) return;
  int k = random(n);
  grid[er[k]][ec[k]] = random(10) == 0 ? 4 : 2;
  newR = er[k]; newC = ec[k];
}

// Slide one line of 4 toward index 0, merging equal neighbours once. Returns true if changed.
static bool slideLine(int *v, int &gained) {
  int out[4] = {0}, n = 0;
  bool merged = false;
  for (int i = 0; i < 4; i++) {
    if (!v[i]) continue;
    if (n && out[n - 1] == v[i] && !merged) { out[n - 1] *= 2; gained += out[n - 1]; merged = true; }
    else { out[n++] = v[i]; merged = false; }
  }
  bool changed = false;
  for (int i = 0; i < 4; i++) { if (v[i] != out[i]) changed = true; v[i] = out[i]; }
  return changed;
}

enum Dir { LEFT, RIGHT, UP, DOWN };

static bool move(Dir d) {
  int backup[4][4], gained = 0;
  memcpy(backup, grid, sizeof grid);
  bool changed = false;
  for (int line = 0; line < 4; line++) {
    int v[4];
    for (int i = 0; i < 4; i++) {           // read line so index 0 is the side we slide toward
      int r = d == UP ? i : d == DOWN ? 3 - i : line;
      int c = d == LEFT ? i : d == RIGHT ? 3 - i : line;
      v[i] = grid[r][c];
    }
    changed |= slideLine(v, gained);
    for (int i = 0; i < 4; i++) {
      int r = d == UP ? i : d == DOWN ? 3 - i : line;
      int c = d == LEFT ? i : d == RIGHT ? 3 - i : line;
      grid[r][c] = v[i];
    }
  }
  if (!changed) return false;
  memcpy(prevGrid, backup, sizeof grid);
  prevScore = score;
  canUndo = true;
  score += gained;
  if (score > best) best = score;
  spawn();
  return true;
}

static bool hasMoves() {
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    if (!grid[r][c]) return true;
    if (c < 3 && grid[r][c] == grid[r][c + 1]) return true;
    if (r < 3 && grid[r][c] == grid[r + 1][c]) return true;
  }
  return false;
}

static void save() {
  prefs.putBytes("grid", grid, sizeof grid);
  prefs.putInt("score", score);
  prefs.putInt("best", best);
  prefs.putBool("kg", keepGoing);
}

static void newGame() {
  memset(grid, 0, sizeof grid);
  score = 0; canUndo = false; over = false; won = false; keepGoing = false;
  spawn(); spawn();
  newR = newC = -1;
  save();
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static void drawBox(int x, int w, const char *label, int value) {
  gfx->fillRoundRect(x, 20, w, 56, 6, C_BOX);
  char b[16];
  centerText(label, x + w / 2, 33, 1, C_TXT_LIGHT);
  snprintf(b, sizeof b, "%d", value);
  centerText(b, x + w / 2, 58, 2, RGB565_WHITE);
}

static void drawButton(int x, int w, const char *label, bool enabled) {
  gfx->fillRoundRect(x, BTN_Y, w, BTN_H, 6, enabled ? C_BTN : C_BOX);
  centerText(label, x + w / 2, BTN_Y + BTN_H / 2, 2, enabled ? RGB565_WHITE : C_GRID);
}

static void render() {
  gfx->fillScreen(C_BG);
  gfx->setTextSize(5);
  gfx->setTextColor(C_TXT_DARK);
  gfx->setCursor(10, 30);
  gfx->print("2048");
  drawBox(140, 84, "SCORE", score);
  drawBox(230, 80, "BEST", best);
  centerText("Swipe to merge tiles", SCR_W / 2, 102, 1, C_TXT_DARK);

  gfx->fillRoundRect(GX, GY, GSZ, GSZ, 8, C_GRID);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int x = GX + GAP + c * (TILE + GAP), y = GY + GAP + r * (TILE + GAP);
      int v = grid[r][c];
      if (!v) { gfx->fillRoundRect(x, y, TILE, TILE, 5, C_EMPTY); continue; }
      int lg = 0;
      for (int t = v; t > 1; t >>= 1) lg++;
      gfx->fillRoundRect(x, y, TILE, TILE, 5, tileColor[min(lg, 11)]);
      if (r == newR && c == newC) gfx->drawRoundRect(x, y, TILE, TILE, 5, RGB565_WHITE);
      char b[8];
      snprintf(b, sizeof b, "%d", v);
      int size = v < 100 ? 4 : v < 1000 ? 3 : 2;
      centerText(b, x + TILE / 2, y + TILE / 2, size, v <= 4 ? C_TXT_DARK : RGB565_WHITE);
    }
  }
  drawButton(10, 140, "NEW", true);
  drawButton(170, 140, "UNDO", canUndo);

  if (over || (won && !keepGoing)) {
    gfx->fillRoundRect(GX + 20, GY + 110, GSZ - 40, 80, 10, C_BOX);
    centerText(over ? "Game over" : "You win!", SCR_W / 2, GY + 135, 3, RGB565_WHITE);
    centerText(over ? "Tap NEW to retry" : "Tap board to continue", SCR_W / 2, GY + 172, 1, C_TXT_LIGHT);
  }
  gfx->flush();
}

// ---------------------------------------------------------------- touch
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

static void doMove(Dir d) {
  if (over || (won && !keepGoing)) return;
  if (!move(d)) return;
  if (!won) for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (grid[r][c] >= 2048) won = true;
  if (!hasMoves()) over = true;
  save();
  render();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C 2048 boot");
  randomSeed(esp_random());
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(250, 248, 239);
  C_GRID = gfx->color565(187, 173, 160);
  C_EMPTY = gfx->color565(205, 193, 180);
  C_TXT_DARK = gfx->color565(119, 110, 101);
  C_TXT_LIGHT = gfx->color565(238, 228, 218);
  C_BTN = gfx->color565(143, 122, 102);
  C_BOX = gfx->color565(187, 173, 160);
  const uint8_t cols[12][3] = {{0,0,0},{238,228,218},{237,224,200},{242,177,121},{245,149,99},{246,124,95},{246,94,59},
                               {237,207,114},{237,204,97},{237,200,80},{237,197,63},{237,194,46}};
  for (int i = 0; i < 12; i++) tileColor[i] = gfx->color565(cols[i][0], cols[i][1], cols[i][2]);

  prefs.begin("g2048", false);
  best = prefs.getInt("best", 0);
  if (prefs.getBytes("grid", grid, sizeof grid) == sizeof grid) {
    score = prefs.getInt("score", 0);
    keepGoing = prefs.getBool("kg", false);
    over = !hasMoves();
  } else newGame();
  render();
}

void loop() {
  static bool down = false, fired = false;
  static int sx, sy, misses;
  int x, y;
  if (readTouch(x, y)) {
    misses = 0;
    if (!down) {
      down = true; fired = false; sx = x; sy = y;
      if (y >= BTN_Y && y < BTN_Y + BTN_H) {                   // buttons act on press
        fired = true;
        if (x < 160) { newGame(); render(); }
        else if (canUndo && !over) { memcpy(grid, prevGrid, sizeof grid); score = prevScore; canUndo = false; newR = newC = -1; save(); render(); }
      } else if (won && !keepGoing && !over) { keepGoing = true; fired = true; save(); render(); }
    } else if (!fired) {                                        // swipe fires as soon as it is long enough
      int dx = x - sx, dy = y - sy;
      if (max(abs(dx), abs(dy)) >= SWIPE_MIN) {
        fired = true;
        if (abs(dx) > abs(dy)) doMove(dx < 0 ? LEFT : RIGHT);
        else doMove(dy < 0 ? UP : DOWN);
      }
    }
  } else if (down && ++misses >= 3) {
    down = false;
  }
  delay(5);
}
