#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

// Guition JC3248W535C: AXS15231B 320x480 QSPI LCD + capacitive touch (I2C 0x3B)
#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define SCR_W 320
#define SCR_H 480

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCR_W, SCR_H);
Arduino_Canvas *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel, 0, 0, 0);

// ---------------------------------------------------------------- shared state
// The worker task (WiFi/HTTP) writes; the LVGL loop reads. Guarded by `mtx`.
struct Shared {
  String status = "Not connected";
  String ssidList;          // '\n'-separated scan result
  uint32_t scanVersion = 0;
  bool wifiUp = false;
  bool haveWeather = false;
  String cityName = "Bangkok";
  float temp = 0;
  int code = 0;
  char dayName[3][4] = {"", "", ""};
  float tmax[3] = {0}, tmin[3] = {0};
  int dcode[3] = {0};
  String pendingSsid, pendingPass, pendingCity;
} sh;
SemaphoreHandle_t mtx;

enum Cmd { CMD_SCAN, CMD_CONNECT, CMD_GEOCODE, CMD_WEATHER };
QueueHandle_t cmdQ;

struct Config {
  float lat = 13.7563, lon = 100.5018;
  int utcOffset = 25200;
  bool h24 = true;
  int bright = 100;
} cfg;
Preferences prefs;

// ---------------------------------------------------------------- helpers
static const char *weatherText(int c) {
  if (c == 0) return "Clear";
  if (c <= 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorm";
  return "--";
}

static String urlEncode(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') o += c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

static void setStatus(const String &s) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.status = s;
  xSemaphoreGive(mtx);
}

static bool httpGetJson(const String &url, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();  // public read-only weather data; no secrets are sent
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getString());
  else Serial.printf("HTTP %d for %s\n", code, url.c_str());
  http.end();
  return ok;
}

// ---------------------------------------------------------------- worker task
static void applyTime() { configTime(cfg.utcOffset, 0, "pool.ntp.org", "time.google.com"); }

static void doWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min"
               "&timezone=auto&forecast_days=3";
  JsonDocument doc;
  if (!httpGetJson(url, doc)) { setStatus("Weather fetch failed"); return; }

  int off = doc["utc_offset_seconds"] | cfg.utcOffset;
  if (off != cfg.utcOffset) {
    cfg.utcOffset = off;
    prefs.putInt("tz", off);
    applyTime();
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.temp = doc["current"]["temperature_2m"] | 0.0f;
  sh.code = doc["current"]["weather_code"] | 0;
  for (int i = 0; i < 3; i++) {
    sh.tmax[i] = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    sh.tmin[i] = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    sh.dcode[i] = doc["daily"]["weather_code"][i] | 0;
    const char *d = doc["daily"]["time"][i] | "";
    int y, m, dd;
    struct tm t = {};
    if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3) {
      t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = dd; t.tm_hour = 12;
      mktime(&t);
      strftime(sh.dayName[i], sizeof sh.dayName[i], "%a", &t);
    }
  }
  sh.haveWeather = true;
  sh.status = "Weather updated";
  xSemaphoreGive(mtx);
}

static void doGeocode() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String city = sh.pendingCity;
  xSemaphoreGive(mtx);
  if (city.isEmpty() || WiFi.status() != WL_CONNECTED) { setStatus("Connect WiFi first"); return; }
  JsonDocument doc;
  if (!httpGetJson("https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + urlEncode(city), doc) ||
      doc["results"].isNull()) {
    setStatus("City not found");
    return;
  }
  cfg.lat = doc["results"][0]["latitude"] | cfg.lat;
  cfg.lon = doc["results"][0]["longitude"] | cfg.lon;
  String name = doc["results"][0]["name"] | city.c_str();
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", name);
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.cityName = name;
  xSemaphoreGive(mtx);
  doWeather();
}

static void doScan() {
  setStatus("Scanning...");
  int n = WiFi.scanNetworks();
  String list;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty() || list.indexOf(s + "\n") >= 0) continue;
    list += s + "\n";
  }
  if (list.endsWith("\n")) list.remove(list.length() - 1);
  WiFi.scanDelete();
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.ssidList = list;
  sh.scanVersion++;
  sh.status = n > 0 ? String(n) + " networks found" : "No networks found";
  xSemaphoreGive(mtx);
}

