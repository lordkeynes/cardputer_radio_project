/** * Game statistics: per-game wins/losses + Snake high score. * Persisted to /games/stats.txt on SD as one line per game: *   chess W 3 L 5 *   snake hi 120 */
#include <Arduino.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "games.h"
#include "gamestats.h"

extern Arduino_GFX *gfx;
extern bool sdOk;

static int statsWins[GS_N] = {0};
static int statsLosses[GS_N] = {0};
static int snakeHi = 0;
static bool statsLoaded = false;

static const char *const gsNames[GS_N] = {
  "Chess", "Go", "Solitaire", "Checkers", "Snake"
};

static void gsLoad() {
  if (statsLoaded) return;
  statsLoaded = true;
  if (!sdOk || !SD.exists("/games/stats.txt")) return;
  File f = SD.open("/games/stats.txt", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("snake hi ")) {
      snakeHi = line.substring(9).toInt();
      continue;
    }
    int w = line.indexOf(" W ");
    if (w < 0) continue;
    int l = line.indexOf(" L ", w);
    if (l < 0) continue;
    String name = line.substring(0, w);
    for (int i = 0; i < GS_N; i++) {
      if (name.equals(gsNames[i])) {
        statsWins[i] = line.substring(w + 3, l).toInt();
        statsLosses[i] = line.substring(l + 3).toInt();
        break;
      }
    }
  }
  f.close();
}

static void gsSave() {
  if (!sdOk) return;
  if (!SD.exists("/games")) SD.mkdir("/games");
  File f = SD.open("/games/stats.txt", FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < GS_N; i++) {
    if (i == GS_SNAKE) { f.printf("Snake hi %d\n", snakeHi); continue; }
    if (statsWins[i] || statsLosses[i])
      f.printf("%s W %d L %d\n", gsNames[i], statsWins[i], statsLosses[i]);
  }
  f.close();
}

void gsRecordResult(int game, bool win) {
  gsLoad();
  if (game < 0 || game >= GS_N) return;
  if (win) statsWins[game]++;
  else statsLosses[game]++;
  gsSave();
}

void gsRecordScore(int game, int score) {
  gsLoad();
  if (game != GS_SNAKE) return;
  if (score > snakeHi) { snakeHi = score; gsSave(); }
}

int gsGetSnakeHi() { gsLoad(); return snakeHi; }

void gsStatsScreen() {
  gsLoad();
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(8, 6);
  gfx->print("Game stats");
  gfx->setTextSize(1);
  int y = 36;
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(8, y);
  gfx->print("Game        W    L");
  y += 16;
  for (int i = 0; i < GS_N; i++) {
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(8, y);
    gfx->print(gsNames[i]);
    if (i == GS_SNAKE) {
      gfx->setCursor(120, y);
      gfx->printf("hi %d", snakeHi);
    } else {
      gfx->setCursor(120, y);
      gfx->printf("%-5d%-5d", statsWins[i], statsLosses[i]);
    }
    y += 16;
  }
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("r = reset stats   Any key = back");
  while (true) {
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_CHAR && (e.ch == 'r' || e.ch == 'R')) {
      for (int i = 0; i < GS_N; i++) { statsWins[i] = 0; statsLosses[i] = 0; }
      snakeHi = 0;
      gsSave();
      gsStatsScreen();
      return;
    }
    return;
  }
}
