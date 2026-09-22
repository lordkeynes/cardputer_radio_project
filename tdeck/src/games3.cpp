/**
 * Puzzle pack: Connect Four, Battleship, Wordle, Sudoku, Sokoban.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "games.h"
#include "gamestats.h"

extern Arduino_GFX *gfx;

// ============================ Connect Four ============================
#define C4_COLS 7
#define C4_ROWS 6
#define C4_CELL 26
#define C4_OX 37
#define C4_OY 40

struct C4 {
  uint8_t b[C4_ROWS][C4_COLS];   // 0 empty, 1 player red, 2 AI yellow
  int cx;
  bool over;
  uint8_t winner;
};

static void c4Reset(C4 &g) {
  memset(g.b, 0, sizeof(g.b));
  g.cx = 3;
  g.over = false;
  g.winner = 0;
}

static int c4Drop(C4 &g, int col, uint8_t who) {
  for (int y = C4_ROWS - 1; y >= 0; y--)
    if (!g.b[y][col]) { g.b[y][col] = who; return y; }
  return -1;
}

static bool c4WinFrom(C4 &g, int x, int y, uint8_t who) {
  static const int dirs[4][2] = {{1,0},{0,1},{1,1},{1,-1}};
  for (int d = 0; d < 4; d++) {
    int cnt = 1;
    for (int s = -1; s <= 1; s += 2) {
      int nx = x + dirs[d][0] * s, ny = y + dirs[d][1] * s;
      while (nx >= 0 && nx < C4_COLS && ny >= 0 && ny < C4_ROWS &&
             g.b[ny][nx] == who) {
        cnt++;
        nx += dirs[d][0] * s; ny += dirs[d][1] * s;
      }
    }
    if (cnt >= 4) return true;
  }
  return false;
}

static bool c4Full(C4 &g) {
  for (int x = 0; x < C4_COLS; x++)
    if (!g.b[0][x]) return false;
  return true;
}

// score a hypothetical drop for the AI (win > block > center bias)
static int c4ScoreCol(C4 &g, int col) {
  int y = -1;
  for (int yy = C4_ROWS - 1; yy >= 0; yy--)
    if (!g.b[yy][col]) { y = yy; break; }
  if (y < 0) return -1;
  g.b[y][col] = 2;
  int sc = 0;
  if (c4WinFrom(g, col, y, 2)) sc = 100;
  else {
    g.b[y][col] = 1;
    if (c4WinFrom(g, col, y, 1)) sc = 50;
  }
  g.b[y][col] = 0;
  if (sc == 0) sc = 4 - abs(col - 3);   // prefer center
  return sc;
}

static void c4AIMove(C4 &g) {
  int best = -1, bestSc = -1;
  for (int x = 0; x < C4_COLS; x++) {
    int sc = c4ScoreCol(g, x);
    if (sc > bestSc) { bestSc = sc; best = x; }
  }
  if (best >= 0) {
    int y = c4Drop(g, best, 2);
    if (c4WinFrom(g, best, y, 2)) { g.over = true; g.winner = 2; }
  }
}

static void c4Draw(C4 &g, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Connect 4");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->drawRect(C4_OX - 3, C4_OY - 3, C4_COLS * C4_CELL + 6,
                  C4_ROWS * C4_CELL + 6, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("l/r move click=drop Long=back");
    for (int y = 0; y < C4_ROWS; y++)
      for (int x = 0; x < C4_COLS; x++)
        if (g.b[y][x]) {
          gfx->fillCircle(C4_OX + x * C4_CELL + C4_CELL / 2,
                          C4_OY + y * C4_CELL + C4_CELL / 2,
                          C4_CELL / 2 - 3,
                          g.b[y][x] == 1 ? TERM_ACCENT : TERM_BRIGHT);
        }
  }
  // column cursor
  static int lastCx = -1;
  if (lastCx >= 0)
    gfx->fillRect(C4_OX + lastCx * C4_CELL, C4_OY - 12, C4_CELL, 8, BLACK);
  gfx->fillRect(C4_OX + g.cx * C4_CELL + 8, C4_OY - 12, C4_CELL - 16, 8,
                TERM_BRIGHT);
  lastCx = g.cx;
  if (g.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(g.winner == 1 ? TERM_GREEN : g.winner == 2 ? TERM_RED : TERM_DIM,
                      BLACK);
    gfx->setCursor(90, 240 - 90);
    gfx->print(g.winner == 1 ? "WIN!" : g.winner == 2 ? "AI WIN" : "DRAW");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(90, 240 - 72);
    gfx->print("click/n = again");
  }
}

void connect4App() {
  C4 g;
  c4Reset(g);
  bool full = true;
  while (true) {
    if (full) { c4Draw(g, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (g.over) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
          (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
        c4Reset(g); full = true;
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    if (e.ev == PDA_EV_LEFT && g.cx > 0) { g.cx--; c4Draw(g, false); }
    else if (e.ev == PDA_EV_RIGHT && g.cx < C4_COLS - 1) { g.cx++; c4Draw(g, false); }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      int y = c4Drop(g, g.cx, 1);
      if (y < 0) continue;   // column full
      gfx->fillCircle(C4_OX + g.cx * C4_CELL + C4_CELL / 2,
                      C4_OY + y * C4_CELL + C4_CELL / 2,
                      C4_CELL / 2 - 3, TERM_ACCENT);
      if (c4WinFrom(g, g.cx, y, 1)) {
        g.over = true; g.winner = 1;
        gsRecordResult(GS_CONNECT4, true);
        full = true;
        continue;
      }
      if (c4Full(g)) { g.over = true; g.winner = 0; full = true; continue; }
      c4AIMove(g);
      if (g.over) gsRecordResult(GS_CONNECT4, false);
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

// ============================ Battleship ============================
#define BS_N 8
#define BS_CELL 18
#define BS_OY 42
#define BS_OX 14
#define BS_OX2 (BS_OX + BS_N * BS_CELL + 16)

struct Battleship {
  uint8_t ships[2][BS_N][BS_N];   // [0]=player fleet, [1]=AI fleet
  uint8_t shots[2][BS_N][BS_N];   // [0]=player shots on AI, [1]=AI shots on player
  int cx, cy;
  bool over;
  int playerFleet, aiFleet;
};

static const int bsShipSizes[5] = {4, 3, 3, 2, 2};

static bool bsCanPlace(Battleship &b, int grid, int x, int y, int len, int horiz) {
  for (int i = 0; i < len; i++) {
    int nx = horiz ? x + i : x;
    int ny = horiz ? y : y + i;
    if (nx >= BS_N || ny >= BS_N) return false;
    if (b.ships[grid][ny][nx]) return false;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        int ax = nx + dx, ay = ny + dy;
        if (ax >= 0 && ax < BS_N && ay >= 0 && ay < BS_N &&
            b.ships[grid][ay][ax]) return false;
      }
  }
  return true;
}

static void bsPlace(Battleship &b, int grid, int x, int y, int len, int horiz, uint8_t id) {
  for (int i = 0; i < len; i++) {
    int nx = horiz ? x + i : x;
    int ny = horiz ? y : y + i;
    b.ships[grid][ny][nx] = id;
  }
}

static void bsAutoPlace(Battleship &b, int grid) {
  memset(b.ships[grid], 0, sizeof(b.ships[grid]));
  for (int s = 0; s < 5; s++) {
    while (true) {
      int horiz = random(2);
      int x = random(BS_N), y = random(BS_N);
      if (bsCanPlace(b, grid, x, y, bsShipSizes[s], horiz)) {
        bsPlace(b, grid, x, y, bsShipSizes[s], horiz, s + 1);
        break;
      }
    }
  }
}

static void bsReset(Battleship &b) {
  memset(b.ships[0], 0, sizeof(b.ships[0]));
  memset(b.shots, 0, sizeof(b.shots));
  b.cx = 0; b.cy = 0;
  b.over = false;
  bsAutoPlace(b, 1);          // AI fleet; player places their own
  b.playerFleet = 14; b.aiFleet = 14;
}

static void bsDrawBoard(Battleship &b, int whichShots, int ox, bool revealShips);

// manual ship-placement phase: move cursor, r = rotate, click = place,
// a = auto-place the rest. Runs until all 5 ships are down.
static void bsPlacement(Battleship &b) {
  int shipIdx = 0;
  bool horiz = true;
  int ox = (SCREEN_W - BS_N * BS_CELL) / 2;
  while (shipIdx < 5) {
    int len = bsShipSizes[shipIdx];
    bool fits = bsCanPlace(b, 0, b.cx, b.cy, len, horiz);
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Battleship - place fleet");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_ACCENT, BLACK);
    gfx->setCursor(ox, 26);
    gfx->printf("ship %d/5: %s (%d)", shipIdx + 1, horiz ? "H" : "V", len);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("u/d/l/r move r=rotate click=place a=auto Long=quit");
    bsDrawBoard(b, 1, ox, true);           // player board + ships so far
    // ghost preview of the ship under the cursor
    for (int i = 0; i < len; i++) {
      int nx = horiz ? b.cx + i : b.cx;
      int ny = horiz ? b.cy : b.cy + i;
      if (nx >= BS_N || ny >= BS_N) break;
      gfx->drawRect(ox + nx * BS_CELL + 2, BS_OY + ny * BS_CELL + 2,
                    BS_CELL - 5, BS_CELL - 5,
                    fits ? TERM_ACCENT : TERM_RED);
    }
    gfx->drawRect(ox + b.cx * BS_CELL, BS_OY + b.cy * BS_CELL,
                  BS_CELL, BS_CELL, TERM_BRIGHT);
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && b.cy > 0) b.cy--;
    else if (e.ev == PDA_EV_DOWN && b.cy < BS_N - 1) b.cy++;
    else if (e.ev == PDA_EV_LEFT && b.cx > 0) b.cx--;
    else if (e.ev == PDA_EV_RIGHT && b.cx < BS_N - 1) b.cx++;
    else if (e.ev == PDA_EV_CHAR && (e.ch == 'r' || e.ch == 'R')) horiz = !horiz;
    else if (e.ev == PDA_EV_CHAR && (e.ch == 'a' || e.ch == 'A')) {
      bsAutoPlace(b, 0);
      return;
    }
    else if ((e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) && fits) {
      bsPlace(b, 0, b.cx, b.cy, len, horiz, shipIdx + 1);
      shipIdx++;
      if (b.cx > 0) b.cx--;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      // back out entirely: auto-place so quitting mid-placement is safe
      bsAutoPlace(b, 0);
      return;
    }
  }
}

// draw one board; whichShots: 0 = player's shots on AI fleet, 1 = AI's shots on player fleet
static void bsDrawBoard(Battleship &b, int whichShots, int ox, bool revealShips) {
  uint8_t fleet = (whichShots == 0) ? 1 : 0;   // the fleet being shot at
  const uint16_t WATER  = RGB565(0x0a, 0x1e, 0x30);
  const uint16_t WATER2 = RGB565(0x10, 0x2a, 0x44);
  const uint16_t HITC   = RGB565(0xc0, 0x30, 0x30);
  const uint16_t SHIPC   = RGB565(0x70, 0x88, 0x98);
  // checkerboard water
  for (int y = 0; y < BS_N; y++)
    for (int x = 0; x < BS_N; x++) {
      int px = ox + x * BS_CELL, py = BS_OY + y * BS_CELL;
      gfx->fillRect(px, py, BS_CELL - 1, BS_CELL - 1, ((x + y) & 1) ? WATER2 : WATER);
    }
  // ships / shots overlay
  for (int y = 0; y < BS_N; y++)
    for (int x = 0; x < BS_N; x++) {
      int px = ox + x * BS_CELL, py = BS_OY + y * BS_CELL;
      bool shot = b.shots[whichShots][y][x];
      bool shipHere = b.ships[fleet][y][x];
      if (revealShips && shipHere && !shot)
        gfx->fillRect(px + 2, py + 2, BS_CELL - 5, BS_CELL - 5, SHIPC);
      if (shot) {
        if (shipHere) {
          gfx->fillRect(px + 1, py + 1, BS_CELL - 3, BS_CELL - 3, HITC);
          gfx->setTextSize(1);
          gfx->setTextColor(RGB565(255, 255, 255), HITC);
          gfx->setCursor(px + 5, py + 5);
          gfx->print("X");
        } else {
          gfx->fillCircle(px + (BS_CELL - 1) / 2, py + (BS_CELL - 1) / 2, 2, RGB565(0x90, 0xb8, 0xd8));
        }
      }
    }
  gfx->drawRect(ox - 1, BS_OY - 1, BS_N * BS_CELL + 1, BS_N * BS_CELL + 1, TERM_DIM);
  // coordinates: A-H columns, 1-8 rows
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_DIM, BLACK);
  for (int x = 0; x < BS_N; x++) {
    char cl[2] = {(char)('A' + x), 0};
    gfx->setCursor(ox + x * BS_CELL + 5, BS_OY - 9);
    gfx->print(cl);
  }
  for (int y = 0; y < BS_N; y++) {
    gfx->setCursor(ox - 7, BS_OY + y * BS_CELL + 5);
    gfx->print(y + 1);
  }
}

static void bsDraw(Battleship &b, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Battleship");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_ACCENT, BLACK);
    gfx->setCursor(BS_OX, 26);
    gfx->print("ENEMY");
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(BS_OX2, 26);
    gfx->print("YOURS");
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->print("u/d/l/r click=fire Long=back");
    bsDrawBoard(b, 0, BS_OX, false);     // enemy: shots only
    bsDrawBoard(b, 1, BS_OX2, true);     // yours: ships shown
    // divider between the two boards
    int dvx = BS_OX + BS_N * BS_CELL + 7;
    for (int y = 0; y < BS_N * BS_CELL; y += 6)
      gfx->drawFastVLine(dvx, BS_OY + y, 4, TERM_DIM);
  }
  // cursor on the enemy board
  static int lcx = -1, lcy = -1;
  if (lcx >= 0 && lcy >= 0)
    gfx->drawRect(BS_OX + lcx * BS_CELL, BS_OY + lcy * BS_CELL,
                  BS_CELL, BS_CELL, RGB565(0x0c, 0x14, 0x0c));
  gfx->drawRect(BS_OX + b.cx * BS_CELL, BS_OY + b.cy * BS_CELL,
                BS_CELL, BS_CELL, TERM_BRIGHT);
  lcx = b.cx; lcy = b.cy;
  // fleet counters
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(BS_OX + 56, 26);
  gfx->printf("E%d", b.aiFleet);
  gfx->setCursor(BS_OX2 + 56, 26);
  gfx->printf("Y%d", b.playerFleet);
  if (b.over) {
    gfx->setTextSize(2);
    bool win = b.playerFleet > 0 && b.aiFleet == 0;
    gfx->setTextColor(win ? TERM_GREEN : TERM_RED, BLACK);
    gfx->setCursor(110, 120);
    gfx->print(win ? "YOU WIN!" : "YOU LOSE");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(116, 145);
    gfx->print("click/n = play again");
  }
}

static void bsAIShoot(Battleship &b) {
  while (true) {
    int x = random(BS_N), y = random(BS_N);
    if (b.shots[1][y][x]) continue;
    b.shots[1][y][x] = 1;
    if (b.ships[0][y][x]) b.playerFleet--;
    return;
  }
}

void battleshipApp() {
  Battleship b;
  bsReset(b);
  bsPlacement(b);
  bool full = true;
  while (true) {
    if (full) { bsDraw(b, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (b.over) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
          (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
        bsReset(b); bsPlacement(b); full = true;
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    if (e.ev == PDA_EV_UP && b.cy > 0) { b.cy--; bsDraw(b, false); }
    else if (e.ev == PDA_EV_DOWN && b.cy < BS_N - 1) { b.cy++; bsDraw(b, false); }
    else if (e.ev == PDA_EV_LEFT && b.cx > 0) { b.cx--; bsDraw(b, false); }
    else if (e.ev == PDA_EV_RIGHT && b.cx < BS_N - 1) { b.cx++; bsDraw(b, false); }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (b.shots[0][b.cy][b.cx]) continue;   // already shot
      b.shots[0][b.cy][b.cx] = 1;
      if (b.ships[1][b.cy][b.cx]) b.aiFleet--;
      if (b.aiFleet == 0) {
        b.over = true;
        gsRecordResult(GS_BATTLESHIP, true);
        full = true;
        continue;
      }
      bsAIShoot(b);
      if (b.playerFleet == 0) {
        b.over = true;
        gsRecordResult(GS_BATTLESHIP, false);
      }
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

// ============================ Wordle ============================
// 5-letter guess, 6 tries. Keyboard input; feedback: green=hit,
// accent=misplaced, dim=miss. 40 built-in answers, no dictionary check
// (any 5 letters accepted as a guess).
#define WD_ROWS 6
#define WD_COLS 5

struct Wordle {
  char answer[6];
  char grid[WD_ROWS][WD_COLS];
  uint8_t fb[WD_ROWS][WD_COLS];   // 0 unknown, 1 hit, 2 misplaced, 3 miss
  int row, col;
  bool over, won;
};

static const char *const wdWords[] = {
  "radio", "pixel", "crypt", "zonal", "quips",
  "brick", "flint", "mango", "plume", "skiff",
  "orbit", "nymph", "glyph", "vixen", "wager",
  "loupe", "terra", "fjord", "banjo", "equip",
  "stead", "rogue", "knoll", "azure", "cider",
  "hoist", "medal", "punch", "grunt", "siege",
  "vowel", "wrist", "yacht", "zesty", "amber",
  "blush", "chase", "douse", "flask", "giver",
  "haven", "inlet", "joust", "kneel", "lithe",
  "mirth", "nudge", "onset", "prism", "quirk",
  "realm", "sonic", "trope", "umbra", "widen"
};
#define WD_NWORDS (int)(sizeof(wdWords)/sizeof(wdWords[0]))

static void wdReset(Wordle &w) {
  strcpy(w.answer, wdWords[random(WD_NWORDS)]);
  memset(w.grid, 0, sizeof(w.grid));
  memset(w.fb, 0, sizeof(w.fb));
  w.row = 0; w.col = 0;
  w.over = false; w.won = false;
}

static void wdScore(Wordle &w) {
  bool used[WD_COLS] = {false};
  // hits first
  for (int i = 0; i < WD_COLS; i++)
    if (w.grid[w.row][i] == w.answer[i]) { w.fb[w.row][i] = 1; used[i] = true; }
  // then misplaced
  for (int i = 0; i < WD_COLS; i++) {
    if (w.fb[w.row][i] == 1) continue;
    w.fb[w.row][i] = 3;
    for (int j = 0; j < WD_COLS; j++)
      if (!used[j] && w.grid[w.row][i] == w.answer[j]) {
        w.fb[w.row][i] = 2; used[j] = true; break;
      }
  }
}

static void wdDraw(Wordle &w, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Wordle");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("type letters Enter=guess BKSP Long=back");
  }
  for (int r = 0; r < WD_ROWS; r++)
    for (int c = 0; c < WD_COLS; c++) {
      int px = 44 + c * 48, py = 28 + r * 30;
      uint16_t bg = BLACK, fg = TERM_DIM;
      if (w.fb[r][c] == 1) { bg = TERM_GREEN; fg = BLACK; }
      else if (w.fb[r][c] == 2) { bg = TERM_ACCENT; fg = BLACK; }
      else if (w.fb[r][c] == 3) { bg = RGB565(0x20, 0x28, 0x20); fg = TERM_DIM; }
      else if (w.grid[r][c]) { bg = RGB565(0x28, 0x38, 0x28); fg = TERM_BRIGHT; }
      gfx->fillRect(px, py, 40, 26, bg);
      gfx->drawRect(px, py, 40, 26, TERM_DIM);
      if (w.grid[r][c]) {
        gfx->setTextSize(2);
        gfx->setTextColor(fg, bg);
        gfx->setCursor(px + 14, py + 6);
        char ch[2] = {w.grid[r][c], 0};
        gfx->print(ch);
      }
      // typing cursor
      if (r == w.row && c == w.col && !w.over)
        gfx->drawRect(px, py, 40, 26, TERM_BRIGHT);
    }
  if (w.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(w.won ? TERM_GREEN : TERM_RED, BLACK);
    gfx->setCursor(236, 40);
    gfx->print(w.won ? "WIN!" : "LOSE");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(236, 70);
    gfx->print(w.answer);
    gfx->setCursor(236, 90);
    gfx->print("n = again");
  }
}

void wordleApp() {
  Wordle w;
  wdReset(w);
  bool full = true;
  while (true) {
    if (full) { wdDraw(w, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (w.over) {
      if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) { wdReset(w); full = true; }
      else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    if (e.ev == PDA_EV_CHAR && e.ch >= 'a' && e.ch <= 'z' && w.col < WD_COLS) {
      w.grid[w.row][w.col++] = e.ch;
      full = true;
    } else if (e.ev == PDA_EV_CHAR && e.ch >= 'A' && e.ch <= 'Z' && w.col < WD_COLS) {
      w.grid[w.row][w.col++] = e.ch - 'A' + 'a';
      full = true;
    } else if (e.ev == PDA_EV_DELETE && w.col > 0) {
      w.grid[w.row][--w.col] = 0;
      full = true;
    } else if ((e.ev == PDA_EV_NEWLINE || e.ev == PDA_EV_SELECT) && w.col == WD_COLS) {
      wdScore(w);
      if (strncmp(w.grid[w.row], w.answer, WD_COLS) == 0) {
        w.over = true; w.won = true;
        gsRecordResult(GS_WORDLE, true);
      } else if (++w.row >= WD_ROWS) {
        w.over = true;
        gsRecordResult(GS_WORDLE, false);
      } else w.col = 0;
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

// ============================ Sudoku ============================
// 9x9 with digits via keyboard 1-9, 0/DEL clears. Built-in puzzle bank
// (hard-coded grids, 0 = empty); "n" cycles puzzles. Error checking is
// against the solution.
#define SU_N 9
#define SU_CELL 20
#define SU_OX 20
#define SU_OY 28

struct Sudoku {
  uint8_t puzzle[SU_N][SU_N];
  uint8_t grid[SU_N][SU_N];
  uint8_t sol[SU_N][SU_N];
  int cx, cy;
  int idx;
  bool over;
};

// 3 puzzles (rows read left-to-right, top-to-bottom; 0 = empty)
static const char *const suPuzzles[] = {
  "530070000600195000098000060800060003400803001700020006060000280000419005000080079",
  "079600034064923805800047000700006009932000017005000403001495000000062351500000000",
  "057210006000008075000007000020000100890172050030006742000001500510720060070695421",
};
#define SU_NP (int)(sizeof(suPuzzles)/sizeof(suPuzzles[0]))

// solutions for the three puzzles above (same order)
static const char *const suSolutions[] = {
  "534678912672195348198342567859761423426853791713924856961537284287419635345286179",
  "279658134164923875853147296748316529932584617615279483381495762497862351526731948",
  "457219836961348275283567914726453189894172653135986742642831597519724368378695421",
};

static void suLoad(Sudoku &s, int idx) {
  s.idx = idx;
  for (int y = 0; y < SU_N; y++)
    for (int x = 0; x < SU_N; x++) {
      char pc = suPuzzles[idx][y * 9 + x];
      char sc = suSolutions[idx][y * 9 + x];
      s.puzzle[y][x] = pc - '0';
      s.grid[y][x] = pc - '0';
      s.sol[y][x] = sc - '0';
    }
  s.cx = 0; s.cy = 0;
  s.over = false;
}

static void suDraw(Sudoku &s, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Sudoku");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("u/d/l/r 1-9 fill 0/del n=puzzle Long=back");
    // grid
    for (int i = 0; i <= SU_N; i++) {
      uint16_t c = (i % 3 == 0) ? TERM_GREEN : TERM_DIM;
      gfx->drawFastVLine(SU_OX + i * SU_CELL, SU_OY, SU_N * SU_CELL, c);
      gfx->drawFastHLine(SU_OX, SU_OY + i * SU_CELL, SU_N * SU_CELL, c);
    }
  }
  for (int y = 0; y < SU_N; y++)
    for (int x = 0; x < SU_N; x++) {
      int px = SU_OX + x * SU_CELL, py = SU_OY + y * SU_CELL;
      gfx->setTextSize(1);
      if (s.grid[y][x]) {
        bool given = s.puzzle[y][x] != 0;
        bool wrong = s.grid[y][x] != s.sol[y][x];
        gfx->setTextColor(wrong ? TERM_RED : (given ? TERM_BRIGHT : TERM_GREEN),
                          BLACK);
        gfx->setCursor(px + 7, py + 6);
        gfx->print((char)('0' + s.grid[y][x]));
      } else {
        gfx->fillRect(px + 1, py + 1, SU_CELL - 1, SU_CELL - 1, BLACK);
      }
    }
  // cursor
  gfx->drawRect(SU_OX + s.cx * SU_CELL + 1, SU_OY + s.cy * SU_CELL + 1,
                SU_CELL - 1, SU_CELL - 1, TERM_ACCENT);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(220, 7);
  gfx->printf("p%d %d/%d ", s.idx + 1, 0, SU_NP);
}

void sudokuApp() {
  Sudoku s;
  suLoad(s, 0);
  bool full = true;
  while (true) {
    if (full) { suDraw(s, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && s.cy > 0) { s.cy--; full = true; }
    else if (e.ev == PDA_EV_DOWN && s.cy < SU_N - 1) { s.cy++; full = true; }
    else if (e.ev == PDA_EV_LEFT && s.cx > 0) { s.cx--; full = true; }
    else if (e.ev == PDA_EV_RIGHT && s.cx < SU_N - 1) { s.cx++; full = true; }
    else if (e.ev == PDA_EV_CHAR && e.ch >= '1' && e.ch <= '9') {
      if (!s.puzzle[s.cy][s.cx]) {
        s.grid[s.cy][s.cx] = e.ch - '0';
        full = true;
        // win check
        bool done = true;
        for (int y = 0; y < SU_N && done; y++)
          for (int x = 0; x < SU_N && done; x++)
            if (s.grid[y][x] != s.sol[y][x]) done = false;
        if (done) {
          s.over = true;
          gsRecordResult(GS_SUDOKU, true);
          gfx->setTextSize(2);
          gfx->setTextColor(TERM_GREEN, BLACK);
          gfx->setCursor(240, 100);
          gfx->print("SOLVED!");
          gfx->setTextSize(1);
          gfx->setTextColor(TERM_BRIGHT, BLACK);
          gfx->setCursor(240, 124);
          gfx->print("n = next puzzle");
        }
      }
    }
    else if ((e.ev == PDA_EV_CHAR && (e.ch == '0' || e.ch == 'd')) ||
             e.ev == PDA_EV_DELETE) {
      if (!s.puzzle[s.cy][s.cx]) { s.grid[s.cy][s.cx] = 0; full = true; }
    }
    else if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
      suLoad(s, (s.idx + 1) % SU_NP);
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

// ============================ Sokoban ============================
// Levels: # wall, @ player, $ box, . goal, * box-on-goal, space floor.
#define SB_MAXW 12
#define SB_MAXH 10
#define SB_LEVELS 5

struct Sokoban {
  uint8_t wall[SB_MAXH][SB_MAXW];
  uint8_t goal[SB_MAXH][SB_MAXW];
  uint8_t box[SB_MAXH][SB_MAXW];
  int px, py;
  int lvl;
  int moves;
  bool over;
};

static const char *const sbLevels[SB_LEVELS] = {
  "########\n"
  "#      #\n"
  "#  $ . #\n"
  "#  @   #\n"
  "########",

  "#########\n"
  "#   #   #\n"
  "# $ . . #\n"
  "#   #   #\n"
  "##@######",

  "  #####\n"
  "###   #\n"
  "#  $  #\n"
  "# .@. #\n"
  "###   #\n"
  "  #####",

  "########\n"
  "#.  $  #\n"
  "#  @   #\n"
  "#  $  .#\n"
  "########",

  "  ######\n"
  "###    #\n"
  "#  $ # #\n"
  "# .@   #\n"
  "#  #$  #\n"
  "#  ..  #\n"
  "########",
};

static void sbLoad(Sokoban &s, int lvl) {
  s.lvl = lvl;
  memset(s.wall, 0, sizeof(s.wall));
  memset(s.goal, 0, sizeof(s.goal));
  memset(s.box, 0, sizeof(s.box));
  int y = 0, x = 0;
  const char *p = sbLevels[lvl];
  while (*p) {
    if (*p == '\n') { y++; x = 0; }
    else {
      if (*p == '#') s.wall[y][x] = 1;
      else if (*p == '.') s.goal[y][x] = 1;
      else if (*p == '$') s.box[y][x] = 1;
      else if (*p == '*') { s.goal[y][x] = 1; s.box[y][x] = 1; }
      else if (*p == '@') { s.px = x; s.py = y; }
      x++;
    }
    p++;
  }
  s.moves = 0;
  s.over = false;
}

static bool sbDone(Sokoban &s) {
  for (int y = 0; y < SB_MAXH; y++)
    for (int x = 0; x < SB_MAXW; x++)
      if (s.goal[y][x] && !s.box[y][x]) return false;
  return true;
}

static void sbDraw(Sokoban &s, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Sokoban");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("u/d/l/r push n=reset Long=back");
  }
  for (int y = 0; y < SB_MAXH; y++)
    for (int x = 0; x < SB_MAXW; x++) {
      int px = 40 + x * 20, py = 30 + y * 20;
      if (s.wall[y][x]) gfx->fillRect(px, py, 20, 20, RGB565(0x24, 0x40, 0x24));
      else {
        gfx->fillRect(px, py, 20, 20, BLACK);
        if (s.goal[y][x]) {
          gfx->drawRect(px + 6, py + 6, 8, 8, TERM_DIM);
          if (!s.box[y][x]) {
            gfx->drawRect(px + 8, py + 8, 4, 4, TERM_DIM);
          }
        }
        if (s.box[y][x]) {
          bool onGoal = s.goal[y][x];
          gfx->fillRect(px + 2, py + 2, 16, 16,
                        onGoal ? TERM_GREEN : RGB565(0x30, 0x50, 0x30));
          gfx->drawRect(px + 2, py + 2, 16, 16, TERM_BRIGHT);
        }
      }
    }
  // player
  gfx->fillCircle(40 + s.px * 20 + 10, 30 + s.py * 20 + 10, 6, TERM_ACCENT);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(220, 7);
  gfx->printf("L%d mv%d  ", s.lvl + 1, s.moves);
  if (s.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(110, 110);
    gfx->print("CLEAR!");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(104, 134);
    gfx->printf("n = level %d", (s.lvl + 1) % SB_LEVELS + 1);
  }
}

void sokobanApp() {
  Sokoban s;
  sbLoad(s, 0);
  bool full = true;
  while (true) {
    if (full) { sbDraw(s, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (s.over) {
      if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
        sbLoad(s, (s.lvl + 1) % SB_LEVELS);
        full = true;
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    int dx = 0, dy = 0;
    if (e.ev == PDA_EV_UP) dy = -1;
    else if (e.ev == PDA_EV_DOWN) dy = 1;
    else if (e.ev == PDA_EV_LEFT) dx = -1;
    else if (e.ev == PDA_EV_RIGHT) dx = 1;
    else if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
      sbLoad(s, s.lvl); full = true; continue;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
    else continue;
    int nx = s.px + dx, ny = s.py + dy;
    if (s.wall[ny][nx]) continue;
    if (s.box[ny][nx]) {
      int bx = nx + dx, by = ny + dy;
      if (s.wall[by][bx] || s.box[by][bx]) continue;   // can't push
      s.box[ny][nx] = 0;
      s.box[by][bx] = 1;
    }
    s.px = nx; s.py = ny;
    s.moves++;
    if (sbDone(s)) {
      s.over = true;
      gsRecordResult(GS_SOKOBAN, true);
    }
    full = true;
  }
}
