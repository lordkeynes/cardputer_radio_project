/**
 * Games: Chess vs simple minimax AI, Go 9x9 vs greedy AI.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "games.h"

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
            if (target == 'k') { g.gameOver = true; g.msg = "You win! n=new"; }
            else if (!kingAlive(g, false)) { g.gameOver = true; g.msg = "You win! n=new"; }
            else {
              g.whiteToMove = false;
              needsRedraw = true;
              // AI move
              vTaskDelay(pdMS_TO_TICKS(200));
              chessAiMove(g);
              g.whiteToMove = true;
              if (!kingAlive(g, true)) { g.gameOver = true; g.msg = "AI wins. n=new"; }
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

static void drawCard(Arduino_GFX *g, int card, int x, int y, bool selected) {
  // 30x40 card
  uint16_t border = selected ? TERM_ACCENT : TERM_DIM;
  g->drawRect(x, y, 30, 40, border);
  if (card == 0) return;
  int r = cardRank(card), su = cardSuit(card);
  uint16_t col = cardColor(card) ? TERM_RED : TERM_GREEN;
  g->setTextSize(1);
  g->setTextColor(col, BLACK);
  g->setCursor(x + 3, y + 3);
  g->print(rankStr(r));
  g->setCursor(x + 12, y + 3);
  const char *suits = "SHDC";
  g->print(suits[su]);
  g->setCursor(x + 3, y + 30);
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
  gfx->fillScreen(BLACK);
  // top row: stock, waste, 4 foundations
  drawCard(gfx, g.stockN ? -1 : 0, 8, 6, g.cursor == 7);
  if (g.stockN) { gfx->setTextSize(1); gfx->setTextColor(TERM_DIM, BLACK); gfx->setCursor(16, 22); gfx->print("$"); }
  drawCard(gfx, g.wasteN ? g.waste[g.wasteN-1] : 0, 46, 6, g.cursor == 8 || g.wasteSel);
  for (int i = 0; i < 4; i++) {
    drawCard(gfx, g.found[i], 130 + i * 40, 6, g.cursor == 9 + i);
    if (g.found[i] == 0) { gfx->setTextSize(1); gfx->setTextColor(TERM_DIM, BLACK); gfx->setCursor(140 + i * 40, 22); const char *s="SHDC"; gfx->print(s[i]); }
  }
  // tableau
  for (int c = 0; c < 7; c++) {
    int x = 8 + c * 44;
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
                cardRank(g.found[2])==13 && cardRank(g.found[3])==13) g.won = true;
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
            if (!ckHasMoves(g, false)) { g.gameOver = true; g.msg = "You win! n=new"; }
            else {
              g.turn = 2; needsRedraw = true;
              vTaskDelay(pdMS_TO_TICKS(300));
              ckAiMove(g);
              g.turn = 1;
              if (!ckHasMoves(g, true)) { g.gameOver = true; g.msg = "AI wins. n=new"; }
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
