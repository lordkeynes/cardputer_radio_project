/**
 * Second games pack: Tetris, Breakout, 2048, Minesweeper, Pong, Reversi.
 * All use the shared input queue (pda.h), theme colors, and game stats.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Arduino_GFX.h>
#include "pda.h"
#include "theme.h"
#include "games.h"
#include "gamestats.h"

extern Arduino_GFX *gfx;

// ============================ Tetris ============================
#define T_COLS 10
#define T_ROWS 18
#define T_CELL 12
#define T_OX 26
#define T_OY 28

struct Tetris {
  uint8_t board[T_ROWS][T_COLS];   // 0 empty, 1-7 piece color id
  int8_t px, py;                    // piece position
  int8_t pid, rot;                  // piece id 0-6, rotation 0-3
  int8_t nextPid;
  int score, lines;
  bool over, started;
};

// piece shapes: 4 rotations each, 4x4 bit grids (bit15 = top-left),
// all 28 rotations verified: 4 cells, connected
static const uint16_t tShapes[7][4] = {
  {0x0F00, 0x2222, 0x00F0, 0x4444},  // I
  {0x0660, 0x0660, 0x0660, 0x0660},  // O
  {0x0E40, 0x2620, 0x0270, 0x0464},  // T
  {0x0E20, 0x2260, 0x0470, 0x0644},  // J
  {0x0E80, 0x6220, 0x0170, 0x0446},  // L
  {0x06C0, 0x4620, 0x0360, 0x0462},  // S
  {0x0C60, 0x2640, 0x0630, 0x0264},  // Z
};
static bool tCellOn(int pid, int rot, int x, int y) {
  if (x < 0 || x > 3 || y < 0 || y > 3) return false;
  return tShapes[pid][rot] & (0x8000 >> (y * 4 + x));
}

static void tSpawn(Tetris &t) {
  t.pid = t.nextPid;
  t.nextPid = random(7);
  t.rot = 0;
  t.px = 3;
  t.py = 0;
  // game over if the spawn cell is occupied
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++)
      if (tCellOn(t.pid, 0, x, y) &&
          (t.py + y >= T_ROWS || t.px + x >= T_COLS || t.board[t.py + y][t.px + x]))
        t.over = true;
}

static void tReset(Tetris &t) {
  memset(t.board, 0, sizeof(t.board));
  t.score = 0; t.lines = 0;
  t.over = false; t.started = false;
  t.nextPid = random(7);
  tSpawn(t);
}

static bool tFits(Tetris &t, int nx, int ny, int nrot) {
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++) {
      if (!tCellOn(t.pid, nrot, x, y)) continue;
      int bx = nx + x, by = ny + y;
      if (bx < 0 || bx >= T_COLS || by >= T_ROWS) return false;
      if (by >= 0 && t.board[by][bx]) return false;
    }
  return true;
}

static void tCellColor(uint8_t v, int x, int y) {
  uint16_t cols[8] = {BLACK, TERM_GREEN, TERM_CYAN, TERM_ACCENT,
                      TERM_BRIGHT, TERM_RED, TERM_GREEN, TERM_DIM};
  gfx->fillRect(T_OX + x * T_CELL + 1, T_OY + y * T_CELL + 1,
                T_CELL - 2, T_CELL - 2, cols[v & 7]);
}

static void tDraw(Tetris &t, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Tetris");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->drawRect(T_OX - 2, T_OY - 2, T_COLS * T_CELL + 4, T_ROWS * T_CELL + 4, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(T_OX + T_COLS * T_CELL + 14, T_OY);
    gfx->print("Next:");
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("l/r move d drop u rot Long=back");
    for (int y = 0; y < T_ROWS; y++)
      for (int x = 0; x < T_COLS; x++)
        if (t.board[y][x]) tCellColor(t.board[y][x], x, y);
  }
  // score line
  gfx->fillRect(T_OX + T_COLS * T_CELL + 14, T_OY + 40, 60, 24, BLACK);
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(T_OX + T_COLS * T_CELL + 14, T_OY + 40);
  gfx->printf("%d", t.score);
  gfx->setCursor(T_OX + T_COLS * T_CELL + 14, T_OY + 52);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->printf("L%d hi%d", t.lines, gsGetHi(GS_TETRIS));
  // next preview (right panel, drawn directly)
  int nx0 = T_OX + T_COLS * T_CELL + 14, ny0 = T_OY + 14;
  gfx->fillRect(nx0, ny0, 48, 24, BLACK);
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++)
      if (tCellOn(t.nextPid, 0, x, y))
        gfx->fillRect(nx0 + x * T_CELL + 1, ny0 + y * T_CELL + 1,
                      T_CELL - 2, T_CELL - 2, TERM_CYAN);
  // active piece
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++)
      if (tCellOn(t.pid, t.rot, x, y) && t.py + y >= 0)
        tCellColor(t.pid + 1, t.px + x, t.py + y);
  if (t.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_RED, BLACK);
    gfx->setCursor(80, 100);
    gfx->print("OVER!");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(76, 120);
    gfx->print("click/n = play again");
  }
  if (!t.started && !t.over) {
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(80, 110);
    gfx->print("click to start");
  }
}

static void tLockPiece(Tetris &t) {
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++)
      if (tCellOn(t.pid, t.rot, x, y) && t.py + y >= 0)
        t.board[t.py + y][t.px + x] = t.pid + 1;
  // clear full rows
  int cleared = 0;
  for (int y = T_ROWS - 1; y >= 0; y--) {
    bool full = true;
    for (int x = 0; x < T_COLS; x++)
      if (!t.board[y][x]) { full = false; break; }
    if (full) {
      cleared++;
      for (int yy = y; yy > 0; yy--)
        for (int x = 0; x < T_COLS; x++)
          t.board[yy][x] = t.board[yy - 1][x];
      for (int x = 0; x < T_COLS; x++) t.board[0][x] = 0;
      y++;
    }
  }
  if (cleared) {
    t.lines += cleared;
    t.score += (cleared == 1) ? 40 : cleared * 100;
    if (t.score > gsGetHi(GS_TETRIS)) gsRecordScore(GS_TETRIS, t.score);
    tDraw(t, true);   // repaint board after row clears
  }
  tSpawn(t);
}

static bool tStep(Tetris &t) {
  if (tFits(t, t.px, t.py + 1, t.rot)) { t.py++; return true; }
  tLockPiece(t);
  return false;
}

void tetrisApp() {
  Tetris t;
  tReset(t);
  bool full = true;
  unsigned long lastStep = 0;
  int stepMs = 500;
  while (true) {
    if (full) { tDraw(t, true); full = false; }
    InputEventP e;
    bool got = pdaGetInput(e, 20);
    if (got) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        if (t.over) { tReset(t); full = true; }
        else t.started = true;
      } else if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
        if (t.over) { tReset(t); full = true; }
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
        if (t.score > gsGetHi(GS_TETRIS)) gsRecordScore(GS_TETRIS, t.score);
        return;
      }
      if (t.started && !t.over) {
        if (e.ev == PDA_EV_LEFT && tFits(t, t.px - 1, t.py, t.rot)) { t.px--; tDraw(t, false); }
        else if (e.ev == PDA_EV_RIGHT && tFits(t, t.px + 1, t.py, t.rot)) { t.px++; tDraw(t, false); }
        else if (e.ev == PDA_EV_UP) {
          int nr = (t.rot + 1) & 3;
          if (tFits(t, t.px, t.py, nr)) { t.rot = nr; tDraw(t, false); }
        } else if (e.ev == PDA_EV_DOWN) {
          while (tFits(t, t.px, t.py + 1, t.rot)) t.py++;
          tDraw(t, false);
        }
      }
    }
    if (!t.started || t.over) continue;
    if (millis() - lastStep < (unsigned)stepMs) continue;
    lastStep = millis();
    if (t.lines >= 2 && stepMs > 120) stepMs -= 10;
    tStep(t);
    tDraw(t, false);
    if (t.over) gsRecordScore(GS_TETRIS, t.score);
  }
}

// ============================ Breakout ============================
#define BR_OX 20
#define BR_OY 30
#define BR_W 280
#define BR_H 190
#define BR_ROWS 5
#define BR_COLS 10
#define BR_BW (BR_W / BR_COLS)
#define BR_BH 12
#define BR_PW 44
#define BR_PH 6

struct Breakout {
  uint8_t bricks[BR_ROWS][BR_COLS];
  float bx, by, bvx, bvy;
  int paddleX, score, lives;
  bool over, started, ballOnPaddle;
};

static void brReset(Breakout &b) {
  memset(b.bricks, 1, sizeof(b.bricks));
  b.paddleX = BR_OX + BR_W / 2 - BR_PW / 2;
  b.score = 0; b.lives = 3;
  b.over = false; b.started = false; b.ballOnPaddle = true;
}

static void brDraw(Breakout &b, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Breakout");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->drawRect(BR_OX - 1, BR_OY - 1, BR_W + 2, BR_H + 2, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("l/r paddle click=launch Long=back");
    for (int r = 0; r < BR_ROWS; r++)
      for (int c = 0; c < BR_COLS; c++)
        if (b.bricks[r][c])
          gfx->fillRect(BR_OX + c * BR_BW + 1, BR_OY + r * BR_BH + 1,
                        BR_BW - 2, BR_BH - 2,
                        r % 2 ? TERM_ACCENT : TERM_GREEN);
  }
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(BR_OX + BR_W + 8, 8);
  gfx->printf("%d   ", b.score);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(BR_OX + BR_W + 60, 8);
  gfx->printf("L%d hi%d", b.lives, gsGetHi(GS_BREAKOUT));
  gfx->fillRect(BR_OX, BR_OY + BR_ROWS * BR_BH, BR_W, BR_H - BR_ROWS * BR_BH, BLACK);
  gfx->fillRect(b.paddleX, BR_OY + BR_H - BR_PH - 2, BR_PW, BR_PH, TERM_BRIGHT);
  gfx->fillCircle((int)b.bx, (int)b.by, 3, TERM_ACCENT);
  if (b.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(b.lives ? TERM_GREEN : TERM_RED, BLACK);
    gfx->setCursor(BR_OX + 70, BR_OY + 80);
    gfx->print(b.lives ? "WIN!" : "OVER!");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(BR_OX + 62, BR_OY + 104);
    gfx->print("click or n = play again");
  }
  if (b.ballOnPaddle && !b.over) {
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(BR_OX + 66, BR_OY + 100);
    gfx->print(b.started ? "click to launch" : "click to start");
  }
}

static void brLaunch(Breakout &b) {
  b.ballOnPaddle = false;
  b.bx = b.paddleX + BR_PW / 2;
  b.by = BR_OY + BR_H - BR_PH - 8;
  b.bvx = 2.2f; b.bvy = -3.2f;
}

void breakoutApp() {
  Breakout b;
  brReset(b);
  bool full = true;
  unsigned long lastStep = 0;
  while (true) {
    if (full) { brDraw(b, true); full = false; }
    InputEventP e;
    bool got = pdaGetInput(e, 16);
    if (got) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        if (b.over) { brReset(b); full = true; }
        else {
          b.started = true;
          if (b.ballOnPaddle) brLaunch(b);
        }
      } else if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
        if (b.over) { brReset(b); full = true; }
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
        gsRecordScore(GS_BREAKOUT, b.score);
        return;
      }
      if (b.started && !b.over) {
        if (e.ev == PDA_EV_LEFT) b.paddleX -= 14;
        else if (e.ev == PDA_EV_RIGHT) b.paddleX += 14;
        if (b.paddleX < BR_OX) b.paddleX = BR_OX;
        if (b.paddleX > BR_OX + BR_W - BR_PW) b.paddleX = BR_OX + BR_W - BR_PW;
      }
    }
    if (b.ballOnPaddle) {
      b.bx = b.paddleX + BR_PW / 2;
      b.by = BR_OY + BR_H - BR_PH - 8;
    }
    if (!b.started || b.over || b.ballOnPaddle) { brDraw(b, false); continue; }
    if (millis() - lastStep < 22) continue;
    lastStep = millis();
    b.bx += b.bvx; b.by += b.bvy;
    // walls
    if (b.bx < BR_OX + 3) { b.bx = BR_OX + 3; b.bvx = -b.bvx; }
    if (b.bx > BR_OX + BR_W - 3) { b.bx = BR_OX + BR_W - 3; b.bvx = -b.bvx; }
    if (b.by < BR_OY + 3) { b.by = BR_OY + 3; b.bvy = -b.bvy; }
    // paddle
    if (b.bvy > 0 && b.by > BR_OY + BR_H - BR_PH - 5 &&
        b.by < BR_OY + BR_H + 4 &&
        b.bx > b.paddleX - 3 && b.bx < b.paddleX + BR_PW + 3) {
      b.bvy = -b.bvy;
      // angle by hit position
      b.bvx = (b.bx - (b.paddleX + BR_PW / 2)) * 0.12f;
    }
    // bricks
    for (int r = 0; r < BR_ROWS; r++)
      for (int c = 0; c < BR_COLS; c++) {
        if (!b.bricks[r][c]) continue;
        int x0 = BR_OX + c * BR_BW, y0 = BR_OY + r * BR_BH;
        if (b.bx > x0 - 3 && b.bx < x0 + BR_BW + 3 &&
            b.by > y0 - 3 && b.by < y0 + BR_BH + 3) {
          b.bricks[r][c] = 0;
          gfx->fillRect(x0 + 1, y0 + 1, BR_BW - 2, BR_BH - 2, BLACK);
          b.score += 10;
          if (b.score > gsGetHi(GS_BREAKOUT)) gsRecordScore(GS_BREAKOUT, b.score);
          // bounce axis by depth
          if (b.by < y0 || b.by > y0 + BR_BH) b.bvy = -b.bvy;
          else b.bvx = -b.bvx;
          r = BR_ROWS; break;
        }
      }
    // count remaining
    int rem = 0;
    for (int r = 0; r < BR_ROWS; r++)
      for (int c = 0; c < BR_COLS; c++) rem += b.bricks[r][c];
    if (rem == 0) { b.over = true; gsRecordScore(GS_BREAKOUT, b.score); }
    // miss
    if (b.by > BR_OY + BR_H + 8) {
      b.lives--;
      if (b.lives <= 0) {
        b.over = true;
        gsRecordScore(GS_BREAKOUT, b.score);
      } else b.ballOnPaddle = true;
    }
    brDraw(b, false);
  }
}

// ============================ 2048 ============================
#define G48_N 4
#define G48_CELL 48
#define G48_OX 40
#define G48_OY 34

struct Game2048 {
  uint16_t v[G48_N][G48_N];
  int score;
  bool over, won, moved;
};

static void g48Reset(Game2048 &g) {
  memset(g.v, 0, sizeof(g.v));
  g.score = 0; g.over = false; g.won = false; g.moved = false;
  // two random tiles
  for (int k = 0; k < 2; k++) {
    int x, y;
    do { x = random(G48_N); y = random(G48_N); } while (g.v[y][x]);
    g.v[y][x] = 2;
  }
}

static uint16_t g48Color(uint16_t v) {
  switch (v) {
    case 0: return BLACK;
    case 2: return RGB565(0x18, 0x30, 0x18);
    case 4: return RGB565(0x1c, 0x3c, 0x1c);
    case 8: return RGB565(0x20, 0x50, 0x20);
    case 16: return RGB565(0x24, 0x64, 0x24);
    case 32: return RGB565(0x28, 0x78, 0x28);
    case 64: return RGB565(0x2c, 0x8c, 0x2c);
    case 128: return TERM_ACCENT;
    case 256: return TERM_GREEN;
    case 512: return TERM_BRIGHT;
    case 1024: return TERM_CYAN;
    default: return TERM_RED;
  }
}

static void g48Draw(Game2048 &g, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("2048");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("l/r/u/d slide click=n Long=back");
  }
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(240, 8);
  gfx->printf("%d    ", g.score);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(296, 8);
  gfx->printf("hi%d", gsGetHi(GS_2048));
  for (int y = 0; y < G48_N; y++)
    for (int x = 0; x < G48_N; x++) {
      int px = G48_OX + x * G48_CELL, py = G48_OY + y * G48_CELL;
      gfx->fillRect(px + 1, py + 1, G48_CELL - 2, G48_CELL - 2, g48Color(g.v[y][x]));
      if (g.v[y][x]) {
        gfx->setTextSize(g.v[y][x] >= 1024 ? 1 : 2);
        gfx->setTextColor(g.v[y][x] >= 128 ? BLACK : TERM_BRIGHT,
                          g48Color(g.v[y][x]));
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", g.v[y][x]);
        int tw = strlen(buf) * (g.v[y][x] >= 1024 ? 6 : 12);
        gfx->setCursor(px + (G48_CELL - tw) / 2, py + G48_CELL / 2 - 6);
        gfx->print(buf);
      }
    }
  if (g.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_RED, BLACK);
    gfx->setCursor(120, 240 - 26);
    gfx->print("OVER!");
  }
}

// slide one row left; returns true if anything moved
static bool g48SlideRow(uint16_t *row) {
  uint16_t tmp[G48_N];
  int n = 0;
  bool moved = false;
  for (int i = 0; i < G48_N; i++)
    if (row[i]) tmp[n++] = row[i];
  for (int i = 0; i < n; i++)
    if (i + 1 < n && tmp[i] == tmp[i + 1]) {
      tmp[i] *= 2;
      for (int j = i + 1; j < n - 1; j++) tmp[j] = tmp[j + 1];
      n--;
    }
  for (int i = 0; i < G48_N; i++) {
    uint16_t nv = (i < n) ? tmp[i] : 0;
    if (nv != row[i]) moved = true;
    row[i] = nv;
  }
  return moved;
}

static void g48AddTile(Game2048 &g) {
  int free[16], n = 0;
  for (int y = 0; y < G48_N; y++)
    for (int x = 0; x < G48_N; x++)
      if (!g.v[y][x]) free[n++] = y * G48_N + x;
  if (n == 0) return;
  int pick = free[random(n)];
  g.v[pick / G48_N][pick % G48_N] = (random(10) == 0) ? 4 : 2;
}

static void g48Move(Game2048 &g, int dir) {
  bool moved = false;
  uint16_t row[G48_N];
  if (dir == 0 || dir == 2) {  // left/right: rows
    for (int y = 0; y < G48_N; y++) {
      for (int x = 0; x < G48_N; x++) row[x] = g.v[y][x];
      bool m = g48SlideRow(row);
      if (dir == 2) {  // reverse for right
        for (int x = 0; x < G48_N / 2; x++) {
          uint16_t t = row[x]; row[x] = row[G48_N - 1 - x]; row[G48_N - 1 - x] = t;
        }
      }
      if (m) moved = true;
      for (int x = 0; x < G48_N; x++) {
        if (dir == 2) g.v[y][G48_N - 1 - x] = row[x];
        else g.v[y][x] = row[x];
      }
    }
  } else {  // up/down: columns
    for (int x = 0; x < G48_N; x++) {
      for (int y = 0; y < G48_N; y++) row[y] = g.v[y][x];
      bool m = g48SlideRow(row);
      if (dir == 3) {  // reverse for down
        for (int y = 0; y < G48_N / 2; y++) {
          uint16_t t = row[y]; row[y] = row[G48_N - 1 - y]; row[G48_N - 1 - y] = t;
        }
      }
      if (m) moved = true;
      for (int y = 0; y < G48_N; y++) {
        if (dir == 3) g.v[G48_N - 1 - y][x] = row[y];
        else g.v[y][x] = row[y];
      }
    }
  }
  if (moved) {
    g48AddTile(g);
    if (g.score > gsGetHi(GS_2048)) gsRecordScore(GS_2048, g.score);
    // over if no move possible
    bool any = false;
    for (int y = 0; y < G48_N && !any; y++)
      for (int x = 0; x < G48_N && !any; x++) {
        if (!g.v[y][x]) any = true;
        if (x + 1 < G48_N && g.v[y][x] == g.v[y][x + 1]) any = true;
        if (y + 1 < G48_N && g.v[y][x] == g.v[y + 1][x]) any = true;
      }
    if (!any) { g.over = true; gsRecordScore(GS_2048, g.score); }
  }
}

void game2048App() {
  Game2048 g;
  g48Reset(g);
  bool full = true;
  while (true) {
    if (full) { g48Draw(g, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_LEFT) g48Move(g, 0);
    else if (e.ev == PDA_EV_UP) g48Move(g, 1);
    else if (e.ev == PDA_EV_RIGHT) g48Move(g, 2);
    else if (e.ev == PDA_EV_DOWN) g48Move(g, 3);
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
             (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
      g48Reset(g);
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      gsRecordScore(GS_2048, g.score);
      return;
    }
    full = true;
  }
}

// ============================ Minesweeper ============================
#define MS_N 9
#define MS_CELL 20
#define MS_OX 34
#define MS_OY 30

struct Mines {
  uint8_t mine[MS_N][MS_N];      // 1 = mine
  uint8_t adj[MS_N][MS_N];       // adjacent mine count
  uint8_t state[MS_N][MS_N];    // bit0 revealed, bit1 flagged
  int cx, cy, revealed, flags;
  bool over, dead;
};

static void msPlace(Mines &m, int safeX, int safeY) {
  memset(m.mine, 0, sizeof(m.mine));
  memset(m.adj, 0, sizeof(m.adj));
  int placed = 0;
  while (placed < 10) {
    int x = random(MS_N), y = random(MS_N);
    if (m.mine[y][x]) continue;
    if (abs(x - safeX) <= 1 && abs(y - safeY) <= 1) continue;
    m.mine[y][x] = 1;
    placed++;
  }
  for (int y = 0; y < MS_N; y++)
    for (int x = 0; x < MS_N; x++) {
      m.adj[y][x] = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = x + dx, ny = y + dy;
          if (nx >= 0 && nx < MS_N && ny >= 0 && ny < MS_N && m.mine[ny][nx])
            m.adj[y][x]++;
        }
    }
}

static void msReset(Mines &m) {
  memset(m.state, 0, sizeof(m.state));
  m.cx = MS_N / 2; m.cy = MS_N / 2;
  m.revealed = 0; m.flags = 0;
  m.over = false; m.dead = false;
  memset(m.mine, 0, sizeof(m.mine));
  memset(m.adj, 0, sizeof(m.adj));
}

static void msDraw(Mines &m, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Mines");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("u/d/l/r f=flag click=open Long=back");
  }
  for (int y = 0; y < MS_N; y++)
    for (int x = 0; x < MS_N; x++) {
      int px = MS_OX + x * MS_CELL, py = MS_OY + y * MS_CELL;
      bool sel = (x == m.cx && y == m.cy);
      bool rev = m.state[y][x] & 1, flg = m.state[y][x] & 2;
      uint16_t bg = sel ? TERM_SEL_BG : BLACK;
      gfx->fillRect(px + 1, py + 1, MS_CELL - 2, MS_CELL - 2,
                    rev ? (flg ? TERM_ACCENT : RGB565(0x10, 0x20, 0x10))
                        : (sel ? TERM_SEL_BG : RGB565(0x18, 0x30, 0x18)));
      gfx->setTextSize(1);
      if (rev && !flg) {
        if (m.mine[y][x]) {
          gfx->setTextColor(TERM_RED, bg);
          gfx->setCursor(px + 7, py + 6);
          gfx->print("*");
        } else if (m.adj[y][x]) {
          gfx->setTextColor(m.adj[y][x] >= 3 ? TERM_ACCENT : TERM_BRIGHT, bg);
          gfx->setCursor(px + 7, py + 6);
          gfx->print((char)('0' + m.adj[y][x]));
        }
      } else if (flg) {
        gfx->setTextColor(BLACK, TERM_ACCENT);
        gfx->setCursor(px + 6, py + 6);
        gfx->print("F");
      }
      if (sel) gfx->drawRect(px, py, MS_CELL, MS_CELL, TERM_BRIGHT);
    }
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(230, 8);
  gfx->printf("mines 10  %d", m.flags);
  if (m.over) {
    gfx->setTextSize(2);
    gfx->setTextColor(m.dead ? TERM_RED : TERM_GREEN, BLACK);
    gfx->setCursor(MS_OX + 10, 220);
    gfx->print(m.dead ? "BOOM!" : "CLEAR!");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(MS_OX + 30, 232);
    gfx->print("click or n = play again");
  }
}

static void msFlood(Mines &m, int x, int y) {
  if (x < 0 || x >= MS_N || y < 0 || y >= MS_N) return;
  if (m.state[y][x] & 1) return;
  m.state[y][x] |= 1;
  m.revealed++;
  if (m.adj[y][x] == 0 && !m.mine[y][x])
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        if (dx || dy) msFlood(m, x + dx, y + dy);
}

void minesApp() {
  Mines m;
  msReset(m);
  bool full = true, firstClick = true;
  while (true) {
    if (full) { msDraw(m, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (m.over) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
          (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
        msReset(m); firstClick = true; full = true;
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    if (e.ev == PDA_EV_UP && m.cy > 0) { m.cy--; full = true; }
    else if (e.ev == PDA_EV_DOWN && m.cy < MS_N - 1) { m.cy++; full = true; }
    else if (e.ev == PDA_EV_LEFT && m.cx > 0) { m.cx--; full = true; }
    else if (e.ev == PDA_EV_RIGHT && m.cx < MS_N - 1) { m.cx++; full = true; }
    else if (e.ev == PDA_EV_CHAR && (e.ch == 'f' || e.ch == 'F')) {
      if (!(m.state[m.cy][m.cx] & 1)) {
        m.state[m.cy][m.cx] ^= 2;
        m.flags += (m.state[m.cy][m.cx] & 2) ? 1 : -1;
        full = true;
      }
    }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (m.state[m.cy][m.cx] & 2) continue;   // flagged, don't open
      if (firstClick) { msPlace(m, m.cx, m.cy); firstClick = false; }
      if (m.mine[m.cy][m.cx]) {
        m.dead = true; m.over = true;
        for (int y = 0; y < MS_N; y++)
          for (int x = 0; x < MS_N; x++)
            if (m.mine[y][x]) m.state[y][x] |= 1;
      } else {
        msFlood(m, m.cx, m.cy);
        if (m.revealed >= MS_N * MS_N - 10) m.over = true;
      }
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

// ============================ Pong ============================
#define PG_W 300
#define PG_H 190
#define PG_OX 10
#define PG_OY 30
#define PG_PH 42

struct Pong {
  float bx, by, bvx, bvy;
  int p1, p2;            // paddle Y (player bottom? no: player left)
  int s1, s2;
  bool started, over;
};

static void pgReset(Pong &p) {
  p.bx = PG_OX + PG_W / 2; p.by = PG_OY + PG_H / 2;
  p.bvx = 2.6f; p.bvy = 1.8f;
  p.p1 = PG_OY + PG_H / 2 - PG_PH / 2;
  p.p2 = PG_OY + PG_H / 2 - PG_PH / 2;
  p.s1 = 0; p.s2 = 0;
  p.started = false; p.over = false;
}

static void pgServe(Pong &p, bool toRight) {
  p.bx = PG_OX + PG_W / 2; p.by = PG_OY + PG_H / 2;
  p.bvx = toRight ? 2.6f : -2.6f;
  p.bvy = (random(2) ? 1 : -1) * (1.2f + random(10) * 0.12f);
}

static void pgDraw(Pong &p, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Pong");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->drawRect(PG_OX - 1, PG_OY - 1, PG_W + 2, PG_H + 2, TERM_DIM);
    for (int y = PG_OY; y < PG_OY + PG_H; y += 12)
      gfx->drawFastVLine(PG_OX + PG_W / 2, y, 6, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("u/d paddle click=serve Long=back");
  }
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(PG_OX + PG_W / 2 - 30, 6);
  gfx->printf("%d - %d", p.s1, p.s2);
  // erase + redraw paddles & ball
  static int l1 = -1, l2 = -1;
  static int lbx = -1, lby = -1;
  if (l1 >= 0) gfx->fillRect(PG_OX + 4, l1, 4, PG_PH, BLACK);
  if (l2 >= 0) gfx->fillRect(PG_OX + PG_W - 8, l2, 4, PG_PH, BLACK);
  if (lbx >= 0) gfx->fillCircle(lbx, lby, 3, BLACK);
  gfx->fillRect(PG_OX + 4, p.p1, 4, PG_PH, TERM_BRIGHT);
  gfx->fillRect(PG_OX + PG_W - 8, p.p2, 4, PG_PH, TERM_GREEN);
  gfx->fillCircle((int)p.bx, (int)p.by, 3, TERM_ACCENT);
  l1 = p.p1; l2 = p.p2; lbx = (int)p.bx; lby = (int)p.by;
  if (!p.started) {
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(PG_OX + PG_W / 2 - 40, PG_OY + PG_H + 6);
    gfx->print("click to serve");
  }
}

void pongApp() {
  Pong p;
  pgReset(p);
  bool full = true;
  unsigned long lastStep = 0;
  while (true) {
    if (full) { pgDraw(p, true); full = false; }
    InputEventP e;
    bool got = pdaGetInput(e, 16);
    if (got) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) p.started = true;
      else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      if (e.ev == PDA_EV_UP) p.p1 -= 16;
      else if (e.ev == PDA_EV_DOWN) p.p1 += 16;
      if (p.p1 < PG_OY) p.p1 = PG_OY;
      if (p.p1 > PG_OY + PG_H - PG_PH) p.p1 = PG_OY + PG_H - PG_PH;
    }
    if (!p.started) { pgDraw(p, false); continue; }
    if (millis() - lastStep < 20) continue;
    lastStep = millis();
    p.bx += p.bvx; p.by += p.bvy;
    if (p.by < PG_OY + 3) { p.by = PG_OY + 3; p.bvy = -p.bvy; }
    if (p.by > PG_OY + PG_H - 3) { p.by = PG_OY + PG_H - 3; p.bvy = -p.bvy; }
    // left paddle (player)
    if (p.bvx < 0 && p.bx < PG_OX + 12 && p.bx > PG_OX + 4 &&
        p.by > p.p1 - 3 && p.by < p.p1 + PG_PH + 3) {
      p.bvx = -p.bvx * 1.03f;
      p.bvy += (p.by - (p.p1 + PG_PH / 2)) * 0.04f;
    }
    // right paddle (AI)
    if (p.bvx > 0 && p.bx > PG_OX + PG_W - 12 && p.bx < PG_OX + PG_W - 4 &&
        p.by > p.p2 - 3 && p.by < p.p2 + PG_PH + 3) {
      p.bvx = -p.bvx * 1.02f;
      p.bvy += (p.by - (p.p2 + PG_PH / 2)) * 0.04f;
    }
    // AI move: track ball with a speed cap
    int aiTarget = (int)p.by - PG_PH / 2;
    if (p.bvx > 0) {
      int diff = aiTarget - p.p2;
      if (diff > 3) p.p2 += 3;
      else if (diff < -3) p.p2 -= 3;
    } else {
      int diff = PG_OY + PG_H / 2 - PG_PH / 2 - p.p2;
      if (diff > 2) p.p2 += 2;
      else if (diff < -2) p.p2 -= 2;
    }
    if (p.p2 < PG_OY) p.p2 = PG_OY;
    if (p.p2 > PG_OY + PG_H - PG_PH) p.p2 = PG_OY + PG_H - PG_PH;
    // scoring
    if (p.bx < PG_OX - 6) { p.s2++; pgServe(p, false); }
    if (p.bx > PG_OX + PG_W + 6) { p.s1++; pgServe(p, true); }
    if (p.s1 >= 7 || p.s2 >= 7) {
      p.over = true;
      gsRecordResult(GS_PONG, p.s1 > p.s2);
      pgDraw(p, false);
      gfx->setTextSize(2);
      gfx->setTextColor(p.s1 > p.s2 ? TERM_GREEN : TERM_RED, BLACK);
      gfx->setCursor(PG_OX + PG_W / 2 - 50, PG_OY + PG_H / 2 - 10);
      gfx->print(p.s1 > p.s2 ? "YOU WIN!" : "AI WINS!");
      while (true) {
        InputEventP e2;
        if (!pdaGetInput(e2, 50)) continue;
        if (e2.ev == PDA_EV_LONGSELECT || e2.ev == PDA_EV_BACK) return;
        if (e2.ev == PDA_EV_SELECT || e2.ev == PDA_EV_NEWLINE ||
            (e2.ev == PDA_EV_CHAR && (e2.ch == 'n' || e2.ch == 'N'))) {
          pgReset(p); full = true;
          break;
        }
      }
      continue;
    }
    pgDraw(p, false);
  }
}

// ============================ Reversi ============================
#define RV_N 8
#define RV_CELL 20
#define RV_OX 40
#define RV_OY 30

struct Reversi {
  uint8_t b[RV_N][RV_N];   // 0 empty, 1 player(black), 2 AI(white)
  int cx, cy;
  bool over;
  int passCount;
};

static void rvReset(Reversi &r) {
  memset(r.b, 0, sizeof(r.b));
  r.b[3][3] = 2; r.b[4][4] = 2;
  r.b[3][4] = 1; r.b[4][3] = 1;
  r.cx = 2; r.cy = 2;
  r.over = false;
  r.passCount = 0;
}

static const int rv_dirs[8][2] = {
  {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

// how many discs flip if `who` plays at x,y (0 if illegal)
static int rvFlips(Reversi &r, int x, int y, uint8_t who) {
  if (r.b[y][x]) return 0;
  uint8_t opp = 3 - who;
  int total = 0;
  for (int d = 0; d < 8; d++) {
    int nx = x + rv_dirs[d][0], ny = y + rv_dirs[d][1];
    int cnt = 0;
    while (nx >= 0 && nx < RV_N && ny >= 0 && ny < RV_N && r.b[ny][nx] == opp) {
      nx += rv_dirs[d][0]; ny += rv_dirs[d][1];
      cnt++;
    }
    if (cnt > 0 && nx >= 0 && nx < RV_N && ny >= 0 && ny < RV_N &&
        r.b[ny][nx] == who)
      total += cnt;
  }
  return total;
}

static void rvApply(Reversi &r, int x, int y, uint8_t who) {
  uint8_t opp = 3 - who;
  r.b[y][x] = who;
  for (int d = 0; d < 8; d++) {
    int nx = x + rv_dirs[d][0], ny = y + rv_dirs[d][1];
    int cnt = 0;
    while (nx >= 0 && nx < RV_N && ny >= 0 && ny < RV_N && r.b[ny][nx] == opp) {
      nx += rv_dirs[d][0]; ny += rv_dirs[d][1];
      cnt++;
    }
    if (cnt > 0 && nx >= 0 && nx < RV_N && ny >= 0 && ny < RV_N &&
        r.b[ny][nx] == who) {
      int fx = x + rv_dirs[d][0], fy = y + rv_dirs[d][1];
      while (fx != nx || fy != ny) {
        r.b[fy][fx] = who;
        fx += rv_dirs[d][0]; fy += rv_dirs[d][1];
      }
    }
  }
}

static bool rvHasMove(Reversi &r, uint8_t who) {
  for (int y = 0; y < RV_N; y++)
    for (int x = 0; x < RV_N; x++)
      if (rvFlips(r, x, y, who) > 0) return true;
  return false;
}

static void rvDraw(Reversi &r, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Reversi");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    for (int i = 0; i <= RV_N; i++) {
      gfx->drawFastVLine(RV_OX + i * RV_CELL, RV_OY, RV_N * RV_CELL, TERM_DIM);
      gfx->drawFastHLine(RV_OX, RV_OY + i * RV_CELL, RV_N * RV_CELL, TERM_DIM);
    }
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("u/d/l/r click=play Long=back");
  }
  int p1 = 0, p2 = 0;
  for (int y = 0; y < RV_N; y++)
    for (int x = 0; x < RV_N; x++) {
      int px = RV_OX + x * RV_CELL, py = RV_OY + y * RV_CELL;
      if (r.b[y][x]) {
        p1 += (r.b[y][x] == 1);
        p2 += (r.b[y][x] == 2);
        gfx->fillCircle(px + RV_CELL / 2, py + RV_CELL / 2, 7,
                        r.b[y][x] == 1 ? TERM_BRIGHT : TERM_GREEN);
      } else {
        gfx->fillRect(px + 1, py + 1, RV_CELL - 1, RV_CELL - 1, BLACK);
        if (rvFlips(r, x, y, 1) > 0) {
          gfx->drawCircle(px + RV_CELL / 2, py + RV_CELL / 2, 4, TERM_DIM);
        }
      }
    }
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(220, 8);
  gfx->printf("B%d W%d", p1, p2);
  // cursor
  int px = RV_OX + r.cx * RV_CELL, py = RV_OY + r.cy * RV_CELL;
  gfx->drawRect(px + 1, py + 1, RV_CELL - 1, RV_CELL - 1, TERM_ACCENT);
}

static void rvAIMove(Reversi &r) {
  int bestX = -1, bestY = -1, bestScore = -1;
  for (int y = 0; y < RV_N; y++)
    for (int x = 0; x < RV_N; x++) {
      int f = rvFlips(r, x, y, 2);
      // greedy with corner/edge bias
      int bias = 0;
      if ((x == 0 || x == RV_N - 1) && (y == 0 || y == RV_N - 1)) bias = 20;
      else if (x == 0 || x == RV_N - 1 || y == 0 || y == RV_N - 1) bias = 4;
      int sc = f + bias;
      if (f > 0 && sc > bestScore) { bestScore = sc; bestX = x; bestY = y; }
    }
  if (bestX >= 0) rvApply(r, bestX, bestY, 2);
}

void reversiApp() {
  Reversi r;
  rvReset(r);
  bool full = true;
  while (true) {
    if (full) { rvDraw(r, true); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (r.over) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
          (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
        rvReset(r); full = true;
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    if (e.ev == PDA_EV_UP && r.cy > 0) { r.cy--; full = true; }
    else if (e.ev == PDA_EV_DOWN && r.cy < RV_N - 1) { r.cy++; full = true; }
    else if (e.ev == PDA_EV_LEFT && r.cx > 0) { r.cx--; full = true; }
    else if (e.ev == PDA_EV_RIGHT && r.cx < RV_N - 1) { r.cx++; full = true; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (rvFlips(r, r.cx, r.cy, 1) > 0) {
        rvApply(r, r.cx, r.cy, 1);
        r.passCount = 0;
        if (rvHasMove(r, 2)) rvAIMove(r);
        else if (rvHasMove(r, 1)) { /* player moves again */ }
        else r.over = true;
        full = true;
        if (r.over) {
          int p1 = 0, p2 = 0;
          for (int y = 0; y < RV_N; y++)
            for (int x = 0; x < RV_N; x++) {
              p1 += (r.b[y][x] == 1); p2 += (r.b[y][x] == 2);
            }
          gsRecordResult(GS_REVERSI, p1 > p2);
        }
      }
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}
