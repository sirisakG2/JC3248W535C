// Memory Match for the JC3248W535C. Flip two cards at a time; matching pairs stay up.
// LEVEL cycles Easy (3x4) / Medium (4x5) / Hard (4x6); NEW reshuffles.
// Fewest moves per level is saved in flash.
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

#define FIELD_X 10
#define FIELD_Y 64
#define FIELD_W 300
#define FIELD_H 366
#define GAP 6
#define BTN_Y 440
#define BTN_H 36
#define MISMATCH_MS 900
#define NSYM 12

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
Preferences prefs;

struct Level { const char *name; int cols, rows; };
const Level LEVELS[3] = {{"Easy", 3, 4}, {"Medium", 4, 5}, {"Hard", 4, 6}};
int lvl = 0, COLS, ROWS, N;

enum CardState { HIDDEN, SHOWN, MATCHED };
int8_t sym[24];
uint8_t st[24];
int first = -1, second = -1;
uint32_t mismatchAt = 0, startMs = 0, endMs = 0;
int moves = 0, pairsLeft = 0, bestMoves[3] = {0, 0, 0};
bool started = false, won = false, newBest = false;
uint16_t C_BG, C_BACK, C_BACK_HI, C_FACE, C_TXT, C_BTN, C_OK;
uint16_t SYMC[NSYM];

// ---------------------------------------------------------------- game
static void setLevel(int l) { lvl = l; COLS = LEVELS[l].cols; ROWS = LEVELS[l].rows; N = COLS * ROWS; }

static void newGame() {
  for (int i = 0; i < N; i++) { sym[i] = i / 2; st[i] = HIDDEN; }
  for (int i = N - 1; i > 0; i--) { int j = esp_random() % (i + 1); int8_t t = sym[i]; sym[i] = sym[j]; sym[j] = t; }
  first = second = -1; mismatchAt = 0; moves = 0; pairsLeft = N / 2;
  started = won = newBest = false;
}

static void hideMismatch() {
  if (first >= 0) st[first] = HIDDEN;
  if (second >= 0) st[second] = HIDDEN;
  first = second = -1; mismatchAt = 0;
}

// ---------------------------------------------------------------- drawing
static void centerText(const char *t, int cx, int cy, int size, uint16_t col) {
  int w = strlen(t) * 6 * size, h = 8 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(t);
}

