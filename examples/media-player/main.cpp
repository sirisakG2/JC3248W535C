// Photo frame + MJPEG video player for the JC3248W535C.
// SD card layout:  /pic/*.jpg   /mjpeg/*.mjpeg  (raw concatenated JPEG frames)
// (/photos and /video are also scanned). Landscape images (e.g. 480x320) are
// rotated 90 degrees to fill the portrait 320x480 screen.
// Touch: tap = pause/resume, swipe left/right = next/previous,
//        long-press = switch Photos <-> Video.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>

// Display (AXS15231B, QSPI on SPI2) + touch (I2C 0x3B)
#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define SCR_W 320
#define SCR_H 480

// microSD on its own SPI bus (SPI3/HSPI) so it never collides with the display
#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

#define SLIDE_MS 5000
#define VIDEO_FPS 20
#define VIDEO_BUF (192 * 1024)

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
SPIClass sdSpi(HSPI);
JPEGDEC jpeg;

enum Mode { PHOTOS, VIDEOS };
Mode mode = PHOTOS;
bool paused = false;
std::vector<String> photos, videos;
int photoIdx = 0, videoIdx = 0;
int drawX = 0, drawY = 0;
bool rotateDraw = false;

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

enum Gesture { G_NONE, G_TAP, G_NEXT, G_PREV, G_LONG };

// Call every loop; returns a gesture when a touch sequence finishes.
// Images are shown rotated 90 deg CW, so a swipe along the viewer's left/right
// is the screen's Y axis; swipes on either axis are accepted.
static Gesture pollGesture() {
  static bool down = false, longFired = false;
  static int sx, sy, lx, ly, misses;
  static uint32_t t0;
  int x, y;
  if (Serial.available() && Serial.read() == 'v') return G_LONG;  // debug: send 'v' over serial to switch mode
  if (readTouch(x, y)) {
    misses = 0;
    if (!down) { down = true; longFired = false; sx = x; sy = y; t0 = millis(); }
    lx = x; ly = y;
    if (!longFired && abs(lx - sx) < 30 && abs(ly - sy) < 30 && millis() - t0 > 800) {
      longFired = true;
      Serial.println("gesture: long-press");
      return G_LONG;
    }
  } else if (down && ++misses >= 4) {  // tolerate brief dropouts mid-swipe
    down = false;
    if (longFired) return G_NONE;
    int dx = lx - sx, dy = ly - sy;
    Serial.printf("touch end dx=%d dy=%d\n", dx, dy);
    int adx = abs(dx), ady = abs(dy);
    if (max(adx, ady) > 50) {
      int d = adx >= ady ? dx : dy;  // dominant axis
      Serial.println(d < 0 ? "gesture: next" : "gesture: prev");
      return d < 0 ? G_NEXT : G_PREV;
    }
    if (max(adx, ady) < 30) { Serial.println("gesture: tap"); return G_TAP; }
  }
  return G_NONE;
}

// ---------------------------------------------------------------- drawing
static void banner(const char *l1, const char *l2 = nullptr) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(16, 200);
  gfx->println(l1);
  if (l2) { gfx->setTextSize(1); gfx->setCursor(16, 240); gfx->println(l2); }
  gfx->flush();
}

