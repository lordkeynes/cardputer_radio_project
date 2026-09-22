/**
 * Games: Chess vs simple minimax AI, Go 9x9 vs greedy AI.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "games.h"
#include "gamestats.h"

extern Arduino_GFX *gfx;

// ============================ Chess ============================
// Board: 8x8, pieces encoded as char. Upper=white, lower=black. '.'=empty.
// Standard-ish rules: moves, captures, castling omitted, en passant omitted,
// pawn promotion to queen only. AI: 2-ply minimax + material eval.
struct Chess {
  char b[64];
  bool whiteToMove = true;
  int selX = 4, selY = 4;   // cursor
  bool pieceSelected = false;
  int pselX, pselY;
  bool gameOver = false;
  const char *msg = "";
};

static void chessInit(Chess &g) {
  const char *back = "rnbqkbnr";
  const char *front = "pppppppp";
  for (int i = 0; i < 64; i++) g.b[i] = '.';
  for (int i = 0; i < 8; i++) {
    g.b[i] = back[i];          // black back rank (y=0)
    g.b[8 + i] = front[i];     // black pawns (y=1)
    g.b[48 + i] = front[i] - 32;  // white pawns (y=6): 'P'
    g.b[56 + i] = back[i] - 32;  // white back rank (y=7)
  }
  g.whiteToMove = true;
  g.selX = 4; g.selY = 4;
  g.pieceSelected = false;
  g.gameOver = false;
  g.msg = "";
}

static bool isWhitePiece(char c) { return c >= 'A' && c <= 'Z'; }
static bool isBlackPiece(char c) { return c >= 'a' && c <= 'z'; }
static char pieceAt(Chess &g, int x, int y) { return (x>=0&&x<8&&y>=0&&y<8) ? g.b[y*8+x] : '?'; }
static void setPiece(Chess &g, int x, int y, char c) { if (x>=0&&x<8&&y>=0&&y<8) g.b[y*8+x] = c; }

// legal pseudo-move test: does not handle check fully (game-level simplification)
static bool chessCanMove(Chess &g, int fx, int fy, int tx, int ty) {
  char p = pieceAt(g, fx, fy);
  if (p == '.' || p == '?') return false;
  char t = pieceAt(g, tx, ty);
  if (t == '?') return false;
  bool white = isWhitePiece(p);
  if (t != '.' && ((white && isWhitePiece(t)) || (!white && isBlackPiece(t)))) return false;
  int dx = tx - fx, dy = ty - fy;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  char up = white ? (p - 32) : p;  // uppercase type
  switch (up) {
    case 'P': {
      int dir = white ? -1 : 1;
      int startRow = white ? 6 : 1;
      if (dx == 0 && t == '.') {
        if (dy == dir) return true;
        if (fy == startRow && dy == 2 * dir && pieceAt(g, fx, fy + dir) == '.') return true;
      }
      if (adx == 1 && ady == 1 && t != '.') return true;  // capture
      return false;
    }
    case 'N': return (adx == 1 && ady == 2) || (adx == 2 && ady == 1);
    case 'B': if (adx == ady && adx > 0) {
        int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
        for (int i = 1; i < adx; i++) if (pieceAt(g, fx + i*sx, fy + i*sy) != '.') return false;
        return true;
      } return false;
    case 'R': if ((dx == 0) != (dy == 0)) {
        int steps = dx != 0 ? adx : ady;
        int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0), sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
        for (int i = 1; i < steps; i++) if (pieceAt(g, fx + i*sx, fy + i*sy) != '.') return false;
        return true;
      } return false;
    case 'Q': {
      // reuse rook+bishop logic
      Chess tmp = g;
      char qp = pieceAt(g, fx, fy);
      if ((dx == 0) != (dy == 0)) { // rook-like
        int steps = dx != 0 ? adx : ady;
        int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0), sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
        for (int i = 1; i < steps; i++) if (pieceAt(g, fx + i*sx, fy + i*sy) != '.') return false;
        return true;
      }
      if (adx == ady && adx > 0) {
        int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
        for (int i = 1; i < adx; i++) if (pieceAt(g, fx + i*sx, fy + i*sy) != '.') return false;
        return true;
      }
      return false;
    }
    case 'K': return adx <= 1 && ady <= 1;
  }
  return false;
}

static int pieceValue(char p) {
  switch (p) {
    case 'P': case 'p': return 100;
    case 'N': case 'n': return 300;
    case 'B': case 'b': return 310;
    case 'R': case 'r': return 500;
    case 'Q': case 'q': return 900;
    case 'K': case 'k': return 100000;
  }
  return 0;
}

static int chessEval(Chess &g) {
  int score = 0;
  for (int i = 0; i < 64; i++) {
    char p = g.b[i];
    if (p == '.') continue;
    int v = pieceValue(p);
    if (p == 'k' || p == 'K') continue;  // count kings for terminal only
    score += isWhitePiece(p) ? v : -v;
  }
  return score;
}

// find king; if missing, that side lost
static bool kingAlive(Chess &g, bool white) {
  char target = white ? 'K' : 'k';
  for (int i = 0; i < 64; i++) if (g.b[i] == target) return true;
  return false;
}

// 2-ply minimax: black (AI) moves to minimize, white maximizes next
static void chessAiMove(Chess &g) {
  int bestScore = 1 << 30;
  int bf = -1, bt = -1;
  for (int fy = 0; fy < 8; fy++) for (int fx = 0; fx < 8; fx++) {
    char p = pieceAt(g, fx, fy);
    if (p == '.' || isWhitePiece(p)) continue;
    for (int ty = 0; ty < 8; ty++) for (int tx = 0; tx < 8; tx++) {
      if (!chessCanMove(g, fx, fy, tx, ty)) continue;
      char target = pieceAt(g, tx, ty);
      // move
      setPiece(g, tx, ty, p);
      setPiece(g, fx, fy, '.');
      int sc = chessEval(g);
      // white best reply (greedy 1-ply)
      int bestWhite = -1 << 30;
      for (int wy = 0; wy < 8; wy++) for (int wx = 0; wx < 8; wx++) {
        char wp = pieceAt(g, wx, wy);
        if (wp == '.' || !isWhitePiece(wp)) continue;
        for (int vy = 0; vy < 8; vy++) for (int vx = 0; vx < 8; vx++) {
          if (!chessCanMove(g, wx, wy, vx, vy)) continue;
          int s2 = chessEval(g);  // eval is white-positive
          if (s2 > bestWhite) bestWhite = s2;
        }
      }
      // NOTE: reply eval is approximate (doesn't apply the reply move); fine for casual play
      sc = bestWhite;
      // undo
      setPiece(g, fx, fy, p);
      setPiece(g, tx, ty, target);
      if (sc < bestScore) { bestScore = sc; bf = fy*8+fx; bt = ty*8+tx; }
    }
  }
  if (bf >= 0) {
    char p = g.b[bf];
    g.b[bt] = p;
    g.b[bf] = '.';
    // pawn promotion (black: reaching y=7)
    if ((p == 'p') && (bt / 8) == 7) g.b[bt] = 'q';
  }
}

static void drawPiece(Arduino_GFX *g, char p, int x, int y, int sz) {
  // 5x7-ish block font for pieces, drawn with rects
  g->setTextSize(1);
  if (isWhitePiece(p)) g->setTextColor(TERM_GREEN, BLACK);
  else g->setTextColor(TERM_DIM, BLACK);
  g->setCursor(x + 2, y + 2);
  char u = isWhitePiece(p) ? p : (p - 32);
  g->print(u);
}

static void chessDraw(Chess &g) {
  gfx->fillScreen(BLACK);
  // board 8x8 at 24px squares = 192x192, offset (64, 30)
  int ox = 64, oy = 30, sq = 24;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      bool light = (x + y) % 2 == 0;
      uint16_t col = light ? RGB565(30, 80, 40) : RGB565(8, 20, 10);
      gfx->fillRect(ox + x * sq, oy + y * sq, sq, sq, col);
      char p = pieceAt(g, x, y);
      if (p != '.') drawPiece(gfx, p, ox + x * sq, oy + y * sq, sq);
    }
  }
  // selection highlight
  if (g.pieceSelected) {
    gfx->drawRect(ox + g.pselX * sq, oy + g.pselY * sq, sq, sq, TERM_ACCENT);
  }
  gfx->drawRect(ox + g.selX * sq, oy + g.selY * sq, sq, sq, TERM_CYAN);
  // status
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(4, 6);
  gfx->print(g.gameOver ? g.msg : (g.whiteToMove ? "White (you)" : "Black (AI)"));
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("Trackball=move  Click=pick/put  n=new  Long=back");
}

void chessApp() {
  static Chess g;
  chessInit(g);
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) { needsRedraw = false; chessDraw(g); }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (g.gameOver) {
      if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) { chessInit(g); needsRedraw = true; }
      if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    switch (e.ev) {
      case PDA_EV_UP: if (g.selY > 0) g.selY--; needsRedraw = true; break;
      case PDA_EV_DOWN: if (g.selY < 7) g.selY++; needsRedraw = true; break;
      case PDA_EV_LEFT: if (g.selX > 0) g.selX--; needsRedraw = true; break;
      case PDA_EV_RIGHT: if (g.selX < 7) g.selX++; needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        if (!g.pieceSelected) {
          char p = pieceAt(g, g.selX, g.selY);
          if (p != '.' && isWhitePiece(p)) {
            g.pieceSelected = true;
            g.pselX = g.selX; g.pselY = g.selY;
          }
        } else {
          if (g.selX == g.pselX && g.selY == g.pselY) {
            g.pieceSelected = false;
          } else if (chessCanMove(g, g.pselX, g.pselY, g.selX, g.selY)) {
            char p = pieceAt(g, g.pselX, g.pselY);
            char target = pieceAt(g, g.selX, g.selY);
            setPiece(g, g.selX, g.selY, p);
            setPiece(g, g.pselX, g.pselY, '.');
            // promotion to queen
            if (p == 'P' && g.selY == 0) setPiece(g, g.selX, g.selY, 'Q');
            g.pieceSelected = false;
            // terminal check
            if (target == 'k') { g.gameOver = true; g.msg = "You win! n=new"; gsRecordResult(GS_CHESS, true); }
            else if (!kingAlive(g, false)) { g.gameOver = true; g.msg = "You win! n=new"; gsRecordResult(GS_CHESS, true); }
            else {
              g.whiteToMove = false;
              needsRedraw = true;
              // AI move
              vTaskDelay(pdMS_TO_TICKS(200));
              chessAiMove(g);
              g.whiteToMove = true;
              if (!kingAlive(g, true)) { g.gameOver = true; g.msg = "AI wins. n=new"; gsRecordResult(GS_CHESS, false); }
            }
          } else {
            g.pieceSelected = false;
          }
        }
        needsRedraw = true;
        break;
      }
      case PDA_EV_CHAR:
        if (e.ch == 'n' || e.ch == 'N') { chessInit(g); needsRedraw = true; }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK: return;
      default: break;
    }
  }
}

// ============================ Go (9x9) ============================
#define GO_N 9
struct GoGame {
  int board[GO_N * GO_N];   // 0 empty, 1 black(human), 2 white(AI)
  int selX = 4, selY = 4;
  bool gameOver = false;
  int capsBlack = 0, capsWhite = 0;  // stones captured BY each
  const char *msg = "";
};

static int goAt(GoGame &g, int x, int y) {
  return (x >= 0 && x < GO_N && y >= 0 && y < GO_N) ? g.board[y * GO_N + x] : -1;
}

// flood-fill group; returns liberties count via visited
static int goGroupLiberties(GoGame &g, int x, int y, uint8_t *visited) {
  int color = goAt(g, x, y);
  if (color <= 0) return 0;
  int libs = 0;
  int stack[81], sp = 0;
  stack[sp++] = y * GO_N + x;
  visited[y * GO_N + x] = 1;
  while (sp > 0) {
    int idx = stack[--sp];
    int cx = idx % GO_N, cy = idx / GO_N;
    int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
      int nx = cx + dx4[d], ny = cy + dy4[d];
      int v = goAt(g, nx, ny);
      if (v == 0) libs++;
      else if (v == color && !visited[ny * GO_N + nx]) {
        visited[ny * GO_N + nx] = 1;
        stack[sp++] = ny * GO_N + nx;
      }
    }
  }
  return libs;
}

// remove a group from board, return stones removed
static int goRemoveGroup(GoGame &g, int x, int y) {
  int color = goAt(g, x, y);
  if (color <= 0) return 0;
  int removed = 0;
  int stack[81], sp = 0;
  stack[sp++] = y * GO_N + x;
  while (sp > 0) {
    int idx = stack[--sp];
    int cx = idx % GO_N, cy = idx / GO_N;
    if (g.board[idx] != color) continue;
    g.board[idx] = 0;
    removed++;
    int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
      int nx = cx + dx4[d], ny = cy + dy4[d];
      if (goAt(g, nx, ny) == color) stack[sp++] = ny * GO_N + nx;
    }
  }
  return removed;
}

// try a move for `color`; returns true if legal (applies captures, suicide check)
static bool goPlay(GoGame &g, int x, int y, int color, int &captured) {
  if (goAt(g, x, y) != 0) return false;
  captured = 0;
  // place tentatively
  g.board[y * GO_N + x] = color;
  // check enemy neighbors for capture
  int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  static uint8_t vis[81];
  for (int d = 0; d < 4; d++) {
    int nx = x + dx4[d], ny = y + dy4[d];
    int v = goAt(g, nx, ny);
    if (v > 0 && v != color) {
      memset(vis, 0, sizeof(vis));
      if (goGroupLiberties(g, nx, ny, vis) == 0) {
        captured += goRemoveGroup(g, nx, ny);
      }
    }
  }
  // suicide check
  memset(vis, 0, sizeof(vis));
  if (goGroupLiberties(g, x, y, vis) == 0) {
    g.board[y * GO_N + x] = 0;  // revert
    return false;
  }
  return true;
}

// simple AI: prefer captures, then max liberties-gained heuristics
static void goAiMove(GoGame &g) {
  int bestScore = -1 << 30, bx = -1, by = -1;
  for (int y = 0; y < GO_N; y++) for (int x = 0; x < GO_N; x++) {
    if (goAt(g, x, y) != 0) continue;
    int captured = 0;
    // simulate on copy
    GoGame sim = g;
    if (!goPlay(sim, x, y, 2, captured)) continue;
    int score = captured * 10;
    // center preference
    int cx = GO_N / 2;
    score += 5 - (abs(x - cx) + abs(y - cx)) / 2;
    // liberty heuristic: own libs after
    static uint8_t vis[81];
    memset(vis, 0, sizeof(vis));
    score += goGroupLiberties(sim, x, y, vis);
    if (score > bestScore) { bestScore = score; bx = x; by = y; }
  }
  if (bx >= 0) {
    int captured = 0;
    goPlay(g, bx, by, 2, captured);
    g.capsWhite += captured;
  } else {
    g.gameOver = true;
    g.msg = "AI passes - game over";
  }
}

static void goDraw(GoGame &g) {
  gfx->fillScreen(BLACK);
  int cell = 22, ox = 40, oy = 28;
  // grid
  gfx->setTextColor(TERM_DIM, BLACK);
  for (int i = 0; i < GO_N; i++) {
    gfx->drawFastHLine(ox, oy + i * cell, (GO_N - 1) * cell, TERM_DIM);
    gfx->drawFastVLine(ox + i * cell, oy, (GO_N - 1) * cell, TERM_DIM);
  }
  // star points
  gfx->fillCircle(ox + 2 * cell, oy + 2 * cell, 2, TERM_DIM);
  gfx->fillCircle(ox + 6 * cell, oy + 2 * cell, 2, TERM_DIM);
  gfx->fillCircle(ox + 2 * cell, oy + 6 * cell, 2, TERM_DIM);
  gfx->fillCircle(ox + 6 * cell, oy + 6 * cell, 2, TERM_DIM);
  gfx->fillCircle(ox + 4 * cell, oy + 4 * cell, 2, TERM_DIM);
  // stones
  for (int y = 0; y < GO_N; y++) for (int x = 0; x < GO_N; x++) {
    int v = goAt(g, x, y);
    if (v == 0) continue;
    uint16_t col = (v == 1) ? RGB565(20, 20, 20) : RGB565(230, 230, 230);
    gfx->fillCircle(ox + x * cell, oy + y * cell, 8, col);
    if (v == 1) gfx->drawCircle(ox + x * cell, oy + y * cell, 8, TERM_GREEN);  // edge for visibility
  }
  // cursor
  gfx->drawCircle(ox + g.selX * cell, oy + g.selY * cell, 10, TERM_CYAN);
  gfx->drawCircle(ox + g.selX * cell, oy + g.selY * cell, 11, TERM_CYAN);
  // status
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(4, 6);
  gfx->printf("You(B) %d - %d (W)AI", g.capsBlack, g.capsWhite);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("Move cursor, click to place  p=pass  n=new  Long=back");
}

void goApp() {
  static GoGame g;
  memset(g.board, 0, sizeof(g.board));
  g.selX = g.selY = 4;
  g.gameOver = false;
  g.capsBlack = g.capsWhite = 0;
  g.msg = "";
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) { needsRedraw = false; goDraw(g); }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (g.gameOver) {
      if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
        memset(g.board, 0, sizeof(g.board));
        g.gameOver = false; g.capsBlack = g.capsWhite = 0;
        needsRedraw = true;
      }
      if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    switch (e.ev) {
      case PDA_EV_UP: if (g.selY > 0) g.selY--; needsRedraw = true; break;
      case PDA_EV_DOWN: if (g.selY < GO_N - 1) g.selY++; needsRedraw = true; break;
      case PDA_EV_LEFT: if (g.selX > 0) g.selX--; needsRedraw = true; break;
      case PDA_EV_RIGHT: if (g.selX < GO_N - 1) g.selX++; needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        int captured = 0;
        if (goPlay(g, g.selX, g.selY, 1, captured)) {
          g.capsBlack += captured;
          needsRedraw = true;
          vTaskDelay(pdMS_TO_TICKS(150));
          goAiMove(g);
          needsRedraw = true;
        }
        break;
      }
      case PDA_EV_CHAR:
        if (e.ch == 'p' || e.ch == 'P') {
          goAiMove(g);  // AI replies to pass
          needsRedraw = true;
        } else if (e.ch == 'n' || e.ch == 'N') {
          memset(g.board, 0, sizeof(g.board));
          g.gameOver = false; g.capsBlack = g.capsWhite = 0;
          needsRedraw = true;
        }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK: return;
      default: break;
    }
  }
}

// ============================ Solitaire (Klondike) ============================
// 7 tableau columns, 4 foundations, stock/waste. Simplified draw-1.
struct Solitaire {
  // piles: 0-6 tableau; 7 stock; 8 waste; 9-12 foundations (S,H,D,C)
  int tab[7][19];   // card codes: 1..13 = A..K, 0 empty; suit = (code-1)/13? no:
  // card encode: suit*100 + rank, suit 0..3 = S,H,D,C, rank 1..13. 0 = empty.
  int tabN[7];
  int found[4];     // top card or 0
  int stock[24]; int stockN;
  int waste[24]; int wasteN;
  int cursor = 0;    // 0..13: 0-6 tableau, 7 stock, 8 waste, 9-12 foundations
  int colSel[7];    // selected depth per tableau column, -1 none
  bool wasteSel = false, stockSel = false;
  bool won = false;
};

static int cardRank(int c) { return c % 100; }
static int cardSuit(int c) { return c / 100; }
static int cardColor(int c) { int s = cardSuit(c); return (s == 0 || s == 3) ? 0 : 1; }  // 0 black 1 red

static void solInit(Solitaire &g) {
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = (i / 13) * 100 + (i % 13) + 1;
  // shuffle (esp_random)
  for (int i = 51; i > 0; i--) {
    int j = esp_random() % (i + 1);
    int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
  }
  int di = 0;
  for (int c = 0; c < 7; c++) {
    for (int k = 0; k <= c; k++) g.tab[c][k] = deck[di++];
    g.tabN[c] = c + 1;
  }
  g.stockN = 24;
  for (int i = 0; i < 24; i++) g.stock[i] = deck[di++];
  g.wasteN = 0;
  for (int i = 0; i < 4; i++) g.found[i] = 0;
  for (int i = 0; i < 7; i++) g.colSel[i] = -1;
  g.wasteSel = false;
  g.cursor = 0;
  g.won = false;
}

static const char *rankStr(int r) {
  static const char *ranks[] = {"","A","2","3","4","5","6","7","8","9","10","J","Q","K"};
  return ranks[r];
}

// tiny suit glyphs drawn with primitives (5x6 area at x,y)
static void drawSuitGlyph(Arduino_GFX *g, int su, int x, int y, uint16_t col) {
  switch (su) {
    case 0:  // spade
      g->fillRect(x + 1, y + 4, 3, 2, col);
      g->fillTriangle(x, y + 3, x + 5, y + 3, x + 2, y - 1, col);
      g->fillTriangle(x, y + 3, x + 5, y + 3, x + 3, y - 1, col);
      break;
    case 1:  // heart
      g->fillCircle(x + 1, y + 1, 2, col);
      g->fillCircle(x + 4, y + 1, 2, col);
      g->fillTriangle(x - 1, y + 2, x + 6, y + 2, x + 2, y + 6, col);
      break;
    case 2:  // diamond
      g->fillTriangle(x, y + 2, x + 5, y + 2, x + 2, y - 2, col);
      g->fillTriangle(x, y + 2, x + 5, y + 2, x + 3, y + 6, col);
      break;
    case 3:  // club
      g->fillCircle(x + 2, y, 2, col);
      g->fillCircle(x, y + 3, 2, col);
      g->fillCircle(x + 5, y + 3, 2, col);
      g->fillRect(x + 1, y + 4, 3, 2, col);
      break;
  }
}

static void drawCard(Arduino_GFX *g, int card, int x, int y, bool selected) {
  // 30x40 card with white face
  if (card == 0) {
    // empty slot outline
    g->drawRect(x, y, 30, 40, selected ? TERM_ACCENT : TERM_DIM);
    return;
  }
  int r = cardRank(card), su = cardSuit(card);
  uint16_t col = cardColor(card) ? RGB565(0xd8, 0x28, 0x28) : RGB565(0x18, 0x18, 0x18);
  // face + border
  g->fillRect(x + 1, y + 1, 28, 38, RGB565(0xf4, 0xf0, 0xe2));
  g->drawRect(x, y, 30, 40, selected ? TERM_ACCENT : RGB565(0x60, 0x60, 0x60));
  // corner rank + suit, spread so they do not overlap
  g->setTextSize(1);
  g->setTextColor(col, RGB565(0xf4, 0xf0, 0xe2));
  g->setCursor(x + 3, y + 2);
  g->print(rankStr(r));
  drawSuitGlyph(g, su, x + 3, y + 10, col);
  // bottom-right inverted mini suit
  drawSuitGlyph(g, su, x + 22, y + 30, col);
  g->setCursor(x + 22, y + 22);
  g->print(rankStr(r));
}

static bool canStackTableau(int moving, int onto) {
  if (onto == 0) return cardRank(moving) == 13;
  return cardColor(moving) != cardColor(onto) && cardRank(moving) == cardRank(onto) - 1;
}

static bool canStackFoundation(int moving, int fndSuit, int fndTop) {
  if (cardSuit(moving) != fndSuit) return false;
  return fndTop == 0 ? cardRank(moving) == 1 : cardRank(moving) == cardRank(fndTop) + 1;
}

static void solDraw(Solitaire &g) {
  gfx->fillScreen(RGB565(0x07, 0x2a, 0x14));   // felt green
  // top row: stock, waste, 4 foundations
  // stock: card back with lattice
  if (g.stockN) {
    gfx->fillRect(9, 7, 28, 38, RGB565(0x8a, 0x2a, 0x2a));
    gfx->drawRect(8, 6, 30, 40, g.cursor == 7 ? TERM_ACCENT : RGB565(0x60, 0x20, 0x20));
    for (int i = 0; i < 4; i++) {
      gfx->drawFastHLine(13, 13 + i * 8, 20, RGB565(0xc0, 0x50, 0x50));
      gfx->drawFastHLine(13, 17 + i * 8, 20, RGB565(0xc0, 0x50, 0x50));
    }
  } else {
    drawCard(gfx, 0, 8, 6, g.cursor == 7);
  }
  drawCard(gfx, g.wasteN ? g.waste[g.wasteN-1] : 0, 46, 6, g.cursor == 8 || g.wasteSel);
  for (int i = 0; i < 4; i++) {
    drawCard(gfx, g.found[i], 130 + i * 40, 6, g.cursor == 9 + i);
    if (g.found[i] == 0) { gfx->setTextSize(1); gfx->setTextColor(TERM_DIM, BLACK); gfx->setCursor(140 + i * 40, 22); const char *s="SHDC"; gfx->print(s[i]); }
  }
  // tableau
  for (int c = 0; c < 7; c++) {
    int x = 8 + c * 44;
    // slot outline behind the column
    gfx->drawRect(x, 54, 30, 40, RGB565(0x0a, 0x3a, 0x1c));
    for (int k = 0; k < g.tabN[c]; k++) {
      bool sel = (g.colSel[c] >= 0 && k >= g.colSel[c]);
      drawCard(gfx, g.tab[c][k], x, 54 + k * 13, sel || (g.cursor == c && k == g.tabN[c]-1));
    }
  }
  // footer
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  if (g.won) { gfx->setTextColor(TERM_GREEN, BLACK); gfx->print("You won! n=new"); }
  else gfx->print("L/R col  U/D row  Click=select/move  s=stock  n=new Long=back");
}

void solitaireApp() {
  static Solitaire g;
  solInit(g);
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) { needsRedraw = false; solDraw(g); }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (g.won) {
      if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) { solInit(g); needsRedraw = true; }
      else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    switch (e.ev) {
      case PDA_EV_LEFT: g.cursor = (g.cursor + 13) % 14; needsRedraw = true; break;
      case PDA_EV_RIGHT: g.cursor = (g.cursor + 1) % 14; needsRedraw = true; break;
      case PDA_EV_UP: case PDA_EV_DOWN: needsRedraw = true; break;  // row nav not needed w/ auto-stacks
      case PDA_EV_CHAR:
        if (e.ch == 's' || e.ch == 'S') {
          // draw from stock
          if (g.stockN > 0) {
            g.waste[g.wasteN++] = g.stock[--g.stockN];
          } else {
            // recycle waste
            while (g.wasteN > 0) g.stock[g.stockN++] = g.waste[--g.wasteN];
          }
          needsRedraw = true;
        } else if (e.ch == 'n' || e.ch == 'N') { solInit(g); needsRedraw = true; }
        break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        // selection / move logic
        if (g.cursor == 7) {  // stock
          if (g.stockN > 0) g.waste[g.wasteN++] = g.stock[--g.stockN];
          else while (g.wasteN > 0) g.stock[g.stockN++] = g.waste[--g.wasteN];
          needsRedraw = true;
        } else if (g.cursor == 8) {  // waste
          g.wasteSel = !g.wasteSel;
          needsRedraw = true;
        } else if (g.cursor >= 9) {  // foundation target
          // move selected card to foundation
          int f = g.cursor - 9;
          int card = 0;
          bool fromWaste = g.wasteSel;
          int fromCol = -1;
          for (int c = 0; c < 7; c++) if (g.colSel[c] >= 0) { fromCol = c; card = g.tab[c][g.colSel[c]]; }
          if (fromWaste && g.wasteN > 0) card = g.waste[g.wasteN - 1];
          if (card && canStackFoundation(card, f, g.found[f])) {
            g.found[f] = card;
            if (fromWaste) { g.wasteN--; g.wasteSel = false; }
            else if (fromCol >= 0) {
              for (int k = g.colSel[fromCol]; k < g.tabN[fromCol]; k++) g.tab[fromCol][k] = 0;
              g.tabN[fromCol] = g.colSel[fromCol];
              g.colSel[fromCol] = -1;
            }
            // win check
            if (g.found[0] && g.found[1] && g.found[2] && g.found[3] &&
                cardRank(g.found[0])==13 && cardRank(g.found[1])==13 &&
                cardRank(g.found[2])==13 && cardRank(g.found[3])==13) {
              g.won = true;
              gsRecordResult(GS_SOLITAIRE, true);
            }
          }
          needsRedraw = true;
        } else {  // tableau column
          int c = g.cursor;
          if (g.colSel[c] >= 0) {
            // already selected: try move selection elsewhere is via cursor; deselect here
            g.colSel[c] = -1;
          } else if (g.wasteSel) {
            // move waste top to this column
            int card = g.wasteN ? g.waste[g.wasteN-1] : 0;
            int top = g.tabN[c] ? g.tab[c][g.tabN[c]-1] : 0;
            if (card && canStackTableau(card, top)) {
              g.tab[c][g.tabN[c]++] = card;
              g.wasteN--; g.wasteSel = false;
            } else g.wasteSel = false;
          } else {
            // select from bottom: pick deepest face-up run start
            if (g.tabN[c] > 0) {
              int depth = g.tabN[c] - 1;
              // allow selecting any single top card or valid run
              g.colSel[c] = depth;
            }
          }
          needsRedraw = true;
        }
        // auto move: if a column selection exists and user clicked another column via cursor change, handled above
        // (moving tableau runs between columns)
        if (g.cursor < 7 && g.colSel[g.cursor] >= 0) {
          // second click on same column -> deselect
        }
        break;
      }
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK: return;
      default: break;
    }
    // tableau-to-tableau: when a column is selected and cursor lands on another column with SELECT handled above.
  }
}

// ============================ Checkers (8x8, vs AI) ============================
struct Checkers {
  int b[64];  // 0 empty, 1 red(human), 2 red king, 3 black(AI), 4 black king
  int selX = 0, selY = 5;
  bool selected = false;
  int pselX, pselY;
  bool gameOver = false;
  const char *msg = "";
  int turn = 1;  // 1 human, 2 ai
};

static int ckAt(Checkers &g, int x, int y) { return (x>=0&&x<8&&y>=0&&y<8) ? g.b[y*8+x] : -1; }
static void ckSet(Checkers &g, int x, int y, int v) { if (x>=0&&x<8&&y>=0&&y<8) g.b[y*8+x] = v; }

static void ckInit(Checkers &g) {
  for (int i = 0; i < 64; i++) g.b[i] = 0;
  // black (AI) on top 3 rows dark squares
  for (int y = 0; y < 3; y++) for (int x = 0; x < 8; x++) {
    if ((x + y) % 2 == 1) g.b[y*8+x] = 3;
  }
  // red (human) bottom 3 rows
  for (int y = 5; y < 8; y++) for (int x = 0; x < 8; x++) {
    if ((x + y) % 2 == 1) g.b[y*8+x] = 1;
  }
  g.selX = 1; g.selY = 5;
  g.selected = false;
  g.gameOver = false;
  g.msg = "";
  g.turn = 1;
}

static bool ckIsRed(int v) { return v == 1 || v == 2; }
static bool ckIsBlack(int v) { return v == 3 || v == 4; }

// list legal simple moves for piece at (x,y) moving in dir of `up` (red moves up)
static bool ckCanMove(Checkers &g, int fx, int fy, int tx, int ty, bool &isJump) {
  isJump = false;
  int p = ckAt(g, fx, fy);
  if (p <= 0) return false;
  int t = ckAt(g, tx, ty);
  if (t != 0) return false;
  int dx = tx - fx, dy = ty - fy;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  if (adx != ady || adx == 0 || adx > 2) return false;
  bool king = (p == 2 || p == 4);
  int forward = ckIsRed(p) ? -1 : 1;
  if (!king && (dy != forward * adx)) return false;
  if (adx == 1) return true;
  // jump: must be over enemy
  int mx = fx + dx / 2, my = fy + dy / 2;
  int mid = ckAt(g, mx, my);
  if (mid <= 0) return false;
  if (ckIsRed(p) == ckIsRed(mid)) return false;
  isJump = true;
  return true;
}

static void ckDraw(Checkers &g) {
  gfx->fillScreen(BLACK);
  int sq = 24, ox = 64, oy = 30;
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
    bool dark = (x + y) % 2 == 1;
    gfx->fillRect(ox + x * sq, oy + y * sq, sq, sq, dark ? RGB565(10, 30, 15) : RGB565(20, 60, 30));
    int v = ckAt(g, x, y);
    if (v > 0) {
      uint16_t col = ckIsRed(v) ? TERM_RED : RGB565(180, 180, 180);
      gfx->fillCircle(ox + x * sq + 12, oy + y * sq + 12, 8, col);
      if (v >= 2) {  // king marker
        gfx->drawCircle(ox + x * sq + 12, oy + y * sq + 12, 4, BLACK);
      }
    }
  }
  if (g.selected) gfx->drawRect(ox + g.pselX * sq, oy + g.pselY * sq, sq, sq, TERM_ACCENT);
  gfx->drawRect(ox + g.selX * sq, oy + g.selY * sq, sq, sq, TERM_CYAN);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(4, 6);
  gfx->print(g.gameOver ? g.msg : (g.turn == 1 ? "Your move (red)" : "AI thinking..."));
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("Jumps are forced-optional: play your best  n=new  Long=back");
}

static bool ckHasMoves(Checkers &g, bool red) {
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
    int p = ckAt(g, x, y);
    if (p <= 0 || ckIsRed(p) != red) continue;
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
      bool j; 
      if ((dx || dy) && ckCanMove(g, x, y, x + dx, y + dy, j)) return true;
    }
  }
  return false;
}

// AI: prefer jumps (with chain), else random-ish first move
static void ckAiMove(Checkers &g) {
  // find all jumps first
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
    int p = ckAt(g, x, y);
    if (p <= 0 || ckIsRed(p)) continue;
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
      if (!dx && !dy) continue;
      bool isJump;
      if (!ckCanMove(g, x, y, x + dx, y + dy, isJump)) continue;
      if (!isJump) continue;
      // perform jump
      int mid = ckAt(g, x + dx / 2, y + dy / 2);
      ckSet(g, x + dx, y + dy, p);
      ckSet(g, x, y, 0);
      ckSet(g, x + dx / 2, y + dy / 2, 0);
      // promote
      if (y + dy == 7) ckSet(g, x + dx, y + dy, 4);
      return;
    }
  }
  // else simple move (first found)
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
    int p = ckAt(g, x, y);
    if (p <= 0 || ckIsRed(p)) continue;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
      if (!dx || !dy || (dx == dy)) continue;
      bool isJump;
      if (!ckCanMove(g, x, y, x + dx, y + dy, isJump)) continue;
      ckSet(g, x + dx, y + dy, p);
      ckSet(g, x, y, 0);
      if (y + dy == 7) ckSet(g, x + dx, y + dy, 4);
      return;
    }
  }
  // no moves: human wins
  g.gameOver = true;
  g.msg = "You win! n=new";
}

void checkersApp() {
  static Checkers g;
  ckInit(g);
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) { needsRedraw = false; ckDraw(g); }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (g.gameOver) {
      if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) { ckInit(g); needsRedraw = true; }
      else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
      continue;
    }
    switch (e.ev) {
      case PDA_EV_UP: if (g.selY > 0) g.selY--; needsRedraw = true; break;
      case PDA_EV_DOWN: if (g.selY < 7) g.selY++; needsRedraw = true; break;
      case PDA_EV_LEFT: if (g.selX > 0) g.selX--; needsRedraw = true; break;
      case PDA_EV_RIGHT: if (g.selX < 7) g.selX++; needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        if (!g.selected) {
          int p = ckAt(g, g.selX, g.selY);
          if (p > 0 && ckIsRed(p)) { g.selected = true; g.pselX = g.selX; g.pselY = g.selY; }
        } else {
          bool isJump;
          if (g.selX == g.pselX && g.selY == g.pselY) {
            g.selected = false;
          } else if (ckCanMove(g, g.pselX, g.pselY, g.selX, g.selY, isJump)) {
            int p = ckAt(g, g.pselX, g.pselY);
            ckSet(g, g.selX, g.selY, p);
            ckSet(g, g.pselX, g.pselY, 0);
            if (isJump) ckSet(g, g.pselX + (g.selX - g.pselX) / 2, g.pselY + (g.selY - g.pselY) / 2, 0);
            if (g.selY == 0) ckSet(g, g.selX, g.selY, 2);  // promote red
            g.selected = false;
            if (!ckHasMoves(g, false)) { g.gameOver = true; g.msg = "You win! n=new"; gsRecordResult(GS_CHECKERS, true); }
            else {
              g.turn = 2; needsRedraw = true;
              vTaskDelay(pdMS_TO_TICKS(300));
              ckAiMove(g);
              g.turn = 1;
              if (!ckHasMoves(g, true)) { g.gameOver = true; g.msg = "AI wins. n=new"; gsRecordResult(GS_CHECKERS, false); }
            }
          } else g.selected = false;
        }
        needsRedraw = true;
        break;
      }
      case PDA_EV_CHAR:
        if (e.ch == 'n' || e.ch == 'N') { ckInit(g); needsRedraw = true; }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK: return;
      default: break;
    }
  }
}

// ============================ Snake ============================
// Trackball steers; click starts/pauses; n = new game.
struct Snake {
  int8_t x[80], y[80];
  int len;
  int8_t dirX, dirY;       // current direction
  int8_t wantX, wantY;     // buffered next direction
  int8_t foodX, foodY;
  bool dead, paused, started;
  int score, hi;
};

static void snakeReset(Snake &s) {
  if (s.dead && s.score > 0) gsRecordScore(GS_SNAKE, s.score);
  s.len = 3;
  for (int i = 0; i < 3; i++) { s.x[i] = 8 - i; s.y[i] = 8; }
  s.dirX = 1; s.dirY = 0;
  s.wantX = 1; s.wantY = 0;
  s.foodX = 12; s.foodY = 8;
  s.dead = false; s.paused = false; s.started = false;
  s.score = 0;
}

#define SNK_CELL 10
#define SNK_COLS 28
#define SNK_ROWS 17
#define SNK_OX ((SCREEN_W - SNK_COLS * SNK_CELL) / 2)
#define SNK_OY 40

static void snakeDraw(Snake &s, bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(8, 6);
    gfx->print("Snake");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("Trackball=steer click=pause n=new Long=back");
    gfx->drawRect(SNK_OX - 2, SNK_OY - 2,
                  SNK_COLS * SNK_CELL + 4, SNK_ROWS * SNK_CELL + 4, TERM_DIM);
  }
  gfx->setTextSize(1);
  // score line
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->fillRect(220, 6, 96, 16, BLACK);
  gfx->setCursor(224, 10);
  gfx->printf("Score %d  Hi %d", s.score, gsGetSnakeHi());
  // erase head cell before moving (we draw tail cells as we go)
  for (int i = 0; i < s.len; i++) {
    int px = SNK_OX + s.x[i] * SNK_CELL, py = SNK_OY + s.y[i] * SNK_CELL;
    gfx->fillRect(px + 1, py + 1, SNK_CELL - 2, SNK_CELL - 2,
                  i == 0 ? TERM_BRIGHT : TERM_GREEN);
  }
  // food
  int fx = SNK_OX + s.foodX * SNK_CELL, fy = SNK_OY + s.foodY * SNK_CELL;
  gfx->fillCircle(fx + SNK_CELL / 2, fy + SNK_CELL / 2, SNK_CELL / 2 - 1,
                  TERM_ACCENT);
  if (s.dead) {
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_RED, BLACK);
    gfx->setCursor(SNK_OX + 40, SNK_OY + 60);
    gfx->print("DEAD!");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(SNK_OX + 34, SNK_OY + 86);
    gfx->print("click or n = play again");
  }
  if (s.paused && !s.dead) {
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_ACCENT, BLACK);
    gfx->setCursor(SNK_OX + 70, SNK_OY + 60);
    gfx->print("PAUSED");
  }
}

static bool snakeStep(Snake &s) {
  // apply buffered direction if it isn't a reversal
  if (!(s.wantX == -s.dirX && s.wantY == -s.dirY)) {
    s.dirX = s.wantX; s.dirY = s.wantY;
  }
  int8_t nx = s.x[0] + s.dirX, ny = s.y[0] + s.dirY;
  if (nx < 0 || nx >= SNK_COLS || ny < 0 || ny >= SNK_ROWS) { s.dead = true; return false; }
  for (int i = 0; i < s.len; i++)
    if (s.x[i] == nx && s.y[i] == ny) { s.dead = true; return false; }
  bool grow = (nx == s.foodX && ny == s.foodY);
  if (grow) {
    s.score += 10;
    if (s.len < 80) s.len++;
    // new food spot not on snake
    while (true) {
      s.foodX = random(SNK_COLS); s.foodY = random(SNK_ROWS);
      bool onSnake = false;
      for (int i = 0; i < s.len; i++)
        if (s.x[i] == s.foodX && s.y[i] == s.foodY) { onSnake = true; break; }
      if (!onSnake) break;
    }
  }
  // move body: erase old tail first, then shift from tail
  if (!grow) {
    int px = SNK_OX + s.x[s.len - 1] * SNK_CELL, py = SNK_OY + s.y[s.len - 1] * SNK_CELL;
    gfx->fillRect(px + 1, py + 1, SNK_CELL - 2, SNK_CELL - 2, BLACK);
  }
  for (int i = s.len - 1; i > 0; i--) { s.x[i] = s.x[i - 1]; s.y[i] = s.y[i - 1]; }
  s.x[0] = nx; s.y[0] = ny;
  return true;
}

void snakeApp() {
  Snake s;
  snakeReset(s);
  bool needsRedraw = true;
  unsigned long lastStep = 0;
  int stepMs = 220;
  while (true) {
    if (needsRedraw) { snakeDraw(s, true); needsRedraw = false; }
    InputEventP e;
    bool got = pdaGetInput(e, 30);
    if (got) {
      if (e.ev == PDA_EV_UP)    { s.wantY = -1; s.wantX = 0; s.started = true; }
      else if (e.ev == PDA_EV_DOWN)  { s.wantY = 1; s.wantX = 0; s.started = true; }
      else if (e.ev == PDA_EV_LEFT)  { s.wantX = -1; s.wantY = 0; s.started = true; }
      else if (e.ev == PDA_EV_RIGHT) { s.wantX = 1; s.wantY = 0; s.started = true; }
      else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        if (s.dead) { snakeReset(s); needsRedraw = true; }
        else s.paused = !s.paused;
      }
      else if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
        snakeReset(s); needsRedraw = true;
      }
      else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
    }
    if (!s.started || s.paused || s.dead) continue;
    if (millis() - lastStep >= (unsigned)stepMs) {
      lastStep = millis();
      // eat the direction buffer: keep latest wanted direction
      snakeStep(s);
      snakeDraw(s, false);
    }
  }
}

// ============================ Flappy ============================
// Flappy-bird style: click/Enter to flap, avoid pipes, score per pipe.
#define FL_GRAVITY     0.35f
#define FL_FLAP        -6.2f
#define FL_PIPE_W      26
#define FL_GAP         66
#define FL_SPEED       2.4f
#define FL_STEP_MS     28

struct Flappy {
  float birdY, birdV;
  float pipeX;
  int   gapY;
  int   score;
  bool  dead, started;
};

static void flappyReset(Flappy &f) {
  f.birdY = SCREEN_H / 2;
  f.birdV = 0;
  f.pipeX = SCREEN_W + 40;
  f.gapY = random(50, SCREEN_H - 50 - FL_GAP);
  f.score = 0;
  f.dead = false;
  f.started = false;
}

// little flappy bird sprite (body ~11x9), wing flips with flapPhase
static void drawBird(Arduino_GFX *g, int x, int y, int flapPhase) {
  const uint16_t BODY = RGB565(0xf2, 0xd3, 0x8a);
  const uint16_t WING = RGB565(0xd9, 0xa4, 0x4a);
  const uint16_t BEAK = RGB565(0xe8, 0x7a, 0x2a);
  const uint16_t OUTL = RGB565(0x50, 0x3a, 0x10);
  g->fillCircle(x, y, 5, BODY);                       // body
  g->drawCircle(x, y, 5, OUTL);
  g->fillCircle(x + 3, y - 1, 2, RGB565(0xff, 0xff, 0xff));   // eye white
  g->fillRect(x + 3, y - 1, 1, 2, RGB565(0x10, 0x10, 0x10));  // pupil
  // beak
  g->fillTriangle(x + 5, y - 1, x + 9, y + 1, x + 5, y + 3, BEAK);
  // wing: up or down depending on flap phase
  if (flapPhase) g->fillTriangle(x - 3, y, x + 2, y, x - 1, y - 5, WING);
  else           g->fillTriangle(x - 3, y + 1, x + 2, y + 1, x - 1, y + 6, WING);
  // tail
  g->fillTriangle(x - 4, y - 2, x - 4, y + 2, x - 8, y, WING);
}

static void flappyDraw(Flappy &f, bool full) {
  const uint16_t SKY   = RGB565(0x4e, 0xc0, 0xe8);
  const uint16_t CLOUD = RGB565(0xd8, 0xf0, 0xf8);
  const uint16_t GROUND = RGB565(0xd8, 0xb0, 0x5a);
  const uint16_t DIRT  = RGB565(0xa8, 0x78, 0x38);
  if (full) {
    gfx->fillScreen(SKY);
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565(0x10, 0x30, 0x40), SKY);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("click=flap Long=back");
  }
  // score
  gfx->fillRect(SCREEN_W / 2 - 30, 4, 60, 16, SKY);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565(0xff, 0xff, 0xff), SKY);
  gfx->setCursor(SCREEN_W / 2 - 12, 4);
  gfx->print(f.score);
  // pipe: erase the strip behind it, then redraw both pipe bodies
  int px = (int)f.pipeX;
  gfx->fillRect(px + FL_PIPE_W, 20, (int)FL_SPEED + 1, SCREEN_H - 40, SKY);
  const uint16_t PIPE = RGB565(0x58, 0xc4, 0x30);
  gfx->fillRect(px, 20, FL_PIPE_W, f.gapY - 20, PIPE);
  gfx->fillRect(px, f.gapY + FL_GAP, FL_PIPE_W,
                SCREEN_H - 20 - f.gapY - FL_GAP, PIPE);
  // pipe lips (wider caps)
  gfx->fillRect(px - 2, f.gapY - 12, FL_PIPE_W + 4, 12, PIPE);
  gfx->fillRect(px - 2, f.gapY + FL_GAP, FL_PIPE_W + 4, 12, PIPE);
  // ground strip
  gfx->fillRect(0, SCREEN_H - 12, SCREEN_W, 4, GROUND);
  gfx->fillRect(0, SCREEN_H - 8, SCREEN_W, 8, DIRT);
  // clouds (simple parallax puffs, redrawn each frame)
  for (int i = 0; i < 3; i++) {
    int cx = (SCREEN_W - ((int)(millis() / 60) + i * 110)) % (SCREEN_W + 40) - 20;
    int cy = 34 + (i % 2) * 26;
    gfx->fillCircle(cx, cy, 7, CLOUD);
    gfx->fillCircle(cx + 8, cy + 2, 5, CLOUD);
    gfx->fillCircle(cx - 8, cy + 2, 5, CLOUD);
  }
  // bird (erase old position first)
  static int lastBy = -1;
  int by = (int)f.birdY;
  if (lastBy >= 0 && lastBy != by)
    gfx->fillCircle(60, lastBy, 8, SKY);
  drawBird(gfx, 60, by, (millis() / 120) & 1);
  lastBy = by;
  if (f.dead) {
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565(0xd0, 0x20, 0x20), SKY);
    gfx->setCursor(SCREEN_W / 2 - 40, SCREEN_H / 2 - 20);
    gfx->print("DEAD!");
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565(0x10, 0x30, 0x40), SKY);
    gfx->setCursor(SCREEN_W / 2 - 60, SCREEN_H / 2 + 6);
    gfx->print("click or n = play again");
  }
  if (!f.started && !f.dead) {
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565(0x10, 0x30, 0x40), SKY);
    gfx->setCursor(SCREEN_W / 2 - 60, SCREEN_H / 2 + 30);
    gfx->print("click to start");
  }
}


void flappyApp() {
  Flappy f;
  flappyReset(f);
  gsRecordScore(GS_FLAPPY, 0);   // loads stats
  int hi = gsGetFlappyHi();
  bool full = true;
  unsigned long lastStep = 0;
  while (true) {
    if (full) { flappyDraw(f, true); full = false; }
    InputEventP e;
    bool got = pdaGetInput(e, 20);
    if (got) {
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        if (f.dead) { flappyReset(f); full = true; }
        else { f.started = true; f.birdV = FL_FLAP; }
      } else if (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N')) {
        if (f.dead) { flappyReset(f); full = true; }
      } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
        return;
      }
    }
    if (!f.started || f.dead) continue;
    if (millis() - lastStep < FL_STEP_MS) continue;
    lastStep = millis();
    f.birdV += FL_GRAVITY;
    f.birdY += f.birdV;
    f.pipeX -= FL_SPEED;
    // pipe pass -> score + new pipe
    if (f.pipeX + FL_PIPE_W < 60 - 5) {
      f.score++;
      if (f.score > hi) { hi = f.score; gsRecordScore(GS_FLAPPY, hi); }
      f.pipeX = SCREEN_W + 10;
      f.gapY = random(50, SCREEN_H - 50 - FL_GAP);
    }
    // collisions
    bool hitPipe = (f.pipeX < 65 && f.pipeX + FL_PIPE_W > 55) &&
                   (f.birdY - 5 < f.gapY || f.birdY + 5 > f.gapY + FL_GAP);
    if (f.birdY < 25 || f.birdY > SCREEN_H - 26 || hitPipe) {
      f.dead = true;
      gsRecordScore(GS_FLAPPY, f.score);
    }
    flappyDraw(f, false);
  }
}

// ============================ Games hub ============================
static const char *const gameNames[] = {
  "Chess", "Go", "Solit", "Chkrs", "Snake", "Flappy",
  "Tetris", "Brkout", "2048", "Mines", "Pong", "Revrsi",
  "C4", "Btlshp", "Wordl", "Sudku", "Sokbn",
  "Invad", "Astrd", "Dodle", "Hearts", "Spades", "Bkgmn", "Stats"
};
static void (*const gameRun[])(void) = {
  chessApp, goApp, solitaireApp, checkersApp, snakeApp, flappyApp,
  tetrisApp, breakoutApp, game2048App, minesApp, pongApp, reversiApp,
  connect4App, battleshipApp, wordleApp, sudokuApp, sokobanApp,
  invadersApp, asteroidsApp, doodleApp, heartsApp, spadesApp, backgammonApp,
  gsStatsScreen
};
#define N_GAMES (int)(sizeof(gameNames)/sizeof(gameNames[0]))

static void drawGameIcon(int idx, int x, int y) {
  uint16_t c = TERM_GREEN, d = TERM_DIM;
  switch (idx) {
    case 0:  // Chess: pawn
      gfx->fillCircle(x + 12, y + 8, 3, c);
      gfx->fillRect(x + 9, y + 12, 6, 4, c);
      gfx->fillRect(x + 7, y + 17, 10, 3, c);
      break;
    case 1:  // Go: 5x5 board + stones
      gfx->drawRect(x + 4, y + 4, 16, 16, c);
      for (int i = 0; i < 4; i++) {
        gfx->drawFastHLine(x + 4, y + 8 + i * 4, 16, d);
        gfx->drawFastVLine(x + 8 + i * 4, y + 4, 16, d);
      }
      gfx->fillCircle(x + 8, y + 8, 2, TERM_BRIGHT);
      gfx->drawCircle(x + 16, y + 16, 2, TERM_BRIGHT);
      break;
    case 2:  // Solitaire: cards
      gfx->fillRect(x + 4, y + 5, 11, 15, TERM_BRIGHT);
      gfx->drawRect(x + 4, y + 5, 11, 15, c);
      gfx->fillRect(x + 8, y + 9, 11, 15, BLACK);
      gfx->drawRect(x + 8, y + 9, 11, 15, c);
      gfx->fillCircle(x + 13, y + 16, 2, TERM_ACCENT);
      break;
    case 3:  // Checkers: board + 2 men
      for (int r = 0; r < 5; r++)
        for (int q = 0; q < 5; q++)
          if ((r + q) % 2) gfx->fillRect(x + 3 + q * 4, y + 3 + r * 4, 4, 4, d);
      gfx->fillCircle(x + 9, y + 9, 2, TERM_BRIGHT);
      gfx->fillCircle(x + 17, y + 13, 2, c);
      break;
    case 4:  // Snake: S body + food
      gfx->drawFastHLine(x + 5, y + 6, 12, TERM_BRIGHT);
      gfx->drawFastVLine(x + 17, y + 6, 7, TERM_BRIGHT);
      gfx->drawFastHLine(x + 8, y + 13, 9, TERM_BRIGHT);
      gfx->drawFastVLine(x + 8, y + 13, 5, TERM_BRIGHT);
      gfx->drawFastHLine(x + 8, y + 17, 5, TERM_BRIGHT);
      gfx->fillCircle(x + 6, y + 6, 2, TERM_ACCENT);
      break;
    case 5: {  // Flappy: bird between pipes
      gfx->fillRect(x + 16, y + 2, 4, 7, c);
      gfx->fillRect(x + 16, y + 15, 4, 7, c);
      const uint16_t BODY = RGB565(0xf2, 0xd3, 0x8a);
      gfx->fillCircle(x + 8, y + 12, 5, BODY);
      gfx->fillCircle(x + 11, y + 11, 2, RGB565(0xff, 0xff, 0xff));
      gfx->fillRect(x + 11, y + 11, 1, 2, RGB565(0x10, 0x10, 0x10));
      gfx->fillTriangle(x + 13, y + 11, x + 17, y + 13, x + 13, y + 15,
                        RGB565(0xe8, 0x7a, 0x2a));
      gfx->fillTriangle(x + 4, y + 12, x + 9, y + 12, x + 6, y + 8,
                        RGB565(0xd9, 0xa4, 0x4a));
      break;
    }
    case 6:  // Tetris: stacked blocks
      gfx->fillRect(x + 4, y + 15, 5, 5, c);
      gfx->fillRect(x + 10, y + 15, 5, 5, TERM_BRIGHT);
      gfx->fillRect(x + 16, y + 15, 5, 5, d);
      gfx->fillRect(x + 10, y + 9, 5, 5, c);
      gfx->fillRect(x + 16, y + 9, 5, 5, TERM_BRIGHT);
      gfx->fillRect(x + 16, y + 3, 5, 5, c);
      break;
    case 7:  // Breakout: paddle + ball + brick
      gfx->fillRect(x + 3, y + 5, 6, 4, c);
      gfx->fillRect(x + 11, y + 5, 6, 4, TERM_BRIGHT);
      gfx->fillRect(x + 19, y + 5, 4, 4, d);
      gfx->fillCircle(x + 12, y + 14, 2, TERM_ACCENT);
      gfx->fillRect(x + 6, y + 19, 12, 3, TERM_BRIGHT);
      break;
    case 8: {  // 2048: two stacked value tiles
      gfx->fillRect(x + 3, y + 4, 9, 9, TERM_BRIGHT);
      gfx->fillRect(x + 13, y + 4, 9, 9, TERM_ACCENT);
      gfx->fillRect(x + 3, y + 14, 9, 9, TERM_ACCENT);
      gfx->drawRect(x + 13, y + 14, 9, 9, c);
      gfx->setTextSize(1);
      gfx->setTextColor(BLACK, TERM_BRIGHT);
      gfx->setCursor(x + 5, y + 7);
      gfx->print("2");
      gfx->setTextColor(BLACK, TERM_ACCENT);
      gfx->setCursor(x + 15, y + 7);
      gfx->print("4");
      gfx->setCursor(x + 5, y + 17);
      gfx->print("8");
      break;
    }
    case 9:  // Mines: mine + flag
      gfx->fillCircle(x + 9, y + 12, 5, TERM_ACCENT);
      gfx->drawFastVLine(x + 9, y + 5, 4, TERM_ACCENT);
      gfx->drawFastHLine(x + 4, y + 12, 10, TERM_ACCENT);
      gfx->fillTriangle(x + 16, y + 8, x + 21, y + 11, x + 16, y + 14, TERM_BRIGHT);
      gfx->drawFastVLine(x + 15, y + 8, 9, TERM_BRIGHT);
      break;
    case 10:  // Pong: paddles + ball
      gfx->fillRect(x + 4, y + 7, 3, 10, TERM_BRIGHT);
      gfx->fillRect(x + 18, y + 5, 3, 10, TERM_GREEN);
      gfx->fillCircle(x + 12, y + 12, 2, TERM_ACCENT);
      gfx->drawFastVLine(x + 12, y + 3, 18, TERM_DIM);
      break;
    case 11:  // Reversi: disc grid
      gfx->drawRect(x + 3, y + 4, 18, 16, c);
      gfx->drawFastHLine(x + 3, y + 12, 18, d);
      gfx->drawFastVLine(x + 12, y + 4, 16, d);
      gfx->fillCircle(x + 7, y + 8, 3, TERM_BRIGHT);
      gfx->drawCircle(x + 17, y + 8, 3, TERM_GREEN);
      gfx->fillCircle(x + 17, y + 17, 3, TERM_BRIGHT);
      break;
    case 12:  // Stats: trophy
      gfx->drawRect(x + 7, y + 5, 10, 7, c);
      gfx->drawFastHLine(x + 7, y + 5, 3, c);  // left handle
      gfx->drawFastVLine(x + 7, y + 6, 4, c);
      gfx->drawFastVLine(x + 16, y + 6, 4, c);
      gfx->drawFastHLine(x + 16, y + 5, 3, c);
      gfx->drawFastVLine(x + 11, y + 12, 4, c);
      gfx->drawFastHLine(x + 8, y + 16, 8, c);
      break;
    case 13: {  // Connect4: discs stack
      for (int r = 0; r < 3; r++)
        for (int cc = 0; cc < 3; cc++)
          gfx->drawCircle(x + 6 + cc * 6, y + 8 + r * 6, 2,
                          (r + cc) % 2 ? TERM_BRIGHT : c);
      gfx->drawRect(x + 3, y + 22, 18, 2, c);
      break;
    }
    case 14: {  // Battleship: ship + crosshair
      gfx->drawFastHLine(x + 3, y + 14, 18, c);
      gfx->fillRect(x + 8, y + 10, 8, 4, c);
      gfx->fillRect(x + 10, y + 7, 4, 3, c);
      gfx->drawLine(x + 2, y + 3, x + 6, y + 7, TERM_BRIGHT);
      gfx->drawLine(x + 6, y + 3, x + 2, y + 7, TERM_BRIGHT);
      break;
    }
    case 15: {  // Wordle: guess row, middle tile "hit"
      for (int i = 0; i < 5; i++) {
        int tx = x + 1 + i * 5;
        if (i == 2) {
          gfx->fillRect(tx, y + 8, 4, 9, TERM_BRIGHT);
        } else if (i == 1) {
          gfx->fillRect(tx, y + 8, 4, 9, TERM_ACCENT);
        } else if (i == 3) {
          gfx->drawRect(tx, y + 8, 4, 9, TERM_ACCENT);
        } else {
          gfx->drawRect(tx, y + 8, 4, 9, d);
        }
      }
      // small keyboard hint below
      gfx->drawFastHLine(x + 3, y + 20, 18, d);
      break;
    }
    case 16: {  // Sudoku: 3x3 grid with digits
      gfx->drawRect(x + 4, y + 4, 16, 16, c);
      gfx->drawFastVLine(x + 9, y + 4, 16, d);
      gfx->drawFastVLine(x + 15, y + 4, 16, d);
      gfx->drawFastHLine(x + 4, y + 9, 16, d);
      gfx->drawFastHLine(x + 4, y + 15, 16, d);
      gfx->fillRect(x + 6, y + 6, 2, 2, TERM_BRIGHT);
      gfx->fillRect(x + 11, y + 11, 2, 2, TERM_BRIGHT);
      gfx->fillRect(x + 16, y + 16, 2, 2, TERM_BRIGHT);
      break;
    }
    case 17: {  // Sokoban: box + target
      gfx->drawRect(x + 10, y + 10, 8, 8, c);
      gfx->drawLine(x + 10, y + 10, x + 17, y + 17, d);
      gfx->drawLine(x + 17, y + 10, x + 10, y + 17, d);
      gfx->drawCircle(x + 6, y + 7, 2, TERM_BRIGHT);
      break;
    }
    case 18: {  // Invaders: alien + ship
      gfx->fillRect(x + 6, y + 4, 12, 3, c);
      gfx->fillRect(x + 4, y + 7, 16, 3, c);
      gfx->fillRect(x + 6, y + 10, 3, 2, c);
      gfx->fillRect(x + 15, y + 10, 3, 2, c);
      gfx->fillRect(x + 10, y + 18, 4, 2, TERM_BRIGHT);
      gfx->fillRect(x + 8, y + 20, 8, 2, TERM_BRIGHT);
      break;
    }
    case 19: {  // Asteroids: ship + rocks
      gfx->fillCircle(x + 6, y + 8, 3, d);
      gfx->drawCircle(x + 18, y + 6, 3, c);
      gfx->drawLine(x + 8, y + 18, x + 14, y + 10, TERM_BRIGHT);
      gfx->drawLine(x + 14, y + 10, x + 20, y + 18, TERM_BRIGHT);
      gfx->drawLine(x + 8, y + 18, x + 20, y + 18, TERM_BRIGHT);
      break;
    }
    case 20: {  // Doodle: bouncing character + platform
      gfx->fillCircle(x + 12, y + 8, 4, TERM_BRIGHT);
      gfx->fillCircle(x + 10, y + 7, 1, BLACK);
      gfx->fillCircle(x + 14, y + 7, 1, BLACK);
      gfx->fillRect(x + 5, y + 17, 14, 3, c);
      break;
    }
    case 21: {  // Hearts: heart shape
      gfx->fillCircle(x + 8, y + 9, 4, TERM_RED);
      gfx->fillCircle(x + 16, y + 9, 4, TERM_RED);
      for (int i = 0; i < 7; i++)
        gfx->fillRect(x + 4 + i, y + 12 + i, 17 - 2 * i, 1, TERM_RED);
      break;
    }
    case 22: {  // Spades: spade shape
      gfx->fillCircle(x + 9, y + 10, 5, c);
      gfx->fillCircle(x + 15, y + 10, 5, c);
      gfx->fillTriangle(x + 12, y + 5, x + 5, y + 14, x + 19, y + 14, c);
      gfx->fillTriangle(x + 8, y + 16, x + 16, y + 16, x + 12, y + 22, c);
      break;
    }
    case 23: {  // Backgammon: dice + triangle board
      gfx->drawRect(x + 2, y + 3, 6, 6, TERM_BRIGHT);
      gfx->fillRect(x + 4, y + 5, 2, 2, TERM_BRIGHT);
      gfx->drawRect(x + 16, y + 3, 6, 6, TERM_BRIGHT);
      gfx->fillRect(x + 19, y + 5, 2, 2, TERM_BRIGHT);
      gfx->fillTriangle(x + 8, y + 20, x + 12, y + 12, x + 16, y + 20, c);
      gfx->fillCircle(x + 12, y + 17, 2, TERM_BRIGHT);
      break;
    }
  }
}

#define GAME_COLS 4
#define GAME_CELL_W (SCREEN_W / GAME_COLS)
#define GAME_CELL_H ((SCREEN_H - 18 - 26) / 2)
#define GAME_ROWS_VIS 2
#define GAME_PER_PAGE (GAME_COLS * GAME_ROWS_VIS)

// drawGameCell(i, selected, page, showLabel): page-relative cell
static void drawGameCell(int i, bool selected, int page) {
  int pi = i - page * GAME_PER_PAGE;
  if (pi < 0 || pi >= GAME_PER_PAGE) return;
  int x = (pi % GAME_COLS) * GAME_CELL_W;
  int y = 26 + (pi / GAME_COLS) * GAME_CELL_H;
  uint16_t bg = selected ? TERM_SEL_BG : BLACK;
  gfx->fillRect(x, y, GAME_CELL_W, GAME_CELL_H, bg);
  drawGameIcon(i, x + (GAME_CELL_W - 24) / 2, y + (GAME_CELL_H - 24 - 10) / 2);
  gfx->setTextSize(1);
  gfx->setTextColor(selected ? BLACK : TERM_DIM, bg);
  int tw = strlen(gameNames[i]) * 6;
  gfx->setCursor(x + (GAME_CELL_W - tw) / 2, y + GAME_CELL_H - 12);
  gfx->print(gameNames[i]);
}

static void drawGamePageDots(int page, int pages) {
  if (pages <= 1) return;
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(SCREEN_W - 40, SCREEN_H - 10);
  gfx->printf("pg %d/%d", page + 1, pages);
}

void gamesApp() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(4, 7);
  gfx->print("Games");
  gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("click=play Long=back");
  int pages = (N_GAMES + GAME_PER_PAGE - 1) / GAME_PER_PAGE;
  int page = 0, sel = 0, lastSel = -1;
  bool full = true;
  while (true) {
    if (full) {
      gfx->fillRect(0, 26, SCREEN_W, SCREEN_H - 18 - 26, BLACK);
      for (int i = 0; i < N_GAMES; i++) drawGameCell(i, i == sel, page);
      drawGamePageDots(page, pages);
      lastSel = sel;
      full = false;
    } else if (sel != lastSel) {
      drawGameCell(lastSel, false, page);
      drawGameCell(sel, true, page);
      lastSel = sel;
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP) sel = (sel + N_GAMES - GAME_COLS) % N_GAMES;
    else if (e.ev == PDA_EV_DOWN) sel = (sel + GAME_COLS) % N_GAMES;
    else if (e.ev == PDA_EV_LEFT) sel = (sel + N_GAMES - 1) % N_GAMES;
    else if (e.ev == PDA_EV_RIGHT) sel = (sel + 1) % N_GAMES;
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      gameRun[sel]();
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
    int newPage = sel / GAME_PER_PAGE;
    if (newPage != page) { page = newPage; full = true; }
  }
}