static void drawStar(int cx, int cy, int r, uint16_t c) {
  int px[10], py[10];
  for (int i = 0; i < 10; i++) {
    float a = -M_PI / 2 + i * M_PI / 5, rr = (i & 1) ? r * 0.45f : r;
    px[i] = cx + cosf(a) * rr; py[i] = cy + sinf(a) * rr;
  }
  for (int i = 0; i < 10; i++) gfx->fillTriangle(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
}

static void drawSymbol(int id, int cx, int cy, int s, uint16_t face) {
  int r = s / 2;
  uint16_t c = SYMC[id];
  switch (id) {
    case 0: gfx->fillCircle(cx, cy, r, c); break;                                        // disc
    case 1: gfx->fillRect(cx - r + 2, cy - r + 2, s - 4, s - 4, c); break;               // square
    case 2: gfx->fillTriangle(cx, cy - r, cx - r, cy + r - 2, cx + r, cy + r - 2, c); break;  // triangle
    case 3: gfx->fillTriangle(cx, cy - r, cx - r, cy, cx + r, cy, c);                    // diamond
            gfx->fillTriangle(cx, cy + r, cx - r, cy, cx + r, cy, c); break;
    case 4: drawStar(cx, cy, r + 2, c); break;                                           // star
    case 5: gfx->fillCircle(cx - r / 2, cy - r / 4, r / 2 + 1, c);                       // heart
            gfx->fillCircle(cx + r / 2, cy - r / 4, r / 2 + 1, c);
            gfx->fillTriangle(cx - r + 1, cy - r / 8, cx + r - 1, cy - r / 8, cx, cy + r, c); break;
    case 6: gfx->fillRect(cx - r / 4 - 1, cy - r, r / 2 + 3, s, c);                      // plus
            gfx->fillRect(cx - r, cy - r / 4 - 1, s, r / 2 + 3, c); break;
    case 7: for (int o = -2; o <= 2; o++) {                                              // cross
              gfx->drawLine(cx - r + o, cy - r, cx + r + o, cy + r, c);
              gfx->drawLine(cx + r + o, cy - r, cx - r + o, cy + r, c);
            } break;
    case 8: for (int t = 0; t < 5; t++) gfx->drawCircle(cx, cy, r - t, c); break;        // ring
    case 9: gfx->fillCircle(cx, cy, r, c); gfx->fillCircle(cx + r / 2, cy - r / 6, r * 4 / 5, face); break;  // moon
    case 10: for (int i = 0; i < 6; i++) {                                               // hexagon
               float a1 = i * M_PI / 3, a2 = (i + 1) * M_PI / 3;
               gfx->fillTriangle(cx, cy, cx + cosf(a1) * r, cy + sinf(a1) * r, cx + cosf(a2) * r, cy + sinf(a2) * r, c);
             } break;
    case 11: for (int i = -1; i <= 1; i++) gfx->fillRect(cx - r, cy + i * (r * 2 / 3) - 3, s, 7, c); break;  // bars
  }
}

// draw the card in slot i; wScale in (0,1] squeezes it horizontally for the flip animation
static void drawCard(int i, float wScale = 1, int faceOverride = -1) {
  int cw = (FIELD_W - GAP * (COLS - 1)) / COLS, ch = (FIELD_H - GAP * (ROWS - 1)) / ROWS;
  int x = FIELD_X + (i % COLS) * (cw + GAP), y = FIELD_Y + (i / COLS) * (ch + GAP);
  int w = max(4, (int)(cw * wScale)), xo = x + (cw - w) / 2;
  bool face = faceOverride >= 0 ? faceOverride : st[i] != HIDDEN;
  if (!face) {
    gfx->fillRoundRect(xo, y, w, ch, 8, C_BACK);
    gfx->drawRoundRect(xo, y, w, ch, 8, C_BACK_HI);
    if (w > cw / 2) centerText("?", xo + w / 2, y + ch / 2, 3, C_BACK_HI);
  } else {
    uint16_t bg = st[i] == MATCHED ? C_OK : C_FACE;
    gfx->fillRoundRect(xo, y, w, ch, 8, bg);
    if (w > cw / 2) drawSymbol(sym[i], xo + w / 2, y + ch / 2, (int)(min(cw, ch) * 0.55f), bg);
  }
}

static void drawBtn(int x, int w, const char *label) {
  gfx->fillRoundRect(x, BTN_Y, w, BTN_H, 8, C_BTN);
  centerText(label, x + w / 2, BTN_Y + BTN_H / 2, 2, C_TXT);
}

static void render(int animIdx = -1, float aw = 1, int aFace = -1) {
  gfx->fillScreen(C_BG);
  char b[24];
  uint32_t secs = !started ? 0 : ((won ? endMs : millis()) - startMs) / 1000;
  gfx->setTextSize(2);
  gfx->setTextColor(C_TXT);
  gfx->setCursor(10, 14);
  snprintf(b, sizeof b, "Moves %d", moves);
  gfx->print(b);
  snprintf(b, sizeof b, "%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  gfx->setCursor(SCR_W - 10 - strlen(b) * 12, 14);
  gfx->print(b);
  if (won) snprintf(b, sizeof b, newBest ? "New best!" : "Best: %d", bestMoves[lvl]);
  else if (bestMoves[lvl]) snprintf(b, sizeof b, "Best: %d moves", bestMoves[lvl]);
  else snprintf(b, sizeof b, "%s", LEVELS[lvl].name);
  centerText(b, 160, 46, 1, won ? C_OK : C_TXT);

  for (int i = 0; i < N; i++) {
    if (i == animIdx) drawCard(i, aw, aFace); else drawCard(i);
  }
  if (won) {
    gfx->fillRoundRect(40, 190, 240, 70, 10, C_BG);
    gfx->drawRoundRect(40, 190, 240, 70, 10, C_OK);
    centerText("YOU WIN!", 160, 212, 3, C_OK);
    snprintf(b, sizeof b, "%d moves", moves);
    centerText(b, 160, 244, 2, C_TXT);
  }
  drawBtn(8, 148, LEVELS[lvl].name);
  drawBtn(164, 148, "NEW");
  gfx->flush();
}

// squeeze the card to a sliver, swap face, expand again
static void flipAnim(int i, bool toFace) {
  const float scales[] = {0.75f, 0.45f, 0.15f};
  for (float s : scales) render(i, s, toFace ? 0 : 1);
  for (int k = 2; k >= 0; k--) render(i, scales[k], toFace ? 1 : 0);
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

static int cardAt(int x, int y) {
  int cw = (FIELD_W - GAP * (COLS - 1)) / COLS, ch = (FIELD_H - GAP * (ROWS - 1)) / ROWS;
  if (x < FIELD_X || y < FIELD_Y) return -1;
  int c = (x - FIELD_X) / (cw + GAP), r = (y - FIELD_Y) / (ch + GAP);
  if (c >= COLS || r >= ROWS) return -1;
  if ((x - FIELD_X) % (cw + GAP) >= cw || (y - FIELD_Y) % (ch + GAP) >= ch) return -1;   // in the gap
  return r * COLS + c;
}

static void finishIfWon() {
  if (pairsLeft) return;
  won = true; endMs = millis();
  if (!bestMoves[lvl] || moves < bestMoves[lvl]) {
    bestMoves[lvl] = moves; newBest = true;
    char k[8]; snprintf(k, sizeof k, "bm%d", lvl);
    prefs.putInt(k, moves);
  }
}

static void handleTap(int x, int y) {
  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (x < 160) { setLevel((lvl + 1) % 3); prefs.putInt("lvl", lvl); }
    newGame();
    render();
    return;
  }
  if (won) { newGame(); render(); return; }
  if (mismatchAt) { hideMismatch(); render(); }         // tapping during the delay clears the pair at once
  int i = cardAt(x, y);
  if (i < 0 || st[i] != HIDDEN) return;
  if (!started) { started = true; startMs = millis(); }

  st[i] = SHOWN;
  flipAnim(i, true);
  if (first < 0) { first = i; render(); return; }
  second = i;
  moves++;
  if (sym[first] == sym[second]) {
    st[first] = st[second] = MATCHED;
    first = second = -1;
    pairsLeft--;
    finishIfWon();
  } else {
    mismatchAt = millis() + MISMATCH_MS;
  }
  render();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C memory boot");
  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  C_BG = gfx->color565(20, 28, 44);
  C_BACK = gfx->color565(40, 80, 170);
  C_BACK_HI = gfx->color565(120, 160, 240);
  C_FACE = gfx->color565(240, 240, 235);
  C_TXT = RGB565_WHITE;
  C_BTN = gfx->color565(44, 60, 96);
  C_OK = gfx->color565(120, 200, 120);
  const uint8_t sc[NSYM][3] = {{230,50,50},{50,90,230},{40,170,70},{240,190,20},{245,130,20},{230,60,160},
                               {20,170,200},{130,60,200},{60,60,70},{200,150,20},{20,150,140},{220,90,110}};
  for (int i = 0; i < NSYM; i++) SYMC[i] = gfx->color565(sc[i][0], sc[i][1], sc[i][2]);

  prefs.begin("memory", false);
  for (int i = 0; i < 3; i++) { char k[8]; snprintf(k, sizeof k, "bm%d", i); bestMoves[i] = prefs.getInt(k, 0); }
  setLevel(constrain(prefs.getInt("lvl", 0), 0, 2));
  newGame();
  render();
}

void loop() {
  static bool touching = false;
  static int misses;
  static uint32_t lastSec = 0;
  int x, y;

  if (readTouch(x, y)) {
    misses = 0;
    if (!touching) { touching = true; handleTap(x, y); }   // act on press; needs a full lift for the next one
  } else if (touching && ++misses >= 6) {
    touching = false;
  }

  if (mismatchAt && millis() >= mismatchAt) {              // flip the wrong pair back
    int a = first, b = second;
    st[a] = st[b] = HIDDEN;
    flipAnim(a, false);
    first = second = -1; mismatchAt = 0;
    render();
  }
  if (started && !won && millis() / 1000 != lastSec) {      // timer
    lastSec = millis() / 1000;
    render();
  }
  delay(4);
}
