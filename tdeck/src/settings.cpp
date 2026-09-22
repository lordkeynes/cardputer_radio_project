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

void settingsApp() {
  int curTheme = themeIndex();
  unsigned long curSleep = settingsSleepMs();
  int sleepSel = 3;
  for (int i = 0; i < N_SLEEP; i++)
    if (sleepChoices[i] == curSleep) { sleepSel = i; break; }

  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 6);
      gfx->print("Settings");
      gfx->setTextSize(1);

      // Theme picker: list with live color swatch
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 34);
      gfx->print("Theme (u/d, click to apply):");
      int nT = themeCount();
      for (int i = 0; i < nT; i++) {
        int y = 48 + i * 14;
        if (i == curTheme) {
          gfx->fillRect(0, y - 2, SCREEN_W, 13, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, y);
        gfx->print(themeName(i));
      }

      // Preview swatches of the active theme
      int py = 48 + nT * 14 + 6;
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, py);
      gfx->print("Colors:");
      uint16_t sw[7] = { TERM_GREEN, TERM_BRIGHT, TERM_DIM,
                         TERM_SEL_BG, TERM_ACCENT, TERM_RED, TERM_CYAN };
      for (int i = 0; i < 7; i++) {
        gfx->fillRect(60 + i * 24, py - 1, 20, 10, sw[i]);
        gfx->drawRect(60 + i * 24, py - 1, 20, 10, TERM_DIM);
      }

      // Sleep timeout picker
      int sy = py + 24;
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, sy);
      gfx->print("Screen sleep (l/r, click):");
      for (int i = 0; i < N_SLEEP; i++) {
        int x = 10 + i * 44;
        if (i == sleepSel) {
          gfx->fillRect(x - 2, sy + 12, 42, 13, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(x, sy + 14);
        gfx->print(sleepLabels[i]);
      }

      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("Long-click = save & exit");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && curTheme > 0) { curTheme--; themeSet(curTheme); needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN && curTheme < themeCount() - 1) {
      curTheme++; themeSet(curTheme); needsRedraw = true;
    }
    else if (e.ev == PDA_EV_LEFT && sleepSel > 0) { sleepSel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_RIGHT && sleepSel < N_SLEEP - 1) { sleepSel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_SELECT) {
      sleepSave(sleepChoices[sleepSel]);
      needsRedraw = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      sleepSave(sleepChoices[sleepSel]);
      return;
    }
  }
}
