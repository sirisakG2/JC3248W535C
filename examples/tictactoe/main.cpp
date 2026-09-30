// Tic-tac-toe for the JC3248W535C.
// Tap a cell to play X. Modes (tap MODE): vs Computer (Hard = unbeatable minimax),
// vs Computer (Easy = random), 2 Players, Forever 2P, Forever CPU. Scores are saved in flash.
// Forever modes: each player keeps at most 3 marks (placing a 4th removes the oldest,
// shown faded) and every move is timed (TURN_MS) - run out and you lose the turn.
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
#define TURN_MS 4000   // Forever modes: time allowed per move

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

// layout
const int BX = 10, BY = 90, CELL = 100;           // 3x3 board, 300x300
const int BTN_Y = 430, BTN_H = 40;
uint16_t C_BG, C_GRID, C_X, C_O, C_Xd, C_Od, C_TXT, C_WIN, C_BTN;

enum Mode { CPU_HARD, CPU_EASY, TWO_PLAYERS, FOREVER_2P, FOREVER_CPU, NUM_MODES };
Mode mode = CPU_HARD;
char board[9];                 // ' ', 'X', 'O'
char turn = 'X';
char startPlayer = 'X';        // alternates each game
int winLine = -1;              // index into LINES, -1 none
bool over = false;
int scoreX = 0, scoreO = 0, scoreD = 0;
uint32_t aiAt = 0;             // when the computer should move (0 = not pending)

const int LINES[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};

static const char *modeName() {
  static const char *names[NUM_MODES] = {"CPU: Hard", "CPU: Easy", "2 Players", "Forever 2P", "Forever CPU"};
  return names[mode];
}
static bool isForever() { return mode == FOREVER_2P || mode == FOREVER_CPU; }
static bool vsCpu() { return mode == CPU_HARD || mode == CPU_EASY || mode == FOREVER_CPU; }

// Forever mode: cells in the order each player placed them (oldest first)
int hist[2][3], hcount[2];
uint32_t turnStart = 0, slowUntil = 0;
static int pidx(char p) { return p == 'X' ? 0 : 1; }
static bool timedTurn() { return isForever() && !over && (mode == FOREVER_2P || turn == 'X'); }

// ---------------------------------------------------------------- game logic
static int findWin(const char *b, char p) {
  for (int i = 0; i < 8; i++)
    if (b[LINES[i][0]] == p && b[LINES[i][1]] == p && b[LINES[i][2]] == p) return i;
  return -1;
}
static bool full(const char *b) { for (int i = 0; i < 9; i++) if (b[i] == ' ') return false; return true; }

// score from O's point of view: +10 O wins, -10 X wins, faster wins score higher
static int minimax(char *b, char p, int depth) {
  if (findWin(b, 'O') >= 0) return 10 - depth;
  if (findWin(b, 'X') >= 0) return depth - 10;
  if (full(b)) return 0;
  int best = p == 'O' ? -100 : 100;
  for (int i = 0; i < 9; i++) {
    if (b[i] != ' ') continue;
    b[i] = p;
    int s = minimax(b, p == 'O' ? 'X' : 'O', depth + 1);
    b[i] = ' ';
    best = p == 'O' ? max(best, s) : min(best, s);
  }
  return best;
}

// Forever CPU: win if possible, else block, else centre > corners > edges.
static int aiForever() {
  auto wouldWin = [](char p, int c) {
    if (board[c] != ' ') return false;
    char b[9];
    memcpy(b, board, 9);
    int k = pidx(p);
    if (hcount[k] == 3) b[hist[k][0]] = ' ';
    b[c] = p;
    return findWin(b, p) >= 0;
  };
  int cells[9], n = 0;
  for (int i = 0; i < 9; i++) if (wouldWin('O', i)) return i;
  for (int i = 0; i < 9; i++) if (wouldWin('X', i)) return i;
  if (board[4] == ' ') return 4;
  for (int i : {0, 2, 6, 8}) if (board[i] == ' ') cells[n++] = i;
  if (n) return cells[random(n)];
  for (int i : {1, 3, 5, 7}) if (board[i] == ' ') cells[n++] = i;
  return cells[random(n)];
}