static void doConnect() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String ssid = sh.pendingSsid, pass = sh.pendingPass;
  xSemaphoreGive(mtx);
  if (ssid.isEmpty()) { setStatus("Pick a network"); return; }
  setStatus("Connecting to " + ssid + "...");
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(500));
  if (WiFi.status() == WL_CONNECTED) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    setStatus("Connected: " + WiFi.localIP().toString());
    applyTime();
    doWeather();
  } else {
    setStatus("Connect failed (check password)");
  }
}

static void workerTask(void *) {
  uint32_t lastWeather = 0;
  bool first = true;
  for (;;) {
    int cmd;
    if (xQueueReceive(cmdQ, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      switch (cmd) {
        case CMD_SCAN: doScan(); break;
        case CMD_CONNECT: doConnect(); lastWeather = millis(); break;
        case CMD_GEOCODE: doGeocode(); lastWeather = millis(); break;
        case CMD_WEATHER: doWeather(); lastWeather = millis(); break;
      }
    }
    bool up = WiFi.status() == WL_CONNECTED;
    xSemaphoreTake(mtx, portMAX_DELAY);
    sh.wifiUp = up;
    xSemaphoreGive(mtx);
    if (up && (first || millis() - lastWeather > 15UL * 60 * 1000)) {
      if (first) { applyTime(); setStatus("Connected: " + WiFi.localIP().toString()); }
      first = false;
      doWeather();
      lastWeather = millis();
    }
  }
}

// ---------------------------------------------------------------- display + touch glue
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
static lv_obj_t *lblClock, *lblDate, *lblCity, *lblTemp, *lblCond, *lblStatus, *lblWifiDot;
static lv_obj_t *fcDay[3], *fcCond[3], *fcTemp[3];
static lv_obj_t *ddSsid, *taPass, *taCity, *kb, *setStatusLbl, *swH24;
static uint32_t seenScan = 0;

static void post(int cmd) { xQueueSend(cmdQ, &cmd, 0); }

static void ui_update(lv_timer_t *) {
  struct tm ti;
  if (getLocalTime(&ti, 0)) {
    char b[24];
    strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M %p", &ti);
    lv_label_set_text(lblClock, b);
    strftime(b, sizeof b, "%a, %d %b %Y", &ti);
    lv_label_set_text(lblDate, b);
  } else {
    lv_label_set_text(lblClock, "--:--");
    lv_label_set_text(lblDate, "Waiting for time...");
  }

  xSemaphoreTake(mtx, portMAX_DELAY);
  String status = sh.status, list = sh.ssidList, city = sh.cityName;
  bool up = sh.wifiUp, have = sh.haveWeather;
  float temp = sh.temp; int code = sh.code;
  uint32_t sv = sh.scanVersion;
  char dn[3][4]; float mx[3], mn[3]; int dc[3];
  memcpy(dn, sh.dayName, sizeof dn);
  memcpy(mx, sh.tmax, sizeof mx); memcpy(mn, sh.tmin, sizeof mn); memcpy(dc, sh.dcode, sizeof dc);
  xSemaphoreGive(mtx);

  lv_label_set_text(lblStatus, status.c_str());
  lv_label_set_text(setStatusLbl, status.c_str());
  lv_obj_set_style_text_color(lblWifiDot, up ? lv_color_hex(0x40D060) : lv_color_hex(0xD05050), 0);
  lv_label_set_text(lblCity, city.c_str());
  if (have) {
    lv_label_set_text_fmt(lblTemp, "%.0f°C", temp);
    lv_label_set_text(lblCond, weatherText(code));
    for (int i = 0; i < 3; i++) {
      lv_label_set_text(fcDay[i], i == 0 ? "Today" : dn[i]);
      lv_label_set_text(fcCond[i], weatherText(dc[i]));
      lv_label_set_text_fmt(fcTemp[i], "%.0f° / %.0f°", mx[i], mn[i]);
    }
  } else {
    lv_label_set_text(lblTemp, "--");
    lv_label_set_text(lblCond, up ? "Loading..." : "Set up WiFi in Settings");
  }
  if (sv != seenScan) {
    seenScan = sv;
    lv_dropdown_set_options(ddSsid, list.isEmpty() ? "(none)" : list.c_str());
  }
}

static void ta_event(lv_event_t *e) {
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
  }
}

