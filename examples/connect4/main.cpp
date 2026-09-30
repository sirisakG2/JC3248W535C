// Connect Four for the JC3248W535C. Tap a column to drop your disc (red).
// Modes (tap MODE): CPU Hard (alpha-beta search), CPU Easy, 2 Players. Scores saved in flash.
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

#define ROWS 6
#define COLS 7
#define HARD_DEPTH 7
#define EASY_DEPTH 2

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

const int BX = 6, BY = 90, CELL = 44;          // board: 308 x 264
const int BTN_Y = 420, BTN_H = 40;
uint16_t C_BG, C_BOARD, C_P1, C_P2, C_TXT, C_WIN, C_BTN;

enum Mode { CPU_HARD, CPU_EASY, TWO_PLAYERS };
Mode mode = CPU_HARD;
int8_t b[ROWS][COLS];          // 0 empty, 1 red (human), 2 yellow
int height[COLS];              // discs per column
int turn = 1, starter = 2;     // starter alternates each game
bool over = false;
int wins[4][2];                // winning cells (row, col)
bool hasWin = false;
int score1 = 0, score2 = 0, scoreD = 0;
uint32_t aiAt = 0;

static bool vsCpu() { return mode != TWO_PLAYERS; }
static const char *modeName() { return mode == CPU_HARD ? "CPU: Hard" : mode == CPU_EASY ? "CPU: Easy" : "2 Players"; }

// ---------------------------------------------------------------- game logic
// Does the disc at (r,c) belong to a line of 4? Optionally records the cells.
static bool winsAt(int r, int c, int p, bool record = false) {
  static const int D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
  for (auto &d : D) {
    int n = 1;
    for (int s = -1; s <= 1; s += 2)
      for (int k = 1; k < 4; k++) {
        int rr = r + d[0] * k * s, cc = c + d[1] * k * s;
        if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS || b[rr][cc] != p) break;
        n++;
      }
    if (n >= 4) {
      if (record) {                      // collect the run through (r,c)
        int cnt = 0;
        int back = 0;
        while (true) {
          int rr = r - d[0] * (back + 1), cc = c - d[1] * (back + 1);
          if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS || b[rr][cc] != p) break;
          back++;
        }
        for (int k = 0; k < 4 && cnt < 4; k++) { wins[cnt][0] = r - d[0] * back + d[0] * k; wins[cnt][1] = c - d[1] * back + d[1] * k; cnt++; }
      }
      return true;
    }
  }
  return false;
}

static bool boardFull() { for (int c = 0; c < COLS; c++) if (height[c] < ROWS) return false; return true; }
static int dropRow(int c) { return ROWS - 1 - height[c]; }

// static evaluation from player 2's (CPU) point of view
static int evalBoard() {
  static const int D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
  int score = 0;
  for (int r = 0; r < ROWS; r++) {
    if (b[r][3] == 2) score += 3; else if (b[r][3] == 1) score -= 3;   // centre column is valuable
    for (int c = 0; c < COLS; c++)
      for (auto &d : D) {
        int er = r + d[0] * 3, ec = c + d[1] * 3;
        if (er < 0 || er >= ROWS || ec < 0 || ec >= COLS) continue;
        int n1 = 0, n2 = 0;
        for (int k = 0; k < 4; k++) { int v = b[r + d[0] * k][c + d[1] * k]; if (v == 1) n1++; else if (v == 2) n2++; }
        if (n1 && n2) continue;
        if (n2 == 3) score += 100; else if (n2 == 2) score += 10;
        if (n1 == 3) score -= 90;  else if (n1 == 2) score -= 8;
      }
  }
  return score;
}

const int ORDER[COLS] = {3, 2, 4, 1, 5, 0, 6};   // centre first = better pruning

// negamax with alpha-beta; returns score from `p`'s point of view
static int negamax(int p, int depth, int alpha, int beta) {
  if (depth == 0) return p == 2 ? evalBoard() : -evalBoard();
  bool any = false;
  int best = -1000000;
  for (int oc = 0; oc < COLS; oc++) {
    int c = ORDER[oc];
    if (height[c] >= ROWS) continue;
    any = true;
    int r = dropRow(c);
    b[r][c] = p; height[c]++;
    int s = winsAt(r, c, p) ? 100000 + depth : -negamax(3 - p, depth - 1, -beta, -alpha);
    b[r][c] = 0; height[c]--;
    if (s > best) best = s;
    if (best > alpha) alpha = best;
    if (alpha >= beta) break;
  }
  return any ? best : 0;
}

