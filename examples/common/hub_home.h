// Shared "return to the game hub" helper. Call hubTouchNote() from the game's touch read.
// Go home by: holding the top-left corner for 1.5 s, or holding the BOOT button for 0.7 s.
// On first use it also makes the launcher the next boot app, so a reset/power cycle returns to the hub.
// Does nothing when the game runs standalone (i.e. it is itself in the ota_0 slot).
#pragma once
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#define HUB_BOOT_PIN 0

// Make the launcher the partition the chip boots next. Returns false when there is nothing to do
// (running standalone, or no launcher installed).
static inline bool hubSetBootHome() {
  const esp_partition_t *home = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (!home || home == run) return false;
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(home, &desc) != ESP_OK) return false;   // no launcher installed
  return esp_ota_set_boot_partition(home) == ESP_OK;
}

static inline void hubGoHome() {
  if (hubSetBootHome()) esp_restart();
}

static inline void hubTouchNote(bool touching, int x, int y) {
  static bool init = false;
  static uint32_t cornerT = 0, lastIn = 0, bootT = 0;
  if (!init) {
    pinMode(HUB_BOOT_PIN, INPUT_PULLUP);
    hubSetBootHome();          // the hub is "home": a reset or power-up from here boots the launcher
    init = true;
  }
  uint32_t now = millis();

  if (digitalRead(HUB_BOOT_PIN) == LOW) { if (!bootT) bootT = now; else if (now - bootT > 700) hubGoHome(); }
  else bootT = 0;

#ifndef HUB_NO_CORNER
  if (touching && x < 60 && y < 60) {
    lastIn = now;
    if (!cornerT) cornerT = now;
    else if (now - cornerT > 1500) hubGoHome();
  } else if (cornerT && now - lastIn > 300) {         // tolerate brief touch dropouts
    cornerT = 0;
  }
#endif
}
