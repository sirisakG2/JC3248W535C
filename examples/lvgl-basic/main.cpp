#include <Arduino.h>
#include <Wire.h>
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

// LVGL renders partial areas into a small buffer; copy each into the canvas,
// then push the whole canvas once after the last area of the frame.
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

static lv_obj_t *counter_label;
static int counter = 0;

static void on_button(lv_event_t *) {
  lv_label_set_text_fmt(counter_label, "Taps: %d", ++counter);
}
static void on_slider(lv_event_t *e) {
  lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
  lv_label_set_text_fmt(lbl, "Brightness: %d%%", (int)lv_slider_get_value(s));
  analogWrite(GFX_BL, map(lv_slider_get_value(s), 0, 100, 10, 255));
}

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(scr, 22, 0);
  lv_obj_set_style_pad_top(scr, 24, 0);

  lv_obj_t *title = lv_label_create(scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "JC3248W535C");

  lv_obj_t *btn = lv_button_create(scr);
  lv_obj_set_size(btn, 240, 64);
  lv_obj_add_event_cb(btn, on_button, LV_EVENT_CLICKED, NULL);
  counter_label = lv_label_create(btn);
  lv_obj_set_style_text_font(counter_label, &lv_font_montserrat_20, 0);
  lv_label_set_text(counter_label, "Tap me");
  lv_obj_center(counter_label);

  lv_obj_t *sl_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(sl_lbl, lv_color_white(), 0);
  lv_label_set_text(sl_lbl, "Brightness: 100%");
  lv_obj_t *slider = lv_slider_create(scr);
  lv_obj_set_width(slider, 240);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, 100, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, sl_lbl);

  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_set_size(row, 240, 60);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *sw_lbl = lv_label_create(row);
  lv_obj_set_style_text_color(sw_lbl, lv_color_white(), 0);
  lv_label_set_text(sw_lbl, "Switch");
  lv_switch_create(row);

  lv_obj_t *arc = lv_arc_create(scr);
  lv_obj_set_size(arc, 150, 150);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("JC3248W535C LVGL boot");

  if (!gfx->begin(40000000)) Serial.println("gfx->begin FAILED");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  const size_t buf_bytes = SCR_W * 40 * sizeof(uint16_t);  // 40-line partial buffer
  void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);

  build_ui();
  Serial.printf("Free heap %u, PSRAM %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop() {
  lv_timer_handler();
  delay(5);
}
