/**
 * Arcade games pack: Space Invaders, Asteroids, Doodle Jump.
 * Shared input queue, theme colors, game stats.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Arduino_GFX.h>
#include "pda.h"
#include "theme.h"
#include "games.h"
#include "gamestats.h"

extern Arduino_GFX *gfx;

// ============================ Space Invaders ============================
#define IV_COLS 6
#define IV_ROWS 4
#define IV_MAXB 4

struct Invaders {
  float ax[IV_ROWS][IV_COLS];   // alien x offsets (px, from grid origin)
  bool  alive[IV_ROWS][IV_COLS];
  float gx, gy;                 // grid origin
  float gvx;
  int stepMs;
  int8_t bx[IV_MAXB], by[IV_MAXB];  // player bullets (y up? we use screen coords, y decreasing)
  int8_t eb[3];                // enemy bullet columns, -1 none (y tracked below)
  int8_t eby[3];
  int px;                      // player ship x
  int score, lives;
  bool over;
  uint32_t lastStep, lastAnim, lastEB;
  int animFrame;
};

static void ivReset(Invaders &s) {
  memset(&s, 0, sizeof(s));
  s.gx = 40; s.gy = 40; s.gvx = 0.6f;
  s.stepMs = 220;
  s.px = SCREEN_W / 2;
  s.lives = 3;
  s.eb[0] = s.eb[1] = s.eb[2] = -1;
  for (int b = 0; b < IV_MAXB; b++) { s.bx[b] = -1; s.by[b] = -1; }
  for (int r = 0; r < IV_ROWS; r++)
    for (int c = 0; c < IV_COLS; c++) { s.alive[r][c] = true; s.ax[r][c] = 0; }
  s.lastStep = millis();
}

static void ivDraw(Invaders &s, bool full) {
  char buf[32];
  gfx->setTextColor(TERM_GREEN);
  if (full) {
    gfx->fillScreen(RGB565(0x0c, 0x14, 0x0c));
    gfx->setTextSize(1);
    gfx->setCursor(4, 7); gfx->print("INVADERS");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
  }
  gfx->setTextSize(1);
  gfx->setCursor(200, 7);
  gfx->fillRect(200, 4, 116, 10, RGB565(0x0c, 0x14, 0x0c));
  snprintf(buf, sizeof(buf), "%d   L%d  hi %d", s.score, s.lives, gsGetHi(GS_INVADERS));
  gfx->print(buf);
  // aliens
  for (int r = 0; r < IV_ROWS; r++) {
    for (int c = 0; c < IV_COLS; c++) {
      int x = (int)(s.gx + c * 34 + s.ax[r][c]);
      int y = (int)(s.gy + r * 16);
      if (x < -12 || x > SCREEN_W) continue;
      // clear cell then draw if alive (simple full redraw of alien area)
      gfx->fillRect(x, y, 12, 8, RGB565(0x0c, 0x14, 0x0c));
      if (!s.alive[r][c]) continue;
      if (s.animFrame) {
        gfx->fillRect(x + 1, y, 3, 3, TERM_GREEN);
        gfx->fillRect(x + 8, y, 3, 3, TERM_GREEN);
        gfx->fillRect(x, y + 3, 12, 2, TERM_GREEN);
      } else {
        gfx->fillRect(x, y, 12, 3, TERM_GREEN);
        gfx->fillRect(x, y + 3, 12, 2, TERM_GREEN);
        gfx->fillRect(x + 2, y + 5, 2, 2, TERM_GREEN);
        gfx->fillRect(x + 8, y + 5, 2, 2, TERM_GREEN);
      }
    }
  }
  // player bullets
  for (int b = 0; b < IV_MAXB; b++) {
    if (s.by[b] < 0) continue;
    gfx->fillRect(s.bx[b], s.by[b], 1, 4, TERM_BRIGHT);
  }
  // enemy bullets
  for (int b = 0; b < 3; b++) {
    if (s.eb[b] < 0) continue;
    gfx->fillRect(s.eb[b], s.eby[b], 2, 4, TERM_RED);
  }
  // player ship
  gfx->fillRect(s.px - 8, SCREEN_H - 16, 16, 3, TERM_BRIGHT);
  gfx->fillRect(s.px - 1, SCREEN_H - 19, 2, 3, TERM_BRIGHT);
  if (full) {
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("< > move  SEL fire  BK exit");
  }
}

void invadersApp() {
  Invaders s;
  ivReset(s);
  ivDraw(s, true);
  bool overDrawn = false;
  while (true) {
    InputEventP e;
    bool have = pdaGetInput(e, 20);
    if (have) {
      if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT) return;
      if (s.over) {
        if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
            (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
          ivReset(s); ivDraw(s, true); overDrawn = false;
        }
        continue;
      }
      if (e.ev == PDA_EV_LEFT)  s.px -= 6;
      if (e.ev == PDA_EV_RIGHT) s.px += 6;
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        for (int b = 0; b < IV_MAXB; b++) {
          if (s.by[b] < 0) { s.bx[b] = s.px; s.by[b] = SCREEN_H - 22; break; }
        }
      }
      if (s.px < 10) s.px = 10;
      if (s.px > SCREEN_W - 10) s.px = SCREEN_W - 10;
    }
    if (s.over) {
      if (!overDrawn) {
        ivDraw(s, false);
        gfx->setTextColor(TERM_BRIGHT);
        gfx->setTextSize(2);
        gfx->setCursor(80, 100);
        gfx->print("GAME OVER");
        gfx->setTextSize(1);
        gfx->setCursor(80, 120);
        gfx->print("SEL/n new  BK exit");
        overDrawn = true;
      }
      continue;
    }
    uint32_t now = millis();
    if (now - s.lastStep >= (uint32_t)s.stepMs) {
      s.lastStep = now;
      s.animFrame ^= 1;
      s.gx += s.gvx;
      bool edge = false;
      for (int r = 0; r < IV_ROWS; r++)
        for (int c = 0; c < IV_COLS; c++)
          if (s.alive[r][c]) {
            int x = (int)(s.gx + c * 34);
            if (s.gvx > 0 && x + 12 > SCREEN_W - 2) edge = true;
            if (s.gvx < 0 && x < 2) edge = true;
          }
      if (edge) { s.gvx = -s.gvx; s.gy += 10; }
      // clear old bullet trails + enemy bullets
      for (int b = 0; b < IV_MAXB; b++) {
        if (s.by[b] >= 0) {
          gfx->fillRect(s.bx[b], s.by[b], 1, 6, RGB565(0x0c, 0x14, 0x0c));
          s.by[b] -= 5;
          if (s.by[b] < 20) { s.by[b] = -1; continue; }
          int gx = (int)((s.gx + 12 - 24) / 34);
          // hit test aliens directly
          for (int r = 0; r < IV_ROWS; r++) {
            for (int c = 0; c < IV_COLS; c++) {
              if (!s.alive[r][c]) continue;
              int ax = (int)(s.gx + c * 34);
              int ay = (int)(s.gy + r * 16);
              if (s.bx[b] >= ax && s.bx[b] < ax + 12 &&
                  s.by[b] >= ay && s.by[b] < ay + 8) {
                s.alive[r][c] = false;
                s.by[b] = -1;
                s.score += 10;
                if (s.stepMs > 80) s.stepMs -= 4;
              }
            }
          }
        }
      }
      // enemy fire
      if (now - s.lastEB > 900 && s.eb[0] >= 0 || s.eb[1] >= 0 || s.eb[2] >= 0) {
        // bullets in flight; move them
      }
      if (now - s.lastEB > (uint32_t)(s.stepMs * 4)) {
        s.lastEB = now;
        for (int b = 0; b < 3; b++) {
          if (s.eb[b] < 0) {
            // pick a random alive alien column
            int tries = 0;
            while (tries++ < 12) {
              int c = random(0, IV_COLS);
              for (int r = IV_ROWS - 1; r >= 0; r--) {
                if (s.alive[r][c]) {
                  s.eb[b] = (int)(s.gx + c * 34 + 6);
                  s.eby[b] = (int)(s.gy + r * 16 + 8);
                  b = 3;  // stop
                  break;
                }
              }
              if (b >= 3) break;
            }
            break;
          }
        }
      }
      for (int b = 0; b < 3; b++) {
        if (s.eb[b] < 0) continue;
        gfx->fillRect(s.eb[b], s.eby[b], 2, 6, RGB565(0x0c, 0x14, 0x0c));
        s.eby[b] += 3;
        if (s.eby[b] > SCREEN_H - 12) { s.eb[b] = -1; continue; }
        if (s.eby[b] >= SCREEN_H - 19 &&
            s.eb[b] >= s.px - 8 && s.eb[b] < s.px + 8) {
          s.eb[b] = -1;
          s.lives--;
          if (s.lives <= 0) {
            s.over = true;
            gsRecordScore(GS_INVADERS, s.score);
          }
        }
      }
      // aliens reached bottom?
      for (int r = 0; r < IV_ROWS; r++)
        for (int c = 0; c < IV_COLS; c++)
          if (s.alive[r][c] && s.gy + r * 16 + 8 > SCREEN_H - 22) {
            s.over = true;
            gsRecordScore(GS_INVADERS, s.score);
          }
      // all dead -> next wave
      bool any = false;
      for (int r = 0; r < IV_ROWS; r++)
        for (int c = 0; c < IV_COLS; c++) if (s.alive[r][c]) any = true;
      if (!any) {
        for (int r = 0; r < IV_ROWS; r++)
          for (int c = 0; c < IV_COLS; c++) s.alive[r][c] = true;
        s.gx = 40; s.gy = 40;
        s.stepMs = s.stepMs > 120 ? s.stepMs - 30 : 80;
      }
      ivDraw(s, false);
    }
  }
}

// ============================ Asteroids ============================
#define AS_MAXR 8
#define AS_MAXB 4

struct AsteroidsG {
  float sx, sy, sa;      // ship pos + angle (radians, 0 = up)
  float svx, svy;
  bool thrust;
  float rx[AS_MAXR], ry[AS_MAXR], ra[AS_MAXR], rvx[AS_MAXR], rvy[AS_MAXR];
  int rsize[AS_MAXR];    // 3=large 2=med 1=small, 0 = empty slot
  float bx[AS_MAXB], by[AS_MAXB], bvx[AS_MAXB], bvy[AS_MAXB];
  int blife[AS_MAXB];    // frames left, -1 unused
  int score, lives;
  bool over;
  uint32_t lastStep;
};

static void asResetShip(AsteroidsG &s) {
  s.sx = SCREEN_W / 2; s.sy = SCREEN_H / 2;
  s.sa = -M_PI / 2;
  s.svx = s.svy = 0;
}

static void asReset(AsteroidsG &s) {
  memset(&s, 0, sizeof(s));
  s.lives = 3;
  asResetShip(s);
  for (int i = 0; i < AS_MAXB; i++) s.blife[i] = -1;
  // 4 large rocks
  for (int i = 0; i < 4; i++) {
    s.rx[i] = random(20, SCREEN_W - 20);
    s.ry[i] = random(30, SCREEN_H - 30);
    if (abs(s.rx[i] - s.sx) < 50 && abs(s.ry[i] - s.sy) < 50) s.rx[i] += 80;
    s.ra[i] = random(0, 100) * 0.06f;
    s.rvx[i] = (random(0, 100) - 50) * 0.02f;
    s.rvy[i] = (random(0, 100) - 50) * 0.02f;
    s.rsize[i] = 3;
  }
  s.lastStep = millis();
}

static void asDrawShip(AsteroidsG &s) {
  float c = cosf(s.sa), sn = sinf(s.sa);
  float tipX = s.sx + c * 8, tipY = s.sy + sn * 8;
  float lX = s.sx - c * 6 - sn * 5, lY = s.sy - sn * 6 + c * 5;
  float rX = s.sx - c * 6 + sn * 5, rY = s.sy - sn * 6 - c * 5;
  gfx->drawLine(tipX, tipY, lX, lY, TERM_BRIGHT);
  gfx->drawLine(tipX, tipY, rX, rY, TERM_BRIGHT);
  gfx->drawLine(lX, lY, rX, rY, TERM_BRIGHT);
  if (s.thrust) {
    gfx->drawLine(s.sx - c * 4, s.sy - sn * 4,
                  s.sx - c * 12, s.sy - sn * 12, TERM_GREEN);
  }
}

static void asDrawRock(float x, float y, int size) {
  int r = size == 3 ? 14 : size == 2 ? 9 : 6;
  gfx->drawCircle(x, y, r, TERM_GREEN);
  gfx->drawLine(x - r * 0.5f, y - r * 0.5f, x + r * 0.4f, y + r * 0.4f, TERM_GREEN);
  gfx->drawLine(x + r * 0.5f, y - r * 0.4f, x - r * 0.3f, y + r * 0.5f, TERM_DIM);
}

static void asDraw(AsteroidsG &s, bool full) {
  char buf[32];
  gfx->setTextColor(TERM_GREEN);
  gfx->setTextSize(1);
  if (full) {
    gfx->fillScreen(RGB565(0x0c, 0x14, 0x0c));
    gfx->setCursor(4, 7); gfx->print("ASTEROIDS");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("< > turn  ^ thrust(UP)  SEL fire");
  }
  gfx->fillRect(200, 4, 116, 10, RGB565(0x0c, 0x14, 0x0c));
  gfx->setCursor(200, 7);
  snprintf(buf, sizeof(buf), "%d   L%d  hi %d", s.score, s.lives, gsGetHi(GS_ASTEROIDS));
  gfx->print(buf);
  for (int i = 0; i < AS_MAXR; i++)
    if (s.rsize[i] > 0) asDrawRock(s.rx[i], s.ry[i], s.rsize[i]);
  for (int i = 0; i < AS_MAXB; i++)
    if (s.blife[i] > 0)
      gfx->fillRect((int)s.bx[i] - 1, (int)s.by[i] - 1, 2, 2, TERM_BRIGHT);
  asDrawShip(s);
}

static void asWrap(float &x, float &y) {
  if (x < 0) x += SCREEN_W;  if (x >= SCREEN_W) x -= SCREEN_W;
  if (y < 20) y += SCREEN_H - 20;  if (y >= SCREEN_H) y -= (SCREEN_H - 20);
}

static void asSplitRock(AsteroidsG &s, int i) {
  int sz = s.rsize[i];
  if (sz > 1) {
    // spawn two smaller rocks in first free slots
    for (int k = 0; k < 2; ) {
      int j = -1;
      for (int t = 0; t < AS_MAXR; t++) if (s.rsize[t] == 0) { j = t; break; }
      if (j < 0) break;
      s.rx[j] = s.rx[i] + random(-4, 5);
      s.ry[j] = s.ry[i] + random(-4, 5);
      s.rvx[j] = (random(0, 100) - 50) * 0.04f;
      s.rvy[j] = (random(0, 100) - 50) * 0.04f;
      s.rsize[j] = sz - 1;
      k++;
    }
  }
  s.rsize[i] = 0;
}

void asteroidsApp() {
  AsteroidsG s;
  asReset(s);
  asDraw(s, true);
  bool overDrawn = false;
  bool fullRedraw = true;
  while (true) {
    InputEventP e;
    bool have = pdaGetInput(e, 16);
    if (have) {
      if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT) return;
      if (s.over) {
        if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
            (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
          asReset(s); fullRedraw = true; overDrawn = false;
        }
        continue;
      }
      if (e.ev == PDA_EV_LEFT)  s.sa -= 0.35f;
      if (e.ev == PDA_EV_RIGHT) s.sa += 0.35f;
      s.thrust = (e.ev == PDA_EV_UP);
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        for (int i = 0; i < AS_MAXB; i++) {
          if (s.blife[i] <= 0) {
            s.bx[i] = s.sx + cosf(s.sa) * 9;
            s.by[i] = s.sy + sinf(s.sa) * 9;
            s.bvx[i] = s.svx + cosf(s.sa) * 3.5f;
            s.bvy[i] = s.svy + sinf(s.sa) * 3.5f;
            s.blife[i] = 60;
            break;
          }
        }
      }
    } else {
      s.thrust = false;
    }
    if (s.over) {
      if (!overDrawn) {
        asDraw(s, false);
        gfx->setTextColor(TERM_BRIGHT); gfx->setTextSize(2);
        gfx->setCursor(80, 100); gfx->print("GAME OVER");
        gfx->setTextSize(1);
        gfx->setCursor(80, 120); gfx->print("SEL/n new  BK exit");
        overDrawn = true;
      }
      continue;
    }
    uint32_t now = millis();
    if (now - s.lastStep >= 33 || fullRedraw) {
      s.lastStep = now;
      // physics: erase old sprites by clearing field, then redraw all
      gfx->fillRect(0, 19, SCREEN_W, SCREEN_H - 30, RGB565(0x0c, 0x14, 0x0c));
      if (s.thrust) {
        s.svx += cosf(s.sa) * 0.18f;
        s.svy += sinf(s.sa) * 0.18f;
      }
      float sp = sqrtf(s.svx * s.svx + s.svy * s.svy);
      if (sp > 2.5f) { s.svx *= 2.5f / sp; s.svy *= 2.5f / sp; }
      s.sx += s.svx; s.sy += s.svy;
      asWrap(s.sx, s.sy);
      for (int i = 0; i < AS_MAXR; i++) {
        if (s.rsize[i] == 0) continue;
        s.rx[i] += s.rvx[i]; s.ry[i] += s.rvy[i];
        asWrap(s.rx[i], s.ry[i]);
      }
      for (int i = 0; i < AS_MAXB; i++) {
        if (s.blife[i] <= 0) continue;
        s.bx[i] += s.bvx[i]; s.by[i] += s.bvy[i];
        s.blife[i]--;
        asWrap(s.bx[i], s.by[i]);
        // bullet vs rock
        for (int j = 0; j < AS_MAXR; j++) {
          if (s.rsize[j] == 0) continue;
          int r = s.rsize[j] == 3 ? 14 : s.rsize[j] == 2 ? 9 : 6;
          float dx = s.bx[i] - s.rx[j], dy = s.by[i] - s.ry[j];
          if (dx * dx + dy * dy < r * r) {
            s.score += s.rsize[j] * 10;
            asSplitRock(s, j);
            s.blife[i] = 0;
            break;
          }
        }
      }
      // rock vs ship
      for (int j = 0; j < AS_MAXR; j++) {
        if (s.rsize[j] == 0) continue;
        int r = s.rsize[j] == 3 ? 14 : s.rsize[j] == 2 ? 9 : 6;
        float dx = s.sx - s.rx[j], dy = s.sy - s.ry[j];
        if (dx * dx + dy * dy < (r + 5) * (r + 5)) {
          s.lives--;
          if (s.lives <= 0) {
            s.over = true;
            gsRecordScore(GS_ASTEROIDS, s.score);
          } else {
            asResetShip(s);
          }
        }
      }
      // new wave when clear
      bool any = false;
      for (int j = 0; j < AS_MAXR; j++) if (s.rsize[j] > 0) any = true;
      if (!any) {
        for (int i = 0; i < 4; i++) {
          s.rx[i] = random(20, SCREEN_W - 20);
          s.ry[i] = random(30, SCREEN_H - 30);
          if (abs(s.rx[i] - s.sx) < 60 && abs(s.ry[i] - s.sy) < 60) s.rx[i] += 100;
          s.rvx[i] = (random(0, 100) - 50) * 0.025f;
          s.rvy[i] = (random(0, 100) - 50) * 0.025f;
          s.rsize[i] = 3;
        }
      }
      asDraw(s, false);
      fullRedraw = false;
    }
  }
}

// ============================ Doodle Jump ============================
#define DJ_NPLAT 12
#define DJ_PW 40

struct Doodle {
  float x, y;         // player center
  float vx, vy;
  float plats[DJ_NPLAT][2];  // x (left), y (top) of each platform
  int platv[DJ_NPLAT];       // -1 static, >0 = move speed (moving plats)
  float camY;         // world y of screen top
  int score;
  bool over;
  uint32_t lastStep;
  // previous-frame screen positions for flicker-free erase
  int prevPX, prevPY;                 // player
  float prevPlatSX[DJ_NPLAT][2];      // platforms
  bool prevValid;
};

static void djGenPlat(Doodle &d, int i, float minSep, float maxSep) {
  float y = d.plats[i == 0 ? 0 : i - 1][1];  // careful with order
  // platforms generated upward: index 0 at top; we gen bottom-up by caller
  (void)y; (void)minSep; (void)maxSep;
  d.plats[i][0] = random(0, SCREEN_W - DJ_PW);
  d.plats[i][1] = random(20, 300) * 1.0f;   // placeholder, caller rewrites
  d.platv[i] = -1;
}

static void djReset(Doodle &d) {
  memset(&d, 0, sizeof(d));
  d.x = SCREEN_W / 2;
  d.y = 200;
  d.prevValid = false;
  d.vy = 0;
  d.camY = 0;
  // platform 0: starting platform right under player
  d.plats[0][0] = d.x - DJ_PW / 2;
  d.plats[0][1] = d.y + 8;
  // gen upward with spacing
  float py = d.plats[0][1];
  for (int i = 1; i < DJ_NPLAT; i++) {
    py -= random(30, 52);
    d.plats[i][0] = random(4, SCREEN_W - DJ_PW - 4);
    d.plats[i][1] = py;
    d.platv[i] = -1;
  }
  d.lastStep = millis();
}

static void djDraw(Doodle &d, bool full) {
  char buf[40];
  gfx->setTextColor(TERM_GREEN);
  gfx->setTextSize(1);
  if (full) {
    gfx->fillScreen(RGB565(0x0c, 0x14, 0x0c));
    gfx->setCursor(4, 7); gfx->print("DOODLE");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("< > move  BK exit");
  }
  const uint16_t BG = RGB565(0x0c, 0x14, 0x0c);
  if (full || !d.prevValid) {
    gfx->fillRect(0, 19, SCREEN_W, SCREEN_H - 30, BG);
  } else {
    // erase only what moved last frame: player + platforms
    gfx->fillRect(d.prevPX - 7, d.prevPY - 7, 15, 16, BG);
    for (int i = 0; i < DJ_NPLAT; i++) {
      float sy = d.prevPlatSX[i][1];
      if (sy < 14 || sy > SCREEN_H + 4) continue;
      gfx->fillRect((int)d.prevPlatSX[i][0] - 1, (int)sy - 1, DJ_PW + 2, 7, BG);
    }
  }
  // score top-right
  snprintf(buf, sizeof(buf), "h %d", d.score);
  gfx->fillRect(260, 4, 56, 10, RGB565(0x0c, 0x14, 0x0c));
  gfx->setCursor(260, 7);
  gfx->print(buf);
  // platforms (world y -> screen y = worldY - camY); record screen pos
  for (int i = 0; i < DJ_NPLAT; i++) {
    float sy = d.plats[i][1] - d.camY;
    d.prevPlatSX[i][0] = d.plats[i][0];
    d.prevPlatSX[i][1] = sy;
    if (sy < 18 || sy > SCREEN_H - 2) continue;
    gfx->fillRect((int)d.plats[i][0], (int)sy, DJ_PW, 4, TERM_GREEN);
  }
  // player: little doodle character (body + eyes)
  int px = (int)d.x, py = (int)(d.y - d.camY);
  gfx->fillCircle(px, py, 5, TERM_BRIGHT);
  gfx->fillCircle(px - 2, py - 2, 1, RGB565(0, 0, 0));
  gfx->fillCircle(px + 2, py - 2, 1, RGB565(0, 0, 0));
  gfx->fillRect(px - 4, py + 5, 3, 3, TERM_GREEN);
  gfx->fillRect(px + 1, py + 5, 3, 3, TERM_GREEN);
  d.prevPX = px;
  d.prevPY = py;
  d.prevValid = true;
}

static void djShiftUp(Doodle &d) {
  // remove lowest platform (largest y), add new one at top
  int lowest = 0;
  for (int i = 1; i < DJ_NPLAT; i++)
    if (d.plats[i][1] > d.plats[lowest][1]) lowest = i;
  float topY = d.plats[0][1];
  for (int i = 0; i < DJ_NPLAT; i++)
    if (d.plats[i][1] < topY) topY = d.plats[i][1];
  d.plats[lowest][0] = random(4, SCREEN_W - DJ_PW - 4);
  d.plats[lowest][1] = topY - random(30, 52);
  d.platv[lowest] = -1;
}

void doodleApp() {
  Doodle d;
  djReset(d);
  djDraw(d, true);
  bool overDrawn = false;
  while (true) {
    InputEventP e;
    bool have = pdaGetInput(e, 16);
    if (have) {
      if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT) return;
      if (d.over) {
        if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
            (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
          djReset(d); djDraw(d, true); overDrawn = false;
        }
        continue;
      }
      if (e.ev == PDA_EV_LEFT)  d.x -= 7;
      if (e.ev == PDA_EV_RIGHT) d.x += 7;
    }
    if (d.over) {
      if (!overDrawn) {
        djDraw(d, false);
        gfx->setTextColor(TERM_BRIGHT); gfx->setTextSize(2);
        gfx->setCursor(90, 100); gfx->print("GAME OVER");
        gfx->setTextSize(1);
        gfx->setCursor(90, 120); gfx->print("SEL/n new  BK exit");
        overDrawn = true;
      }
      continue;
    }
    uint32_t now = millis();
    if (now - d.lastStep >= 33) {
      d.lastStep = now;
      // physics
      d.vy += 0.35f;
      d.y += d.vy;
      if (d.vy > 0) {  // falling: check platform collisions
        for (int i = 0; i < DJ_NPLAT; i++) {
          if (d.x + 4 >= d.plats[i][0] && d.x - 4 <= d.plats[i][0] + DJ_PW &&
              d.y + 6 >= d.plats[i][1] && d.y + 6 <= d.plats[i][1] + 8 &&
              d.y + 6 - d.vy <= d.plats[i][1] + 2) {
            d.vy = -8.5f;
            break;
          }
        }
      }
      // camera follows player when above mid-screen
      if (d.y - d.camY < 110) {
        float scroll = 110 - (d.y - d.camY);
        d.camY -= scroll;
        for (int i = 0; i < DJ_NPLAT; i++) d.plats[i][1] += scroll;
        d.y += scroll;
        d.score += (int)scroll;
      }
      // recycle platforms that fell below view
      for (int i = 0; i < DJ_NPLAT; i++) {
        if (d.plats[i][1] - d.camY > SCREEN_H + 10) {
          float topY = d.plats[0][1];
          for (int j = 0; j < DJ_NPLAT; j++)
            if (d.plats[j][1] < topY) topY = d.plats[j][1];
          d.plats[i][0] = random(4, SCREEN_W - DJ_PW - 4);
          d.plats[i][1] = topY - random(30, 52);
          d.platv[i] = -1;
        }
      }
      // fell below screen -> game over
      if (d.y - d.camY > SCREEN_H + 10) {
        d.over = true;
        gsRecordScore(GS_DOODLE, d.score);
      }
      // screen wrap horizontally
      if (d.x < -5) d.x = SCREEN_W + 5;
      if (d.x > SCREEN_W + 5) d.x = -5;
      djDraw(d, false);
    }
  }
}
