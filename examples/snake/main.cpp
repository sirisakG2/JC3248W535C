// Snake for the JC3248W535C. Steer with the on-screen arrow pad (instant) or by swiping
// on the field. Walls and your own tail are deadly; each food speeds the snake up.
// Best score is saved in flash.
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

#define CELL 16
#define COLS 20                  // 320 / 16
#define ROWS 20                  // 320 / 16
#define FIELD_Y 40
#define MAX_LEN (COLS * ROWS)
#define START_MS 200
#define MIN_MS 90

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

// on-screen controls (below the 320px field): arrow pad + PAUSE / NEW
struct Btn { int x, y, w, h; };
const Btn B_UP = {122, 364, 76, 50}, B_LEFT = {36, 420, 76, 52}, B_DOWN = {122, 420, 76, 52},
          B_RIGHT = {208, 420, 76, 52}, B_PAUSE = {8, 364, 100, 50}, B_NEW = {212, 364, 100, 50};
static bool hit(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

enum State { READY, PLAYING, PAUSED, DEAD };
enum Dir { UP, RIGHT, DOWN, LEFT };
const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};

State state = READY;
int sx[MAX_LEN], sy[MAX_LEN], len;      // index 0 = head
Dir dir = RIGHT;
Dir queued[2];                          // up to two buffered turns so quick swipes aren't lost
int nq = 0;
int foodX, foodY, score = 0, best = 0, stepMs = START_MS;
uint32_t lastStep = 0;

uint16_t C_BG, C_A, C_B, C_HEAD, C_BODY, C_FOOD, C_TXT, C_BTN;

// ---------------------------------------------------------------- game logic
static bool onSnake(int x, int y, int skipTail = 0) {
  for (int i = 0; i < len - skipTail; i++) if (sx[i] == x && sy[i] == y) return true;
  return false;
}

static void placeFood() {
  do { foodX = random(COLS); foodY = random(ROWS); } while (onSnake(foodX, foodY));
}

static void reset() {
  len = 3;
  for (int i = 0; i < len; i++) { sx[i] = 8 - i; sy[i] = ROWS / 2; }
  dir = RIGHT; nq = 0; score = 0; stepMs = START_MS;
  placeFood();
}

static void turn(Dir d) {
  Dir last = nq ? queued[nq - 1] : dir;
  if (d == last || (d + 2) % 4 == last) return;   // same direction or 180-degree reverse
  if (nq < 2) queued[nq++] = d;
}

static void step() {
  if (nq) { dir = queued[0]; queued[0] = queued[1]; nq--; }
  int nx = sx[0] + DX[dir], ny = sy[0] + DY[dir];
  bool eat = nx == foodX && ny == foodY;
  // the tail cell frees up this step unless we grow
  if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS || onSnake(nx, ny, eat ? 0 : 1)) {
    state = DEAD;
    if (score > best) { best = score; prefs.putInt("best", best); }
    return;
  }
  if (eat && len < MAX_LEN) len++;
  for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
  sx[0] = nx; sy[0] = ny;
  if (eat) {
    score += 10;
    stepMs = max(MIN_MS, START_MS - (score / 10) * 5);
    placeFood();
  }
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static void drawButton(const Btn &b, const char *label) {
  gfx->fillRoundRect(b.x, b.y, b.w, b.h, 8, C_BTN);
  centerText(label, b.x + b.w / 2, b.y + b.h / 2, 2, RGB565_WHITE);
}

static void drawArrow(const Btn &b, Dir d) {
  gfx->fillRoundRect(b.x, b.y, b.w, b.h, 8, C_BTN);
  int cx = b.x + b.w / 2, cy = b.y + b.h / 2, r = 14;
  switch (d) {
    case UP:    gfx->fillTriangle(cx, cy - r, cx - r, cy + r - 2, cx + r, cy + r - 2, RGB565_WHITE); break;
    case DOWN:  gfx->fillTriangle(cx, cy + r, cx - r, cy - r + 2, cx + r, cy - r + 2, RGB565_WHITE); break;
    case LEFT:  gfx->fillTriangle(cx - r, cy, cx + r - 2, cy - r, cx + r - 2, cy + r, RGB565_WHITE); break;
    case RIGHT: gfx->fillTriangle(cx + r, cy, cx - r + 2, cy - r, cx - r + 2, cy + r, RGB565_WHITE); break;
  }
}

