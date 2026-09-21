/** * Runtime themes for the terminal GUI. * A theme is one struct; the active theme's colors are copied into globals * so existing TERM_* usage compiles and behaves unchanged. The choice is * persisted to /config/theme.txt on SD (one decimal index line). */
#include <Arduino.h>
#include <SD.h>
#include "theme.h"

extern bool sdOk;

uint16_t TERM_GREEN, TERM_BRIGHT, TERM_DIM, TERM_SEL_BG,
         TERM_ACCENT, TERM_RED, TERM_CYAN;

static const Theme themes[] = {
  // 0: Phosphor — classic P1 green phosphor CRT
  {"Phosphor",
   RGB565(51, 255, 51),   // fg: phosphor green
   RGB565(140, 255, 140),  // bright
   RGB565(28, 142, 28),    // dim
   RGB565(0, 96, 0),       // selBg
   RGB565(255, 255, 130),  // accent: pale yellow
   RGB565(255, 80, 80),    // red
   RGB565(80, 220, 255)},  // cyan

  // 1: Amber — P3 amber phosphor
  {"Amber",
   RGB565(255, 176, 0),
   RGB565(255, 225, 150),
   RGB565(170, 110, 0),
   RGB565(140, 90, 0),
   RGB565(255, 255, 130),
   RGB565(255, 80, 80),
   RGB565(80, 220, 255)},

  // 2: Ice — cyan/white on black
  {"Ice",
   RGB565(120, 220, 255),
   RGB565(220, 245, 255),
   RGB565(70, 140, 180),
   RGB565(0, 70, 110),
   RGB565(255, 255, 130),
   RGB565(255, 80, 80),
   RGB565(255, 255, 255)},

  // 3: Crimson — red terminal
  {"Crimson",
   RGB565(255, 80, 80),
   RGB565(255, 170, 170),
   RGB565(150, 40, 40),
   RGB565(120, 20, 20),
   RGB565(255, 220, 120),
   RGB565(255, 120, 120),
   RGB565(80, 220, 255)},

  // 4: Mono — paper white on black
  {"Mono",
   RGB565(235, 235, 235),
   RGB565(255, 255, 255),
   RGB565(130, 130, 130),
   RGB565(70, 70, 70),
   RGB565(255, 220, 120),
   RGB565(255, 80, 80),
   RGB565(80, 220, 255)},
};
static const int N_THEMES = sizeof(themes) / sizeof(themes[0]);

static void applyTheme(int idx) {
  if (idx < 0 || idx >= N_THEMES) idx = 0;
  const Theme &t = themes[idx];
  TERM_GREEN  = t.fg;
  TERM_BRIGHT = t.bright;
  TERM_DIM    = t.dim;
  TERM_SEL_BG = t.selBg;
  TERM_ACCENT = t.accent;
  TERM_RED    = t.red;
  TERM_CYAN   = t.cyan;
}

int themeIndex() { static int cur = 0; return cur; }

void themeSet(int idx) {
  applyTheme(idx);
  // persist to SD if available
  if (sdOk) {
    if (!SD.exists("/config")) SD.mkdir("/config");
    File f = SD.open("/config/theme.txt", FILE_WRITE);
    if (f) { f.printf("%d\n", idx); f.close(); }
  }
}

void themeInit() {
  int idx = 0;
  if (sdOk && SD.exists("/config/theme.txt")) {
    File f = SD.open("/config/theme.txt", FILE_READ);
    if (f) {
      String s = f.readStringUntil('\n');
      f.close();
      int v = s.toInt();
      if (v >= 0 && v < N_THEMES) idx = v;
    }
  }
  applyTheme(idx);
}

int themeCount() { return N_THEMES; }

const char *themeName(int idx) {
  if (idx < 0 || idx >= N_THEMES) return "?";
  return themes[idx].name;
}
