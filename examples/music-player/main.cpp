// Music player for the JC3248W535C: plays /music/*.mp3 (also wav/flac/aac/m4a) from
// the microSD through the on-board NS4168 I2S speaker amp. LVGL UI: track list,
// prev / play-pause / next, volume, progress bar. Auto-advances to the next track.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <Audio.h>

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

// NS4168 speaker amp (mono)
#define I2S_BCLK 42
#define I2S_LRC 2
#define I2S_DOUT 41

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);
SPIClass sdSpi(HSPI);
Audio audio;

// ---------------------------------------------------------------- audio task
// audio.loop() must run often and uninterrupted, so it lives on its own core.
struct AudioCmd { enum Type { PLAY, TOGGLE, VOL } type; int value; char path[192]; };
static QueueHandle_t audioQ;

static void audioTask(void *) {
  for (;;) {
    AudioCmd c;
    while (xQueueReceive(audioQ, &c, 0) == pdTRUE) {
      switch (c.type) {
        case AudioCmd::PLAY: audio.connecttoFS(SD, c.path); break;
        case AudioCmd::TOGGLE: audio.pauseResume(); break;
        case AudioCmd::VOL: audio.setVolume(c.value); break;
      }
    }
    audio.loop();
    vTaskDelay(1);
  }
}

static void sendPlay(const String &path) {
  AudioCmd c{AudioCmd::PLAY, 0, {0}};
  strlcpy(c.path, path.c_str(), sizeof c.path);
  xQueueSend(audioQ, &c, 0);
}
static void sendSimple(AudioCmd::Type t, int v = 0) {
  AudioCmd c{t, v, {0}};
  xQueueSend(audioQ, &c, 0);
}

// ---------------------------------------------------------------- playlist
std::vector<String> tracks;
std::vector<String> titles;
int cur = -1;
bool playing = false, paused = false;
uint32_t startedAt = 0;

static String prettyName(const String &path, int idx) {
  String n = path.substring(path.lastIndexOf('/') + 1);
  n.remove(n.lastIndexOf('.'));
  String a;  // the UI font has no CJK glyphs: keep printable ASCII only
  for (size_t i = 0; i < n.length(); i++) {
    unsigned char ch = n[i];
    if (ch >= 32 && ch < 127) a += (char)ch;
  }
  a.trim();
  return a.length() >= 3 ? a : "Track " + String(idx + 1);
}

static void scanTracks() {
  File d = SD.open("/music");
  if (!d) return;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    String n = e.name(), low = n;
    low.toLowerCase();
    if (!e.isDirectory() && !n.startsWith(".") &&
        (low.endsWith(".mp3") || low.endsWith(".wav") || low.endsWith(".flac") || low.endsWith(".aac") || low.endsWith(".m4a")))
      tracks.push_back("/music/" + n);
    e.close();
  }
  d.close();
  for (size_t i = 0; i < tracks.size(); i++) titles.push_back(prettyName(tracks[i], i));
}