static void toast(const char *msg) {
  gfx->fillRect(0, 0, SCR_W, 28, RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(8, 6);
  gfx->print(msg);
  gfx->flush();
}

static int jpegDraw(JPEGDRAW *d) {
  if (!rotateDraw) {
    gfx->draw16bitRGBBitmap(d->x + drawX, d->y + drawY, d->pPixels, d->iWidth, d->iHeight);
    return 1;
  }
  // 90 deg clockwise: image (x,y) -> screen (SCR_W-1-y, x), written straight into the framebuffer
  uint16_t *fb = gfx->getFramebuffer();
  for (int j = 0; j < d->iHeight; j++) {
    int sx = SCR_W - 1 - (d->y + j + drawY);
    if (sx < 0 || sx >= SCR_W) continue;
    const uint16_t *src = d->pPixels + j * d->iWidth;
    for (int i = 0; i < d->iWidth; i++) {
      int sy = d->x + i + drawX;
      if (sy >= 0 && sy < SCR_H) fb[sy * SCR_W + sx] = src[i];
    }
  }
  return 1;
}

// Decode a JPEG in RAM, scaled down to fit and centred. Returns false on error.
static bool showJpeg(uint8_t *data, size_t len, bool clear) {
  if (!jpeg.openRAM(data, len, jpegDraw)) return false;
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  int w = jpeg.getWidth(), h = jpeg.getHeight();
  int scale = 0, div = 1;
  const int opts[4] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
  rotateDraw = w > h;
  const int fitW = rotateDraw ? SCR_H : SCR_W;  // image-space box after rotation
  const int fitH = rotateDraw ? SCR_W : SCR_H;
  while (scale < 3 && (w / div > fitW || h / div > fitH)) { scale++; div *= 2; }
  drawX = max(0, (fitW - w / div) / 2);
  drawY = max(0, (fitH - h / div) / 2);
  if (clear) gfx->fillScreen(RGB565_BLACK);
  bool ok = jpeg.decode(0, 0, opts[scale]);
  jpeg.close();
  return ok;
}

// ---------------------------------------------------------------- photos
static void showPhoto(int idx) {
  File f = SD.open(photos[idx]);
  if (!f) return;
  size_t len = f.size();
  uint8_t *buf = (uint8_t *)ps_malloc(len);
  if (!buf) { f.close(); Serial.println("photo too large for PSRAM"); return; }
  f.read(buf, len);
  f.close();
  Serial.printf("photo %d/%d %s (%u bytes)\n", idx + 1, (int)photos.size(), photos[idx].c_str(), (unsigned)len);
  if (showJpeg(buf, len, true)) gfx->flush();
  else banner("Can't decode", photos[idx].c_str());
  free(buf);
}

// ---------------------------------------------------------------- video
// Returns the gesture that stopped playback (or G_NONE when the clip ended).
static Gesture playVideo(const String &path) {
  File f = SD.open(path);
  if (!f) return G_NONE;
  uint8_t *buf = (uint8_t *)ps_malloc(VIDEO_BUF);
  if (!buf) { f.close(); return G_NONE; }
  size_t have = 0;
  uint32_t next = millis(), frames = 0, bad = 0, tlog = millis();
  Serial.printf("video %s\n", path.c_str());

  for (;;) {
    Gesture g = pollGesture();
    if (g == G_TAP) { paused = !paused; toast(paused ? "Paused" : "Playing"); }
    else if (g != G_NONE) { free(buf); f.close(); return g; }
    if (paused) { delay(20); next = millis(); continue; }

    have += f.read(buf + have, VIDEO_BUF - have);
    // find one frame: SOI (FFD8) .. EOI (FFD9)
    size_t s = 0;
    while (s + 1 < have && !(buf[s] == 0xFF && buf[s + 1] == 0xD8)) s++;
    size_t e = s + 2;
    while (e + 1 < have && !(buf[e] == 0xFF && buf[e + 1] == 0xD9)) e++;
    if (e + 1 >= have) {
      if (!f.available() || have >= VIDEO_BUF) break;  // end of clip / oversized frame
      continue;
    }
    size_t flen = e + 2 - s;

    while ((int32_t)(millis() - next) < 0) delay(1);  // frame pacing
    if (showJpeg(buf + s, flen, false)) gfx->flush(); else bad++;
    frames++;
    if (millis() - tlog > 2000) {
      Serial.printf("  frames=%u bad=%u last=%uB fps=%.1f\n", frames, bad, (unsigned)flen, frames * 1000.0 / (millis() - tlog));
      frames = 0; tlog = millis();
    }
    next += 1000 / VIDEO_FPS;
    if ((int32_t)(millis() - next) > 200) next = millis();  // fell behind: resync

    have -= e + 2;
    memmove(buf, buf + e + 2, have);
  }
  free(buf);
  f.close();
  return G_NONE;
}

// ---------------------------------------------------------------- SD scan
static void scanDir(const char *dir, const char *ext, std::vector<String> &out) {
  File d = SD.open(dir);
  if (!d) return;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    String n = e.name();
    String low = n;
    low.toLowerCase();
    if (!e.isDirectory() && !n.startsWith(".") && low.endsWith(ext)) out.push_back(String(dir) + "/" + n);
    e.close();
  }
  d.close();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C media player boot");
  Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());

  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSpi, 40000000)) {
    Serial.println("SD mount FAILED");
    banner("No SD card", "Insert a FAT32 microSD card and reset.");
    return;
  }
  Serial.printf("SD OK: %llu MB\n", SD.cardSize() / (1024 * 1024));
  for (const char *d : {"/pic", "/photos"}) { scanDir(d, ".jpg", photos); scanDir(d, ".jpeg", photos); }
  for (const char *d : {"/mjpeg", "/video"}) { scanDir(d, ".mjpeg", videos); scanDir(d, ".mjpg", videos); }
  Serial.printf("%d photos, %d videos\n", (int)photos.size(), (int)videos.size());
  if (photos.empty() && videos.empty()) {
    banner("No media found", "Add /pic/*.jpg and /mjpeg/*.mjpeg");
    return;
  }
  if (photos.empty()) mode = VIDEOS;
}

void loop() {
  if (photos.empty() && videos.empty()) { delay(1000); return; }

  if (mode == PHOTOS) {
    if (photos.empty()) { mode = VIDEOS; return; }
    showPhoto(photoIdx);
    uint32_t t0 = millis();
    for (;;) {
      Gesture g = pollGesture();
      if (g == G_TAP) { paused = !paused; toast(paused ? "Paused" : "Slideshow"); t0 = millis(); }
      else if (g == G_NEXT) { photoIdx = (photoIdx + 1) % photos.size(); break; }
      else if (g == G_PREV) { photoIdx = (photoIdx + photos.size() - 1) % photos.size(); break; }
      else if (g == G_LONG && !videos.empty()) { mode = VIDEOS; toast("Video"); delay(400); break; }
      if (!paused && millis() - t0 > SLIDE_MS) { photoIdx = (photoIdx + 1) % photos.size(); break; }
      delay(10);
    }
  } else {
    if (videos.empty()) { mode = PHOTOS; return; }
    Gesture g = playVideo(videos[videoIdx]);
    if (g == G_PREV) videoIdx = (videoIdx + videos.size() - 1) % videos.size();
    else if (g == G_LONG && !photos.empty()) { mode = PHOTOS; toast("Photos"); delay(400); }
    else videoIdx = (videoIdx + 1) % videos.size();  // clip ended or swipe next
  }
}