static int aiMove() {
  if (mode == FOREVER_CPU) return aiForever();
  int cells[9], n = 0;
  if (mode == CPU_EASY) {
    for (int i = 0; i < 9; i++) if (board[i] == ' ') cells[n++] = i;
    return cells[random(n)];
  }
  int best = -100;
  for (int i = 0; i < 9; i++) {
    if (board[i] != ' ') continue;
    board[i] = 'O';
    int s = minimax(board, 'X', 1);
    board[i] = ' ';
    if (s > best) { best = s; n = 0; }
    if (s == best) cells[n++] = i;   // keep ties so play varies
  }
  return cells[random(n)];
}

// ---------------------------------------------------------------- drawing
static void drawX(int cx, int cy, uint16_t c) {
  for (int o = -3; o <= 3; o++) {
    gfx->drawLine(cx - 28 + o, cy - 28, cx + 28 + o, cy + 28, c);
    gfx->drawLine(cx + 28 + o, cy - 28, cx - 28 + o, cy + 28, c);
  }
}
static void drawO(int cx, int cy, uint16_t c) {
  for (int r = 26; r <= 32; r++) gfx->drawCircle(cx, cy, r, c);
}
static void cellCenter(int i, int &cx, int &cy) { cx = BX + (i % 3) * CELL + CELL / 2; cy = BY + (i / 3) * CELL + CELL / 2; }

static void drawButton(int x, int w, const char *label) {
  gfx->fillRoundRect(x, BTN_Y, w, BTN_H, 8, C_BTN);
  gfx->setTextColor(C_TXT);
  gfx->setTextSize(2);
  int tw = strlen(label) * 12;
  gfx->setCursor(x + (w - tw) / 2, BTN_Y + 12);
  gfx->print(label);
}

static void render() {
  gfx->fillScreen(C_BG);

  // status
  gfx->setTextColor(C_TXT);
  gfx->setTextSize(3);
  const char *st;
  static char buf[32];
  if (over) {
    if (winLine >= 0) { snprintf(buf, sizeof buf, "%c wins!", board[LINES[winLine][0]]); st = buf; }
    else st = "Draw!";
  } else if (millis() < slowUntil) st = "Too slow!";
  else if (vsCpu() && turn == 'O') st = "Thinking...";
  else { snprintf(buf, sizeof buf, "%c's turn", turn); st = buf; }
  int w = strlen(st) * 18;
  gfx->setCursor((SCR_W - w) / 2, 28);
  gfx->print(st);

  // grid
  for (int i = 1; i < 3; i++) {
    for (int t = -2; t <= 2; t++) {
      gfx->drawFastVLine(BX + i * CELL + t, BY + 8, 3 * CELL - 16, C_GRID);
      gfx->drawFastHLine(BX + 8, BY + i * CELL + t, 3 * CELL - 16, C_GRID);
    }
  }
  // marks
  for (int i = 0; i < 9; i++) {
    int cx, cy;
    cellCenter(i, cx, cy);
    bool fading = false;   // this player's oldest mark vanishes on their next placement
    if (isForever() && !over) { int k = pidx(turn); fading = hcount[k] == 3 && hist[k][0] == i; }
    if (board[i] == 'X') drawX(cx, cy, fading ? C_Xd : C_X);
    else if (board[i] == 'O') drawO(cx, cy, fading ? C_Od : C_O);
  }
  if (timedTurn()) {
    int el = millis() - turnStart, left = max(0, TURN_MS - el);
    int w = 300 * left / TURN_MS;
    gfx->fillRect(10, 66, w, 8, left < TURN_MS * 3 / 10 ? RGB565_RED : C_WIN);
  }
  // win line
  if (winLine >= 0) {
    int ax, ay, bx, by;
    cellCenter(LINES[winLine][0], ax, ay);
    cellCenter(LINES[winLine][2], bx, by);
    for (int o = -3; o <= 3; o++) {
      gfx->drawLine(ax + o, ay, bx + o, by, C_WIN);
      gfx->drawLine(ax, ay + o, bx, by + o, C_WIN);
    }
  }

  // scoreboard
  gfx->setTextSize(2);
  gfx->setCursor(14, 402);
  gfx->setTextColor(C_X); gfx->printf("X:%d", scoreX);
  gfx->setTextColor(C_TXT); gfx->setCursor(120, 402); gfx->printf("Draw:%d", scoreD);
  gfx->setTextColor(C_O); gfx->setCursor(240, 402); gfx->printf("O:%d", scoreO);

  drawButton(10, 140, "NEW GAME");
  drawButton(160, 150, modeName());
  gfx->flush();
}

