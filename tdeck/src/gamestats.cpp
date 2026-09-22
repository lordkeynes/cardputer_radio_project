/**
 * Game statistics: per-game wins/losses + persistent high scores.
 * Persisted to /games/stats.txt on SD, one line per game:
 *   chess W 3 L 5
 *   snake hi 120
 */
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
static int statsHi[GS_N] = {0};
static bool statsLoaded = false;

static const char *const gsNames[GS_N] = {
  "Chess", "Go", "Solitaire", "Checkers", "Snake", "Flappy",
  "Tetris", "Breakout", "2048", "Mines", "Pong", "Reversi",
  "Connect4", "Battleship", "Wordle", "Sudoku", "Sokoban",
  "Invaders", "Asteroids", "Doodle", "Hearts", "Spades", "Backgammon"};

static void gsLoad() {
  if (statsLoaded) return;
  statsLoaded = true;
  if (!sdOk || !SD.exists("/games/stats.txt")) return;
  File f = SD.open("/games/stats.txt", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int w = line.indexOf(" W ");
    if (w >= 0) {
      int l = line.indexOf(" L ", w);
      if (l < 0) continue;
      String name = line.substring(0, w);
      for (int i = 0; i < GS_N; i++) {
        if (name.equalsIgnoreCase(gsNames[i])) {
          statsWins[i] = line.substring(w + 3, l).toInt();
          statsLosses[i] = line.substring(l + 3).toInt();
          break;
        }
      }
      continue;
    }
    int hi = line.indexOf(" hi ");
    if (hi >= 0) {
      String name = line.substring(0, hi);
      for (int i = 0; i < GS_N; i++) {
        if (name.equalsIgnoreCase(gsNames[i])) {
          statsHi[i] = line.substring(hi + 4).toInt();
          break;
        }
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
    if (statsHi[i] > 0)
      f.printf("%s hi %d\n", gsNames[i], statsHi[i]);
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
  if (game < 0 || game >= GS_N) return;
  if (score > statsHi[game]) { statsHi[game] = score; gsSave(); }
}

int gsGetHi(int game) { gsLoad(); return (game >= 0 && game < GS_N) ? statsHi[game] : 0; }
int gsGetSnakeHi() { return gsGetHi(GS_SNAKE); }
int gsGetFlappyHi() { return gsGetHi(GS_FLAPPY); }

void gsStatsScreen() {
  gsLoad();
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(8, 6);
  gfx->print("Game stats");
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(8, 26);
  gfx->print("Game          hi / W-L   Game          hi / W-L");
  int colh = (GS_N + 1) / 2;
  for (int i = 0; i < GS_N; i++) {
    int cx = (i / colh) * 156;
    int y = 38 + (i % colh) * 13;
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(8 + cx, y);
    gfx->print(gsNames[i]);
    gfx->setCursor(88 + cx, y);
    if (statsHi[i] > 0 || (statsWins[i] == 0 && statsLosses[i] == 0))
      gfx->printf("hi%d", statsHi[i]);
    else
      gfx->printf("%d-%d", statsWins[i], statsLosses[i]);
  }
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("r = reset stats   Any key = back");
  while (true) {
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_CHAR && (e.ch == 'r' || e.ch == 'R')) {
      for (int i = 0; i < GS_N; i++) { statsWins[i] = 0; statsLosses[i] = 0; statsHi[i] = 0; }
      gsSave();
      gfx->fillScreen(BLACK);
      // re-enter the draw loop instead of recursing (stack-safe reset)
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("r = reset stats   Any key = back");
      for (int i = 0; i < GS_N; i++) {
        int cx = (i / colh) * 156;
        int y = 38 + (i % colh) * 13;
        gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(8 + cx, y);
        gfx->print(gsNames[i]);
        gfx->setCursor(88 + cx, y);
        gfx->printf("0-0");
      }
      continue;
    }
    return;
  }
}