static void kb_event(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
  lv_obj_t *ta = lv_keyboard_get_textarea(kb);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_state(ta, LV_STATE_FOCUSED);
  if (c == LV_EVENT_READY && ta == taCity) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    sh.pendingCity = lv_textarea_get_text(taCity);
    sh.status = "Looking up city...";
    xSemaphoreGive(mtx);
    post(CMD_GEOCODE);
  }
}

static void scan_event(lv_event_t *) { post(CMD_SCAN); }

static void connect_event(lv_event_t *) {
  char ssid[64] = "";
  lv_dropdown_get_selected_str(ddSsid, ssid, sizeof ssid);
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.pendingSsid = ssid;
  sh.pendingPass = lv_textarea_get_text(taPass);
  xSemaphoreGive(mtx);
  post(CMD_CONNECT);
}

static void h24_event(lv_event_t *e) {
  cfg.h24 = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
  prefs.putBool("h24", cfg.h24);
}

static void bright_event(lv_event_t *e) {
  cfg.bright = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
  analogWrite(GFX_BL, map(cfg.bright, 0, 100, 25, 255));
  prefs.putInt("bl", cfg.bright);
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static void build_home(lv_obj_t *t) {
  lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(t, 6, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

  lblWifiDot = makeLabel(t, LV_SYMBOL_WIFI, &lv_font_montserrat_20, 0xD05050);
  lblClock = makeLabel(t, "--:--", &lv_font_montserrat_48, 0xFFFFFF);
  lblDate = makeLabel(t, "", &lv_font_montserrat_20, 0xA0B0C0);

  lv_obj_t *card = lv_obj_create(t);
  lv_obj_set_size(card, 296, 130);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1C2A38), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lblCity = makeLabel(card, "", &lv_font_montserrat_20, 0xA0B0C0);
  lblTemp = makeLabel(card, "--", &lv_font_montserrat_48, 0xFFD060);
  lblCond = makeLabel(card, "", &lv_font_montserrat_20, 0xFFFFFF);

  lv_obj_t *row = lv_obj_create(t);
  lv_obj_set_size(row, 300, 110);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_set_size(c, 94, 104);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1C2A38), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    fcDay[i] = makeLabel(c, "", &lv_font_montserrat_14, 0xA0B0C0);
    fcCond[i] = makeLabel(c, "", &lv_font_montserrat_14, 0xFFFFFF);
    lv_label_set_long_mode(fcCond[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(fcCond[i], 84);
    lv_obj_set_style_text_align(fcCond[i], LV_TEXT_ALIGN_CENTER, 0);
    fcTemp[i] = makeLabel(c, "", &lv_font_montserrat_14, 0xFFD060);
  }

  lblStatus = makeLabel(t, "", &lv_font_montserrat_14, 0x708090);
}

static void build_settings(lv_obj_t *t) {
  lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(t, 8, 0);
  lv_obj_set_style_pad_bottom(t, 330, 0);  // room to scroll above the keyboard

  makeLabel(t, "WiFi", &lv_font_montserrat_20, 0xFFFFFF);
  lv_obj_t *r1 = lv_obj_create(t);
  lv_obj_set_size(r1, 290, 50);
  lv_obj_set_style_bg_opa(r1, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r1, 0, 0);
  lv_obj_set_style_pad_all(r1, 0, 0);
  lv_obj_remove_flag(r1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  ddSsid = lv_dropdown_create(r1);
  lv_dropdown_set_options(ddSsid, "(tap Scan)");
  lv_obj_set_width(ddSsid, 190);
  lv_obj_t *bScan = lv_button_create(r1);
  lv_obj_set_size(bScan, 90, 44);
  lv_obj_add_event_cb(bScan, scan_event, LV_EVENT_CLICKED, NULL);
  lv_obj_center(lv_label_create(bScan));
  lv_label_set_text(lv_obj_get_child(bScan, 0), "Scan");

  taPass = lv_textarea_create(t);
  lv_obj_set_width(taPass, 290);
  lv_textarea_set_one_line(taPass, true);
  lv_textarea_set_password_mode(taPass, true);
  lv_textarea_set_placeholder_text(taPass, "Password");
  lv_obj_add_event_cb(taPass, ta_event, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *bConn = lv_button_create(t);
  lv_obj_set_size(bConn, 290, 44);
  lv_obj_add_event_cb(bConn, connect_event, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(bConn), "Connect");
  lv_obj_center(lv_obj_get_child(bConn, 0));

  setStatusLbl = makeLabel(t, "", &lv_font_montserrat_14, 0x8FB0D0);

  makeLabel(t, "City (press Enter to apply)", &lv_font_montserrat_14, 0xFFFFFF);
  taCity = lv_textarea_create(t);
  lv_obj_set_width(taCity, 290);
  lv_textarea_set_one_line(taCity, true);
  lv_textarea_set_text(taCity, sh.cityName.c_str());
  lv_obj_add_event_cb(taCity, ta_event, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *r2 = lv_obj_create(t);
  lv_obj_set_size(r2, 290, 44);
  lv_obj_set_style_bg_opa(r2, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r2, 0, 0);
  lv_obj_set_style_pad_all(r2, 0, 0);
  lv_obj_remove_flag(r2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  makeLabel(r2, "24-hour clock", &lv_font_montserrat_20, 0xFFFFFF);
  swH24 = lv_switch_create(r2);
  if (cfg.h24) lv_obj_add_state(swH24, LV_STATE_CHECKED);
  lv_obj_add_event_cb(swH24, h24_event, LV_EVENT_VALUE_CHANGED, NULL);

  makeLabel(t, "Brightness", &lv_font_montserrat_14, 0xFFFFFF);
  lv_obj_t *sl = lv_slider_create(t);
  lv_obj_set_width(sl, 270);
  lv_slider_set_range(sl, 0, 100);
  lv_slider_set_value(sl, cfg.bright, LV_ANIM_OFF);
  lv_obj_add_event_cb(sl, bright_event, LV_EVENT_VALUE_CHANGED, NULL);
}

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

  lv_obj_t *tv = lv_tabview_create(scr);
  lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(tv, 48);
  lv_obj_set_style_bg_color(tv, lv_color_hex(0x101820), 0);
  lv_obj_remove_flag(lv_tabview_get_content(tv), LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_t *home = lv_tabview_add_tab(tv, "Home");
  lv_obj_t *set = lv_tabview_add_tab(tv, "Settings");
  build_home(home);
  build_settings(set);

  kb = lv_keyboard_create(lv_layer_top());
  lv_obj_set_size(kb, SCR_W, 320);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(kb, 4, 0);
  lv_obj_set_style_pad_gap(kb, 5, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(kb, kb_event, LV_EVENT_ALL, NULL);

  lv_timer_create(ui_update, 500, NULL);
  ui_update(NULL);
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C weather clock boot");

  mtx = xSemaphoreCreateMutex();
  cmdQ = xQueueCreate(8, sizeof(int));

  prefs.begin("clock", false);
  cfg.lat = prefs.getFloat("lat", cfg.lat);
  cfg.lon = prefs.getFloat("lon", cfg.lon);
  cfg.utcOffset = prefs.getInt("tz", cfg.utcOffset);
  cfg.h24 = prefs.getBool("h24", true);
  cfg.bright = prefs.getInt("bl", 100);
  sh.cityName = prefs.getString("city", "Bangkok");

  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  analogWrite(GFX_BL, map(cfg.bright, 0, 100, 25, 255));
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

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

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
    setStatus("Connecting to " + ssid + "...");
  }
  xTaskCreatePinnedToCore(workerTask, "worker", 12288, NULL, 1, NULL, 0);
}

void loop() {
  lv_timer_handler();
  delay(5);
}