// ---------------------------------------------------------------- game flow
static void saveScores() {
  prefs.putInt("x", scoreX); prefs.putInt("o", scoreO); prefs.putInt("d", scoreD); prefs.putInt("mode", mode);
}

static void newGame() {
  for (int i = 0; i < 9; i++) board[i] = ' ';
  startPlayer = startPlayer == 'X' ? 'O' : 'X';
  if (vsCpu() && scoreX + scoreO + scoreD == 0) startPlayer = 'X';
  turn = startPlayer;
  winLine = -1;
  over = false;
  hcount[0] = hcount[1] = 0;
  turnStart = millis();
  aiAt = (vsCpu() && turn == 'O') ? millis() + 500 : 0;
  render();
}

static void place(int i) {
  if (isForever()) {
    int k = pidx(turn);
    if (hcount[k] == 3) {            // oldest mark vanishes
      board[hist[k][0]] = ' ';
      hist[k][0] = hist[k][1]; hist[k][1] = hist[k][2];
      hcount[k] = 2;
    }
    hist[k][hcount[k]++] = i;
  }
  turnStart = millis();
  board[i] = turn;
  winLine = findWin(board, turn);
  if (winLine >= 0) { over = true; (turn == 'X' ? scoreX : scoreO)++; saveScores(); }
  else if (!isForever() && full(board)) { over = true; scoreD++; saveScores(); }
  else turn = turn == 'X' ? 'O' : 'X';
  aiAt = (!over && vsCpu() && turn == 'O') ? millis() + 600 : 0;
  render();
}

static void handleTap(int x, int y) {
  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (x < 150) { newGame(); }
    else if (x >= 160) {
      mode = (Mode)((mode + 1) % NUM_MODES);
      scoreX = scoreO = scoreD = 0;   // scores are per mode
      saveScores();
      startPlayer = 'O';              // newGame flips it back to X
      newGame();
    }
    return;
  }
  if (over) { newGame(); return; }    // tap anywhere to continue
  if (vsCpu() && turn != 'X') return;
  if (x < BX || y < BY || x >= BX + 3 * CELL || y >= BY + 3 * CELL) return;
  int i = ((y - BY) / CELL) * 3 + (x - BX) / CELL;
  if (board[i] == ' ') place(i);
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C tic-tac-toe boot");
  randomSeed(esp_random());
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(16, 24, 40);
  C_GRID = gfx->color565(90, 110, 140);
  C_X = gfx->color565(255, 96, 96);
  C_O = gfx->color565(96, 190, 255);
  C_Xd = gfx->color565(110, 55, 60);
  C_Od = gfx->color565(50, 90, 120);
  C_TXT = RGB565_WHITE;
  C_WIN = gfx->color565(255, 220, 60);
  C_BTN = gfx->color565(40, 60, 96);

  prefs.begin("ttt", false);
  scoreX = prefs.getInt("x", 0); scoreO = prefs.getInt("o", 0); scoreD = prefs.getInt("d", 0);
  mode = (Mode)constrain(prefs.getInt("mode", CPU_HARD), 0, NUM_MODES - 1);
  startPlayer = 'O';
  newGame();
}

void loop() {
  static bool down = false;
  static int misses;
  int x, y;
  if (readTouch(x, y)) {
    misses = 0;
    if (!down) { down = true; handleTap(x, y); }   // act on press
  } else if (down && ++misses >= 3) {
    down = false;
  }
  if (aiAt && millis() >= aiAt && !over) { aiAt = 0; place(aiMove()); }
  if (timedTurn()) {                 // Forever: countdown bar; out of time = lose the turn
    static uint32_t lastBar = 0;
    if (millis() - turnStart >= TURN_MS) {
      turn = turn == 'X' ? 'O' : 'X';
      turnStart = millis();
      slowUntil = millis() + 900;
      aiAt = (vsCpu() && turn == 'O') ? millis() + 600 : 0;
      render();
    } else if (millis() - lastBar > 100) {
      lastBar = millis();
      render();
    }
  } else if (slowUntil && millis() > slowUntil) {
    slowUntil = 0;
    render();
  }
  delay(5);
}
