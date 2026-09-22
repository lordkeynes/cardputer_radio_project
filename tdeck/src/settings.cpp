/** * Settings app: theme picker (live preview), screen-sleep timeout. * Persisted to /config/theme.txt and /config/sleep.txt on SD. */
#include <Arduino.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "settings.h"

extern Arduino_GFX *gfx;
extern bool sdOk;

static void sleepSave(unsigned long ms) {
  if (!sdOk) return;
  if (!SD.exists("/config")) SD.mkdir("/config");
  File f = SD.open("/config/sleep.txt", FILE_WRITE);
  if (f) { f.printf("%lu\n", ms); f.close(); }
}

unsigned long settingsSleepMs() {
  static unsigned long cached = 45000UL;
  static bool loaded = false;
  if (!loaded) {
    // Load once, called from setup() AFTER SD init so we never touch the
    // SD/SPI bus from a task while another task is using it.
    loaded = true;
    cached = 45000UL;
    if (sdOk && SD.exists("/config/sleep.txt")) {
      File f = SD.open("/config/sleep.txt", FILE_READ);
      if (f) {
        String s = f.readStringUntil('\n');
        f.close();
        long v = s.toInt();
        if (v == 0) cached = 0;             // 0 = never sleep
        else if (v >= 10000 && v <= 600000UL) cached = (unsigned long)v;
      }
    }
  }
  return cached;
}

void settingsApplySleep() {
  // pda.cpp reads the timeout via this accessor at tick time
  settingsSleepMs();
}

static const unsigned long sleepChoices[] = {
  0UL, 15000UL, 30000UL, 45000UL, 60000UL, 120000UL, 300000UL
};
static const char *const sleepLabels[] = {
  "Never", "15 s", "30 s", "45 s", "1 min", "2 min", "5 min"
};
#define N_SLEEP (int)(sizeof(sleepChoices)/sizeof(sleepChoices[0]))

static void settingsThemeScreen() {
  int cur = themeIndex();
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(4, 7);
      gfx->print("Theme");
      gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 26);
      gfx->print("u/d=pick click=apply Long=back");
      int nT = themeCount();
      for (int i = 0; i < nT; i++) {
        int y = 40 + i * 14;
        if (i == cur) {
          gfx->fillRect(0, y - 2, SCREEN_W, 13, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, y);
        gfx->print(themeName(i));
      }
      int py = 40 + nT * 14 + 6;
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, py);
      gfx->print("Colors:");
      uint16_t sw[7] = { TERM_GREEN, TERM_BRIGHT, TERM_DIM,
                         TERM_SEL_BG, TERM_ACCENT, TERM_RED, TERM_CYAN };
      for (int i = 0; i < 7; i++) {
        gfx->fillRect(60 + i * 24, py - 1, 20, 10, sw[i]);
        gfx->drawRect(60 + i * 24, py - 1, 20, 10, TERM_DIM);
      }
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && cur > 0) { cur--; themeSet(cur); needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN && cur < themeCount() - 1) {
      cur++; themeSet(cur); needsRedraw = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

static void settingsSleepScreen() {
  unsigned long cur = settingsSleepMs();
  int sel = 3;
  for (int i = 0; i < N_SLEEP; i++)
    if (sleepChoices[i] == cur) { sel = i; break; }
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(4, 7);
      gfx->print("Screen sleep");
      gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 26);
      gfx->print("l/r=pick click=save Long=back");
      for (int i = 0; i < N_SLEEP; i++) {
        int y = 44 + i * 16;
        if (i == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 15, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, y);
        gfx->print(sleepLabels[i]);
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("click=save Long=back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN && sel < N_SLEEP - 1) { sel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_LEFT && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_RIGHT && sel < N_SLEEP - 1) { sel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      sleepSave(sleepChoices[sel]);
      gfx->fillRect(0, SCREEN_H - 20, SCREEN_W, 14, BLACK);
      gfx->setTextColor(TERM_ACCENT, BLACK);
      gfx->setCursor(4, SCREEN_H - 16);
      gfx->print("Saved");
      delay(400);
      needsRedraw = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

extern int wifiKnownList(String *ssids, int maxN);
extern void wifiForget(int idx);

#define SET_WIFI_MAX 8

static void settingsWifiScreen() {
  static String ssids[SET_WIFI_MAX];
  int n = sdOk ? wifiKnownList(ssids, SET_WIFI_MAX) : 0;
  int sel = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(4, 7);
      gfx->print("Saved WiFi");
      gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 26);
      gfx->print("click=forget  Long=back");
      if (n == 0) {
        gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, 50);
        gfx->print("No saved networks");
      }
      for (int i = 0; i < n; i++) {
        int y = 44 + i * 16;
        if (i == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 15, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, y);
        gfx->print(ssids[i]);
      }
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN && sel < n - 1) { sel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (n > 0) {
        wifiForget(sel);
        n = sdOk ? wifiKnownList(ssids, SET_WIFI_MAX) : 0;
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
      }
      needsRedraw = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

void settingsApp() {
  static const char *const items[] = { "Theme", "Screen sleep", "Saved WiFi" };
  const int n = 3;
  int sel = 0;
  int lastSel = -1;
  bool full = true;
  while (true) {
    if (full) {
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(4, 7);
      gfx->print("Settings");
      gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 26);
      gfx->print("u/d=pick click=open Long=back");
      for (int i = 0; i < n; i++) {
        int y = 48 + i * 18;
        if (i == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 15, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(12, y);
        gfx->print(items[i]);
      }
      lastSel = sel;
      full = false;
    } else if (sel != lastSel) {
      int y = 48 + lastSel * 18;
      gfx->fillRect(0, y - 2, SCREEN_W, 15, BLACK);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->setCursor(12, y);
      gfx->print(items[lastSel]);
      y = 48 + sel * 18;
      gfx->fillRect(0, y - 2, SCREEN_W, 15, TERM_SEL_BG);
      gfx->setTextColor(BLACK, TERM_SEL_BG);
      gfx->setCursor(12, y);
      gfx->print(items[sel]);
      lastSel = sel;
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) { sel--; }
    else if (e.ev == PDA_EV_DOWN && sel < n - 1) { sel++; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (sel == 0) settingsThemeScreen();
      else if (sel == 1) settingsSleepScreen();
      else settingsWifiScreen();
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}