static void render() {
  gfx->fillScreen(C_BG);
  // header
  char b[24];
  gfx->setTextSize(2);
  gfx->setTextColor(C_TXT);
  gfx->setCursor(10, 12);
  snprintf(b, sizeof b, "Score %d", score);
  gfx->print(b);
  snprintf(b, sizeof b, "Best %d", best);
  gfx->setCursor(SCR_W - 10 - strlen(b) * 12, 12);
  gfx->print(b);

  // checkerboard field
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      gfx->fillRect(c * CELL, FIELD_Y + r * CELL, CELL, CELL, ((r + c) & 1) ? C_A : C_B);

  // food
  gfx->fillCircle(foodX * CELL + CELL / 2, FIELD_Y + foodY * CELL + CELL / 2, CELL / 2 - 2, C_FOOD);

  // snake (tail to head so the head is on top)
  for (int i = len - 1; i >= 0; i--) {
    int x = sx[i] * CELL, y = FIELD_Y + sy[i] * CELL;
    gfx->fillRoundRect(x + 1, y + 1, CELL - 2, CELL - 2, 4, i == 0 ? C_HEAD : C_BODY);
  }
  // eyes on the head
  int hx = sx[0] * CELL + CELL / 2, hy = FIELD_Y + sy[0] * CELL + CELL / 2;
  int ex = DX[dir], ey = DY[dir];
  int px = -ey, py = ex;                       // perpendicular
  gfx->fillCircle(hx + ex * 3 + px * 4, hy + ey * 3 + py * 4, 2, RGB565_WHITE);
  gfx->fillCircle(hx + ex * 3 - px * 4, hy + ey * 3 - py * 4, 2, RGB565_WHITE);

  drawButton(B_PAUSE, state == PAUSED ? "RESUME" : "PAUSE");
  drawButton(B_NEW, "NEW");
  drawArrow(B_UP, UP); drawArrow(B_LEFT, LEFT); drawArrow(B_DOWN, DOWN); drawArrow(B_RIGHT, RIGHT);

  if (state == READY || state == PAUSED || state == DEAD) {
    gfx->fillRoundRect(30, 130, 260, 100, 10, C_BG);
    gfx->drawRoundRect(30, 130, 260, 100, 10, C_HEAD);
    if (state == DEAD) {
      centerText("Game over", SCR_W / 2, 160, 3, C_TXT);
      snprintf(b, sizeof b, "Score %d", score);
      centerText(b, SCR_W / 2, 192, 2, C_TXT);
      centerText("Tap field or NEW", SCR_W / 2, 215, 1, C_TXT);
    } else if (state == PAUSED) {
      centerText("Paused", SCR_W / 2, 180, 3, C_TXT);
    } else {
      centerText("SNAKE", SCR_W / 2, 158, 3, C_TXT);
      centerText("Press an arrow", SCR_W / 2, 195, 2, C_TXT);
    }
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

static void startGame(Dir d = RIGHT) {
  reset();
  dir = d;
  state = PLAYING;
  lastStep = millis();
  render();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C snake boot");
  randomSeed(esp_random());
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(20, 30, 24);
  C_A = gfx->color565(38, 70, 44);
  C_B = gfx->color565(32, 60, 38);
  C_HEAD = gfx->color565(120, 230, 90);
  C_BODY = gfx->color565(70, 180, 70);
  C_FOOD = gfx->color565(240, 70, 70);
  C_TXT = RGB565_WHITE;
  C_BTN = gfx->color565(50, 90, 60);

  prefs.begin("snake", false);
  best = prefs.getInt("best", 0);
  reset();
  render();
}

void loop() {
  static bool down = false, tapped = false;
  static int tx, ty, misses;
  int x, y;

  if (readTouch(x, y)) {
    misses = 0;
    if (!down) {
      down = true; tapped = false; tx = x; ty = y;
      Dir d = UP;
      bool arrow = true;
      if (hit(B_UP, x, y)) d = UP;
      else if (hit(B_DOWN, x, y)) d = DOWN;
      else if (hit(B_LEFT, x, y)) d = LEFT;
      else if (hit(B_RIGHT, x, y)) d = RIGHT;
      else arrow = false;
      if (arrow) {                                              // arrows act instantly on press
        tapped = true;
        if (state == READY) startGame(d == LEFT ? RIGHT : d);
        else if (state == PLAYING) turn(d);
      } else if (hit(B_PAUSE, x, y)) {
        tapped = true;
        if (state == PLAYING) { state = PAUSED; render(); }
        else if (state == PAUSED) { state = PLAYING; lastStep = millis(); render(); }
      } else if (hit(B_NEW, x, y)) { tapped = true; startGame(); }
      else if (state == DEAD && y < FIELD_Y + ROWS * CELL) { tapped = true; startGame(); }
    } else if (!tapped && y < FIELD_Y + ROWS * CELL) {
      int dx = x - tx, dy = y - ty;
      if (max(abs(dx), abs(dy)) >= 18) {                        // swipe on the field also steers
        Dir d = abs(dx) > abs(dy) ? (dx < 0 ? LEFT : RIGHT) : (dy < 0 ? UP : DOWN);
        if (state == READY) startGame(d == LEFT ? RIGHT : d);
        else if (state == PLAYING) turn(d);
        tx = x; ty = y;
      }
    }
  } else if (down && ++misses >= 3) {
    down = false;
  }

  if (state == PLAYING && millis() - lastStep >= (uint32_t)stepMs) {
    lastStep += stepMs;
    step();
    render();
  }
  delay(3);
}