static int aiPick() {
  int depth = mode == CPU_HARD ? HARD_DEPTH : EASY_DEPTH;
  int cols[COLS], n = 0, best = -1000001;
  for (int oc = 0; oc < COLS; oc++) {
    int c = ORDER[oc];
    if (height[c] >= ROWS) continue;
    int r = dropRow(c);
    b[r][c] = 2; height[c]++;
    int s = winsAt(r, c, 2) ? 100000 + depth : -negamax(1, depth - 1, -1000000, 1000000);
    b[r][c] = 0; height[c]--;
    if (s > best) { best = s; n = 0; }
    if (s == best) cols[n++] = c;
  }
  if (mode == CPU_EASY && random(4) == 0) {           // easy sometimes plays randomly
    int all[COLS], m = 0;
    for (int c = 0; c < COLS; c++) if (height[c] < ROWS) all[m++] = c;
    return all[random(m)];
  }
  return cols[random(n)];
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static uint16_t discColor(int p) { return p == 1 ? C_P1 : C_P2; }

static void drawButton(int x, int w, const char *label) {
  gfx->fillRoundRect(x, BTN_Y, w, BTN_H, 8, C_BTN);
  centerText(label, x + w / 2, BTN_Y + BTN_H / 2, 2, C_TXT);
}

// falling disc: player p at pixel row y (centre) in column fc; fc < 0 = none
static void render(int fc = -1, int fy = 0, int fp = 0) {
  gfx->fillScreen(C_BG);

  // status line
  char buf[24];
  const char *st;
  if (over) {
    if (hasWin) { snprintf(buf, sizeof buf, "%s wins!", b[wins[0][0]][wins[0][1]] == 1 ? "Red" : "Yellow"); st = buf; }
    else st = "Draw!";
  } else if (vsCpu() && turn == 2) st = "Thinking...";
  else { snprintf(buf, sizeof buf, "%s's turn", turn == 1 ? "Red" : "Yellow"); st = buf; }
  centerText(st, SCR_W / 2 + 14, 34, 3, C_TXT);
  gfx->fillCircle(SCR_W / 2 - strlen(st) * 9 - 6, 34, 11, over ? (hasWin ? discColor(b[wins[0][0]][wins[0][1]]) : C_BOARD) : discColor(turn));

  // falling disc goes behind the board
  if (fc >= 0) gfx->fillCircle(BX + fc * CELL + CELL / 2, fy, CELL / 2 - 5, discColor(fp));

  // board with holes
  gfx->fillRoundRect(BX, BY, COLS * CELL, ROWS * CELL, 10, C_BOARD);
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      int cx = BX + c * CELL + CELL / 2, cy = BY + r * CELL + CELL / 2;
      gfx->fillCircle(cx, cy, CELL / 2 - 5, b[r][c] ? discColor(b[r][c]) : C_BG);
    }
  if (hasWin)
    for (int i = 0; i < 4; i++) {
      int cx = BX + wins[i][1] * CELL + CELL / 2, cy = BY + wins[i][0] * CELL + CELL / 2;
      for (int t = 0; t < 3; t++) gfx->drawCircle(cx, cy, CELL / 2 - 5 - t, C_WIN);
    }

  // scores
  gfx->setTextSize(2);
  gfx->setTextColor(C_P1);  gfx->setCursor(14, 384);  gfx->printf("Red:%d", score1);
  gfx->setTextColor(C_TXT); gfx->setCursor(125, 384); gfx->printf("Draw:%d", scoreD);
  gfx->setTextColor(C_P2);  gfx->setCursor(215, 384); gfx->printf("Yel:%d", score2);

  drawButton(8, 140, "NEW GAME");
  drawButton(158, 154, modeName());
  gfx->flush();
}

static void animateDrop(int c, int r, int p) {
  int targetY = BY + r * CELL + CELL / 2;
  float y = BY - CELL / 2, v = 4;
  while (y < targetY) {
    render(c, (int)y, p);
    y += v;
    v += 2.2f;                 // gravity
  }
}

// ---------------------------------------------------------------- game flow
static void saveScores() {
  prefs.putInt("s1", score1); prefs.putInt("s2", score2); prefs.putInt("sd", scoreD); prefs.putInt("mode", mode);
}

static void newGame() {
  memset(b, 0, sizeof b);
  memset(height, 0, sizeof height);
  memset(wins, 0, sizeof wins);
  hasWin = false; over = false;
  starter = 3 - starter;
  turn = starter;
  aiAt = (vsCpu() && turn == 2) ? millis() + 500 : 0;
  render();
}

static void play(int c) {
  int r = dropRow(c), p = turn;
  animateDrop(c, r, p);
  b[r][c] = p; height[c]++;
  if (winsAt(r, c, p, true)) { hasWin = true; over = true; (p == 1 ? score1 : score2)++; saveScores(); }
  else if (boardFull()) { over = true; scoreD++; saveScores(); }
  else turn = 3 - p;
  aiAt = (!over && vsCpu() && turn == 2) ? millis() + 300 : 0;
  render();
}

static void handleTap(int x, int y) {
  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (x < 152) newGame();
    else {
      mode = (Mode)((mode + 1) % 3);
      score1 = score2 = scoreD = 0;
      saveScores();
      starter = 1;               // newGame flips to red
      newGame();
    }
    return;
  }
  if (over) { newGame(); return; }
  if (vsCpu() && turn != 1) return;
  if (x < BX || x >= BX + COLS * CELL || y < 60 || y >= BTN_Y) return;
  int c = (x - BX) / CELL;
  if (height[c] < ROWS) play(c);
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
  Serial.println("JC3248W535C connect4 boot");
  randomSeed(esp_random());
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(16, 24, 40);
  C_BOARD = gfx->color565(30, 70, 170);
  C_P1 = gfx->color565(230, 60, 60);
  C_P2 = gfx->color565(250, 210, 40);
  C_TXT = RGB565_WHITE;
  C_WIN = RGB565_WHITE;
  C_BTN = gfx->color565(40, 60, 96);

  prefs.begin("c4", false);
  score1 = prefs.getInt("s1", 0); score2 = prefs.getInt("s2", 0); scoreD = prefs.getInt("sd", 0);
  mode = (Mode)constrain(prefs.getInt("mode", CPU_HARD), 0, 2);
  starter = 2;
  newGame();
}

void loop() {
  static bool down = false;
  static int misses;
  int x, y;
  if (readTouch(x, y)) {
    misses = 0;
    if (!down) { down = true; handleTap(x, y); }
  } else if (down && ++misses >= 3) {
    down = false;
  }
  if (aiAt && millis() >= aiAt && !over) {
    aiAt = 0;
    uint32_t t = millis();
    int c = aiPick();
    Serial.printf("cpu move col %d (%u ms)\n", c, (unsigned)(millis() - t));
    play(c);
  }
  delay(5);
}