// ---------------------------------------------------------------- touch + display glue
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

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
  if (lv_display_flush_is_last(disp)) gfx->flush();
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  int x, y;
  if (readTouch(x, y)) {
    data->point.x = constrain(x, 0, SCR_W - 1);
    data->point.y = constrain(y, 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------------------------------------------------------------- UI
static lv_obj_t *lblTitle, *lblTime, *bar, *btnPlayLbl, *listObj, *slVol;
static std::vector<lv_obj_t *> listBtns;

static void playIndex(int i) {
  if (tracks.empty()) return;
  cur = (i + tracks.size()) % tracks.size();
  playing = true;
  paused = false;
  startedAt = millis();
  sendPlay(tracks[cur]);
  Serial.printf("play %d/%d: %s\n", cur + 1, (int)tracks.size(), tracks[cur].c_str());
  lv_label_set_text(lblTitle, titles[cur].c_str());
  lv_label_set_text(btnPlayLbl, LV_SYMBOL_PAUSE);
  for (size_t k = 0; k < listBtns.size(); k++) {
    if ((int)k == cur) lv_obj_add_state(listBtns[k], LV_STATE_CHECKED);
    else lv_obj_remove_state(listBtns[k], LV_STATE_CHECKED);
  }
}

static void on_track(lv_event_t *e) { playIndex((int)(intptr_t)lv_event_get_user_data(e)); }
static void on_next(lv_event_t *) { playIndex(cur + 1); }
static void on_prev(lv_event_t *) { playIndex(cur < 0 ? 0 : cur - 1); }
static void on_play(lv_event_t *) {
  if (!playing) { playIndex(cur < 0 ? 0 : cur); return; }
  paused = !paused;
  sendSimple(AudioCmd::TOGGLE);
  lv_label_set_text(btnPlayLbl, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}
static void on_vol(lv_event_t *e) {
  sendSimple(AudioCmd::VOL, lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}

static void ui_update(lv_timer_t *) {
  if (playing && !paused) {
    uint32_t t = audio.getAudioCurrentTime(), d = audio.getAudioFileDuration();
    lv_label_set_text_fmt(lblTime, "%u:%02u / %u:%02u", t / 60, t % 60, d / 60, d % 60);
    lv_bar_set_value(bar, d ? (int)(t * 100 / d) : 0, LV_ANIM_OFF);
    // track finished -> next (grace period lets a new track start)
    if (!audio.isRunning() && millis() - startedAt > 2500) playIndex(cur + 1);
  }
}

static lv_obj_t *makeBtn(lv_obj_t *parent, const char *sym, int w, lv_event_cb_t cb, lv_obj_t **lblOut = nullptr) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, 56);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_label_set_text(l, sym);
  lv_obj_center(l);
  if (lblOut) *lblOut = l;
  return b;
}

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(scr, 10, 0);
  lv_obj_set_style_pad_top(scr, 12, 0);

  lblTitle = lv_label_create(scr);
  lv_obj_set_width(lblTitle, 296);
  lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, lv_color_white(), 0);
  lv_obj_set_style_text_align(lblTitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblTitle, tracks.empty() ? "No music in /music" : "Pick a track");

  bar = lv_bar_create(scr);
  lv_obj_set_size(bar, 280, 8);
  lblTime = lv_label_create(scr);
  lv_obj_set_style_text_color(lblTime, lv_color_hex(0xA0B0C0), 0);
  lv_label_set_text(lblTime, "0:00 / 0:00");

  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_set_size(row, 300, 64);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  makeBtn(row, LV_SYMBOL_PREV, 84, on_prev);
  makeBtn(row, LV_SYMBOL_PLAY, 100, on_play, &btnPlayLbl);
  makeBtn(row, LV_SYMBOL_NEXT, 84, on_next);

  lv_obj_t *vrow = lv_obj_create(scr);
  lv_obj_set_size(vrow, 300, 40);
  lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(vrow, 0, 0);
  lv_obj_set_style_pad_all(vrow, 0, 0);
  lv_obj_remove_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *vl = lv_label_create(vrow);
  lv_label_set_text(vl, LV_SYMBOL_VOLUME_MAX);
  lv_obj_set_style_text_color(vl, lv_color_white(), 0);
  slVol = lv_slider_create(vrow);
  lv_obj_set_width(slVol, 240);
  lv_slider_set_range(slVol, 0, 21);
  lv_slider_set_value(slVol, 12, LV_ANIM_OFF);
  lv_obj_add_event_cb(slVol, on_vol, LV_EVENT_VALUE_CHANGED, NULL);

  listObj = lv_list_create(scr);
  lv_obj_set_size(listObj, 304, 250);
  lv_obj_set_style_bg_color(listObj, lv_color_hex(0x1C2A38), 0);
  lv_obj_set_style_text_color(listObj, lv_color_white(), 0);
  for (size_t i = 0; i < tracks.size(); i++) {
    lv_obj_t *b = lv_list_add_button(listObj, LV_SYMBOL_AUDIO, titles[i].c_str());
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C2A38), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2E5A88), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_add_event_cb(b, on_track, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    listBtns.push_back(b);
  }
  lv_timer_create(ui_update, 300, NULL);
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C music player boot");

  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSpi, 40000000)) scanTracks();
  else Serial.println("SD mount FAILED");
  Serial.printf("%d tracks\n", (int)tracks.size());

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.forceMono(true);
  audio.setVolume(12);  // 0..21
  audioQ = xQueueCreate(8, sizeof(AudioCmd));
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 3, NULL, 0);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  const size_t buf_bytes = SCR_W * 40 * sizeof(uint16_t);
  void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);
  build_ui();
}

void loop() {
  lv_timer_handler();
  delay(5);
}
