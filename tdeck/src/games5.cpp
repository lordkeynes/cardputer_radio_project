/**
 * Card games pack: Hearts, Spades, Backgammon.
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

// ============================ shared deck helpers ============================
#define RANK_A 14
#define SUITS "SHDC"   // Spades Hearts Diamonds Clubs

static int cardRank(int c) { return (c >> 2) + 2; }   // 2..14
static int cardSuit(int c) { return c & 3; }          // 0=S 1=H 2=D 3=C

// ============================ Hearts ============================
#define HT_NHAND 13
#define HT_NAME_LEN 12

struct HeartsG {
  int hands[4][HT_NHAND];   // -1 = empty slot
  int nh[4];                // cards left per player
  int trick[4];             // card played this trick, -1 none
  int lead;                 // leading player
  int turn;                 // current player
  int heartsBroken;
  int taken[4];             // points taken
  int round;                // tricks completed
  int sel;                  // selected card in human hand
  int phase;                // 0 = play, 1 = round over
  int pass[3];              // 3 cards human passes (indices), -1 none
  bool passPhase;
  char names[4][HT_NAME_LEN];
};

static void htSortHand(int *h, int n) {
  for (int i = 1; i < n; i++) {
    int c = h[i], j = i - 1;
    while (j >= 0 && (cardRank(h[j]) > cardRank(c) ||
           (cardRank(h[j]) == cardRank(c) && cardSuit(h[j]) > cardSuit(c)))) {
      h[j + 1] = h[j]; j--;
    }
    h[j + 1] = c;
  }
}

static void htDeal(HeartsG &g) {
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = i;
  for (int i = 51; i > 0; i--) {
    int j = random(0, i + 1);
    int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
  }
  for (int p = 0; p < 4; p++) {
    for (int i = 0; i < HT_NHAND; i++) g.hands[p][i] = deck[p * 13 + i];
    htSortHand(g.hands[p], HT_NHAND);
    g.nh[p] = HT_NHAND;
  }
}

static void htReset(HeartsG &g) {
  memset(&g, 0, sizeof(g));
  strcpy(g.names[0], "You");
  strcpy(g.names[1], "West");
  strcpy(g.names[2], "North");
  strcpy(g.names[3], "East");
  htDeal(g);
  for (int i = 0; i < 4; i++) g.trick[i] = -1;
  g.passPhase = false;   // simplified: no passing
  // 2 of clubs leads
  g.lead = g.turn = 0;
  for (int p = 0; p < 4; p++) {
    for (int i = 0; i < HT_NHAND; i++) {
      if (g.hands[p][i] == ((2 - 2) << 2) | 3) {  // rank 2, clubs
        g.lead = g.turn = p;
      }
    }
  }
  g.sel = 0;
  g.phase = 0;
}

static int htCardPoints(int c) {
  if (cardSuit(c) == 1) return 1;                 // heart
  if (cardRank(c) == 12 && cardSuit(c) == 0) return 13;  // Q spades
  return 0;
}

// does card follow suit / is it legal?
static bool htLegal(HeartsG &g, int p, int c) {
  int leadSuit = -1;
  if (g.trick[g.lead] >= 0) leadSuit = cardSuit(g.trick[g.lead]);
  if (g.trick[g.lead] < 0) {
    // leading: cannot lead hearts unless broken or only hearts
    if (cardSuit(c) == 1 && !g.heartsBroken) {
      bool onlyHearts = true;
      for (int i = 0; i < g.nh[p]; i++)
        if (cardSuit(g.hands[p][i]) != 1) onlyHearts = false;
      if (!onlyHearts) return false;
    }
    return true;
  }
  if (cardSuit(c) == leadSuit) return true;
  // must follow suit if possible
  for (int i = 0; i < g.nh[p]; i++)
    if (cardSuit(g.hands[p][i]) == leadSuit) return false;
  // can't play hearts or Q-spades on first trick (simplified: allow)
  return true;
}

static void htPlayCard(HeartsG &g, int p, int idx) {
  int c = g.hands[p][idx];
  for (int i = idx; i < g.nh[p] - 1; i++) g.hands[p][i] = g.hands[p][i + 1];
  g.nh[p]--;
  if (cardSuit(c) == 1) g.heartsBroken = true;
  g.trick[p] = c;
}

static int htTrickWinner(HeartsG &g) {
  int leadSuit = cardSuit(g.trick[g.lead]);
  int best = g.lead;
  for (int p = 0; p < 4; p++) {
    if (p == g.lead) continue;
    if (cardSuit(g.trick[p]) == leadSuit &&
        cardRank(g.trick[p]) > cardRank(g.trick[best])) best = p;
  }
  return best;
}

// simple AI: legal low card, avoid taking points
static int htAIChoose(HeartsG &g, int p) {
  int best = -1;
  int bestScore = 1000000;
  for (int i = 0; i < g.nh[p]; i++) {
    if (!htLegal(g, p, g.hands[p][i])) continue;
    int c = g.hands[p][i];
    int score = cardRank(c) + htCardPoints(c) * 20;
    // if void in lead suit we already can't follow; prefer dumping QS/high hearts
    if (g.trick[g.lead] >= 0) {
      int leadSuit = cardSuit(g.trick[g.lead]);
      if (cardSuit(c) != leadSuit) score = 100 - cardRank(c) - htCardPoints(c) * -30;
    }
    if (score < bestScore) { bestScore = score; best = i; }
  }
  return best;  // -1 shouldn't happen (some card is always legal)
}

static void htDrawCard(int x, int y, int c, bool faceUp) {
  gfx->fillRect(x, y, 14, 18, faceUp ? RGB565(0xe8, 0xf0, 0xe8) : TERM_DIM);
  gfx->drawRect(x, y, 14, 18, TERM_GREEN);
  if (!faceUp) return;
  char r = "23456789TJQKA"[cardRank(c) - 2];
  char s = SUITS[cardSuit(c)];
  gfx->setTextColor(cardSuit(c) == 1 || cardSuit(c) == 2 ? TERM_RED : RGB565(0, 0, 0));
  gfx->setTextSize(1);
  gfx->setCursor(x + 3, y + 6);
  gfx->printf("%c%c", r, s);
}

static void htDraw(HeartsG &g, bool full) {
  char buf[40];
  if (full) {
    gfx->fillScreen(RGB565(0x0c, 0x14, 0x0c));
    gfx->setTextColor(TERM_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(4, 7); gfx->print("HEARTS");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("< > pick  SEL play  BK exit");
  }
  gfx->setTextSize(1);
  // opponents: show counts around table
  gfx->fillRect(0, 19, SCREEN_W, 62, RGB565(0x0c, 0x14, 0x0c));
  for (int p = 1; p < 4; p++) {
    int bx = p == 1 ? 4 : p == 2 ? 140 : 276;
    int by = 22;
    gfx->setTextColor(p == g.turn ? TERM_BRIGHT : TERM_DIM);
    gfx->setCursor(bx, by);
    snprintf(buf, sizeof(buf), "%s:%d", g.names[p], g.nh[p]);
    gfx->print(buf);
    gfx->setCursor(bx, by + 10);
    snprintf(buf, sizeof(buf), "pts %d", g.taken[p]);
    gfx->print(buf);
  }
  // trick area
  gfx->fillRect(0, 82, SCREEN_W, 56, RGB565(0x0c, 0x14, 0x0c));
  gfx->setTextColor(TERM_DIM);
  gfx->setCursor(4, 84);
  snprintf(buf, sizeof(buf), "Trick %d/13  broken:%s", g.round + 1,
           g.heartsBroken ? "Y" : "N");
  gfx->print(buf);
  int tx[4] = {140, 40, 140, 240};
  int ty[4] = {112, 96, 96, 96};
  for (int p = 0; p < 4; p++) {
    if (g.trick[p] >= 0) htDrawCard(tx[p], ty[p], g.trick[p], true);
    else if (p == g.turn) {
      gfx->setTextColor(TERM_BRIGHT);
      gfx->setCursor(tx[p] + 4, ty[p] + 6);
      gfx->print("...");
    }
  }
  // your points + hand
  gfx->fillRect(0, 140, SCREEN_W, 70, RGB565(0x0c, 0x14, 0x0c));
  gfx->setTextColor(TERM_GREEN);
  gfx->setCursor(4, 142);
  snprintf(buf, sizeof(buf), "You  pts %d", g.taken[0]);
  gfx->print(buf);
  int hx = 10;
  for (int i = 0; i < g.nh[0]; i++) {
    htDrawCard(hx + i * 16, 156, g.hands[0][i], true);
  }
  // selection highlight
  if (g.nh[0] > 0) {
    int sx = 10 + g.sel * 16;
    gfx->drawRect(sx - 1, 155, 16, 20, TERM_BRIGHT);
  }
}

static void htEndTrick(HeartsG &g) {
  int w = htTrickWinner(g);
  int pts = 0;
  for (int p = 0; p < 4; p++) pts += htCardPoints(g.trick[p]);
  g.taken[w] += pts;
  for (int p = 0; p < 4; p++) g.trick[p] = -1;
  g.lead = g.turn = w;
  g.round++;
  if (g.round >= 13) {
    g.phase = 1;
    // human wins the round if fewest points
    int min = g.taken[0];
    for (int p = 1; p < 4; p++) if (g.taken[p] < min) min = g.taken[p];
    gsRecordResult(GS_HEARTS, g.taken[0] == min);
  }
}

void heartsApp() {
  HeartsG g;
  htReset(g);
  htDraw(g, true);
  bool msg = false;
  char msgBuf[40] = "";
  while (true) {
    InputEventP e;
    bool have = pdaGetInput(e, 30);
    if (have) {
      if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT) return;
      if (g.phase == 1) {
        if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
            (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
          htReset(g); htDraw(g, true); msg = false;
        }
        continue;
      }
      if (e.ev == PDA_EV_LEFT)  g.sel = (g.sel + g.nh[0] - 1) % g.nh[0];
      if (e.ev == PDA_EV_RIGHT) g.sel = (g.sel + 1) % g.nh[0];
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        if (g.turn != 0) { strcpy(msgBuf, "Not your turn"); msg = true; }
        else if (!htLegal(g, 0, g.hands[0][g.sel])) {
          strcpy(msgBuf, "Illegal: follow suit"); msg = true;
        } else {
          htPlayCard(g, 0, g.sel);
          if (g.nh[0] > 0) g.sel = min(g.sel, g.nh[0] - 1);
          msg = false;
          g.turn = (g.turn + 1) % 4;
          htDraw(g, false);
          // AI plays
          while (g.turn != 0 && g.trick[0] < 0 ? g.turn != 0 : false) {}
          // (AI handled below)
        }
      }
    }
    // AI moves whenever it's their turn
    if (g.phase == 0) {
      while (g.turn != 0) {
        int idx = htAIChoose(g, g.turn);
        if (idx < 0) break;
        htPlayCard(g, g.turn, idx);
        bool trickFull = true;
        for (int p = 0; p < 4; p++) if (g.trick[p] < 0) trickFull = false;
        if (trickFull) {
          htDraw(g, false);
          delay(700);
          htEndTrick(g);
        } else {
          g.turn = (g.turn + 1) % 4;
        }
        htDraw(g, false);
      }
      // check trick completion after human play
      bool trickFull = true;
      for (int p = 0; p < 4; p++) if (g.trick[p] < 0) trickFull = false;
      if (trickFull && g.phase == 0) {
        htDraw(g, false);
        delay(700);
        htEndTrick(g);
        htDraw(g, false);
      }
    }
    if (g.phase == 1) {
      // round over: show scores
      htDraw(g, false);
      char buf[80];
      int min = g.taken[0];
      for (int p = 1; p < 4; p++) if (g.taken[p] < min) min = g.taken[p];
      snprintf(buf, sizeof(buf), "Round over  You %d  (best %d)  SEL new",
               g.taken[0], min);
      gfx->setTextColor(TERM_BRIGHT);
      gfx->setTextSize(1);
      gfx->setCursor(30, 210);
      gfx->fillRect(0, 205, SCREEN_W, 20, RGB565(0x0c, 0x14, 0x0c));
      gfx->setCursor(20, 210);
      gfx->print(buf);
      msg = false;
    }
    if (msg) {
      gfx->setTextColor(TERM_RED);
      gfx->setTextSize(1);
      gfx->fillRect(0, 205, SCREEN_W, 12, RGB565(0x0c, 0x14, 0x0c));
      gfx->setCursor(20, 207);
      gfx->print(msgBuf);
    }
  }
}

// ============================ Spades ============================
#define SP_NHAND 13

struct SpadesG {
  int hands[4][SP_NHAND];
  int nh[4];
  int trick[4];
  int lead, turn;
  int bids[4];           // tricks bid
  int tricksWon[4];
  int round;             // tricks completed this hand
  int sel;
  int phase;             // 0 play, 1 hand over
  bool spadesBroken;
  char names[4][8];
};

static void spDeal(SpadesG &g) {
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = i;
  for (int i = 51; i > 0; i--) {
    int j = random(0, i + 1);
    int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
  }
  for (int p = 0; p < 4; p++) {
    for (int i = 0; i < SP_NHAND; i++) g.hands[p][i] = deck[p * 13 + i];
    htSortHand(g.hands[p], SP_NHAND);
    g.nh[p] = SP_NHAND;
  }
}

static void spReset(SpadesG &g) {
  memset(&g, 0, sizeof(g));
  strcpy(g.names[0], "You");
  strcpy(g.names[1], "West");
  strcpy(g.names[2], "North");
  strcpy(g.names[3], "East");
  spDeal(g);
  for (int i = 0; i < 4; i++) { g.trick[i] = -1; g.tricksWon[i] = 0; }
  // simple bids: count spades + high cards
  for (int p = 0; p < 4; p++) {
    int bid = 0;
    for (int i = 0; i < SP_NHAND; i++) {
      int c = g.hands[p][i];
      if (cardSuit(c) == 0 && cardRank(c) >= 12) bid++;
      else if (cardRank(c) == 14) bid++;
    }
    g.bids[p] = max(1, min(bid, 13));
  }
  g.lead = g.turn = 0;   // dealer's left simplified: human leads first
  g.sel = 0;
  g.spadesBroken = false;
  g.phase = 0;
}

static bool spLegal(SpadesG &g, int p, int c) {
  int leadSuit = -1;
  if (g.trick[g.lead] >= 0) leadSuit = cardSuit(g.trick[g.lead]);
  if (g.trick[g.lead] < 0) {
    // cannot lead spades until broken (unless only spades)
    if (cardSuit(c) == 0 && !g.spadesBroken) {
      bool onlySpades = true;
      for (int i = 0; i < g.nh[p]; i++)
        if (cardSuit(g.hands[p][i]) != 0) onlySpades = false;
      if (!onlySpades) return false;
    }
    return true;
  }
  if (cardSuit(c) == leadSuit) return true;
  for (int i = 0; i < g.nh[p]; i++)
    if (cardSuit(g.hands[p][i]) == leadSuit) return false;
  return true;   // void: may play anything incl. spades (breaks them)
}

static void spPlayCard(SpadesG &g, int p, int idx) {
  int c = g.hands[p][idx];
  for (int i = idx; i < g.nh[p] - 1; i++) g.hands[p][i] = g.hands[p][i + 1];
  g.nh[p]--;
  if (cardSuit(c) == 0) g.spadesBroken = true;
  g.trick[p] = c;
}

static int spTrickWinner(SpadesG &g) {
  int leadSuit = cardSuit(g.trick[g.lead]);
  int best = g.lead;
  for (int p = 0; p < 4; p++) {
    if (p == g.lead) continue;
    int bs = cardSuit(g.trick[best]), bc = cardRank(g.trick[best]);
    int cs = cardSuit(g.trick[p]), rc = cardRank(g.trick[p]);
    if (cs == leadSuit && rc > (bs == leadSuit ? bc : 0)) best = p;
    else if (cs == 0 && (bs != 0 || rc > bc)) best = p;
  }
  return best;
}

static int spAIChoose(SpadesG &g, int p) {
  // try to win if partner (p+2)%4 currently winning & we can afford;
  // simplified: play lowest legal, spades only when void or to win
  int best = -1, bestScore = 1000000;
  for (int i = 0; i < g.nh[p]; i++) {
    int c = g.hands[p][i];
    if (!spLegal(g, p, c)) continue;
    int score = cardRank(c);
    if (cardSuit(c) == 0) score += 40;   // avoid burning spades
    if (score < bestScore) { bestScore = score; best = i; }
  }
  return best;
}

static void spDraw(SpadesG &g, bool full) {
  char buf[48];
  if (full) {
    gfx->fillScreen(RGB565(0x0c, 0x14, 0x0c));
    gfx->setTextColor(TERM_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(4, 7); gfx->print("SPADES");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("< > pick  SEL play  BK exit");
  }
  gfx->setTextSize(1);
  gfx->fillRect(0, 19, SCREEN_W, 62, RGB565(0x0c, 0x14, 0x0c));
  for (int p = 1; p < 4; p++) {
    int bx = p == 1 ? 4 : p == 2 ? 120 : 250;
    gfx->setTextColor(p == g.turn ? TERM_BRIGHT : TERM_DIM);
    gfx->setCursor(bx, 22);
    snprintf(buf, sizeof(buf), "%s bid %d got %d", g.names[p], g.bids[p], g.tricksWon[p]);
    gfx->print(buf);
  }
  gfx->fillRect(0, 82, SCREEN_W, 56, RGB565(0x0c, 0x14, 0x0c));
  gfx->setTextColor(TERM_DIM);
  gfx->setCursor(4, 84);
  snprintf(buf, sizeof(buf), "Trick %d/13  brk:%s  Y bid %d got %d",
           g.round + 1, g.spadesBroken ? "Y" : "N", g.bids[0], g.tricksWon[0]);
  gfx->print(buf);
  int tx[4] = {140, 40, 140, 240};
  int ty[4] = {112, 96, 96, 96};
  for (int p = 0; p < 4; p++) {
    if (g.trick[p] >= 0) htDrawCard(tx[p], ty[p], g.trick[p], true);
    else if (p == g.turn) {
      gfx->setTextColor(TERM_BRIGHT);
      gfx->setCursor(tx[p] + 4, ty[p] + 6);
      gfx->print("...");
    }
  }
  gfx->fillRect(0, 140, SCREEN_W, 70, RGB565(0x0c, 0x14, 0x0c));
  int hx = 10;
  for (int i = 0; i < g.nh[0]; i++)
    htDrawCard(hx + i * 16, 156, g.hands[0][i], true);
  if (g.nh[0] > 0) {
    int sx = 10 + g.sel * 16;
    gfx->drawRect(sx - 1, 155, 16, 20, TERM_BRIGHT);
  }
}

static void spEndTrick(SpadesG &g) {
  int w = spTrickWinner(g);
  g.tricksWon[w]++;
  for (int p = 0; p < 4; p++) g.trick[p] = -1;
  g.lead = g.turn = w;
  g.round++;
  if (g.round >= 13) {
    g.phase = 1;
    // team: you(0) + North(2) vs West(1) + East(3)
    int us = g.tricksWon[0] + g.tricksWon[2];
    int them = g.tricksWon[1] + g.tricksWon[3];
    int ourBid = g.bids[0] + g.bids[2];
    gsRecordResult(GS_SPADES, us >= ourBid && us > them);
  }
}

void spadesApp() {
  SpadesG g;
  spReset(g);
  spDraw(g, true);
  char msgBuf[40] = "";
  bool msg = false;
  while (true) {
    InputEventP e;
    bool have = pdaGetInput(e, 30);
    if (have) {
      if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT) return;
      if (g.phase == 1) {
        if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
            (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
          spReset(g); spDraw(g, true); msg = false;
        }
        continue;
      }
      if (e.ev == PDA_EV_LEFT)  g.sel = (g.sel + g.nh[0] - 1) % g.nh[0];
      if (e.ev == PDA_EV_RIGHT) g.sel = (g.sel + 1) % g.nh[0];
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        if (g.turn != 0) { strcpy(msgBuf, "Not your turn"); msg = true; }
        else if (!spLegal(g, 0, g.hands[0][g.sel])) {
          strcpy(msgBuf, "Illegal card"); msg = true;
        } else {
          spPlayCard(g, 0, g.sel);
          if (g.nh[0] > 0) g.sel = min(g.sel, g.nh[0] - 1);
          msg = false;
          g.turn = (g.turn + 1) % 4;
          spDraw(g, false);
        }
      }
    }
    if (g.phase == 0) {
      // AI moves
      bool progressed = false;
      while (g.turn != 0 && g.phase == 0) {
        int idx = spAIChoose(g, g.turn);
        if (idx < 0) break;
        spPlayCard(g, g.turn, idx);
        bool trickFull = true;
        for (int p = 0; p < 4; p++) if (g.trick[p] < 0) trickFull = false;
        if (trickFull) {
          spDraw(g, false);
          delay(600);
          spEndTrick(g);
        } else {
          g.turn = (g.turn + 1) % 4;
        }
        spDraw(g, false);
        progressed = true;
      }
      // trick completion after human play
      bool trickFull = true;
      for (int p = 0; p < 4; p++) if (g.trick[p] < 0) trickFull = false;
      if (trickFull && g.phase == 0) {
        spDraw(g, false);
        delay(600);
        spEndTrick(g);
        spDraw(g, false);
      }
    }
    if (g.phase == 1) {
      spDraw(g, false);
      gfx->fillRect(0, 205, SCREEN_W, 20, RGB565(0x0c, 0x14, 0x0c));
      gfx->setTextColor(TERM_BRIGHT);
      gfx->setTextSize(1);
      gfx->setCursor(20, 210);
      int us = g.tricksWon[0] + g.tricksWon[2];
      int ourBid = g.bids[0] + g.bids[2];
      gfx->printf("Hand over: %d/%d bid  SEL new", us, ourBid);
      msg = false;
    }
    if (msg) {
      gfx->setTextColor(TERM_RED);
      gfx->setTextSize(1);
      gfx->fillRect(0, 205, SCREEN_W, 12, RGB565(0x0c, 0x14, 0x0c));
      gfx->setCursor(20, 207);
      gfx->print(msgBuf);
    }
  }
}

// ============================ Backgammon ============================
// Points numbered 0..23 from AI's perspective; player (you) moves from
// point 23 down to 0 and bears off at <0. AI moves 0 up to >23.
#define BG_PIP 15

struct Backgammon {
  int pts[24];       // + = player checkers, - = AI checkers
  int barP, barA;    // checkers on bar
  int homeP, homeA;  // borne off counts
  int dice[2];
  int used[2];       // dice used flags
  int selPoint;      // selected point to move from (-1 = none)
  int phase;         // 0 play, 1 game over
  bool playerTurn;
};

static void bgSetup(Backgammon &g) {
  memset(g.pts, 0, sizeof(g.pts));
  // Player (positive), moving 23 -> 0, bears off <0:
  g.pts[23] = 2; g.pts[18] = 5; g.pts[16] = 3; g.pts[12] = 5;
  // AI (negative), mirrored, moving 0 -> 23, bears off >23:
  g.pts[0] = -2; g.pts[5] = -5; g.pts[7] = -3; g.pts[11] = -5;
  g.barP = g.barA = 0;
  g.homeP = g.homeA = 0;
}

static void bgRoll(Backgammon &g) {
  g.dice[0] = random(1, 7);
  g.dice[1] = random(1, 7);
  g.used[0] = g.used[1] = 0;
}

static bool bgPlayerHome(Backgammon &g) {
  // all player checkers in points 18..23 and none on bar
  if (g.barP > 0) return false;
  for (int p = 0; p < 18; p++)
    if (g.pts[p] > 0) return false;
  return true;
}

// can player move from 'from' by 'die'? to = from - die
static bool bgCanMove(Backgammon &g, int from, int die) {
  if (g.barP > 0) {
    // must enter from bar: enter point = 24 - die
    int to = 24 - die;
    if (g.pts[to] >= -1) return from == 24;   // 24 = bar code
  }
  if (from < 0 || from > 23 || g.pts[from] <= 0) return false;
  int to = from - die;
  if (to >= 0) return g.pts[to] >= -1;
  // bearing off: only from exact or highest point when all in home
  if (!bgPlayerHome(g)) return false;
  if (from - die == -1) return true;   // exact roll
  // die larger than point: allow if no checkers on higher points
  for (int p = from + 1; p < 24; p++)
    if (g.pts[p] > 0) return false;
  return true;
}

static void bgApplyMove(Backgammon &g, int from, int die) {
  int to = from - die;
  bool fromBar = (from == 24);
  if (fromBar) {
    g.barP--;
    from = 24 - die;
    to = from;
  }
  if (to >= 0) {
    if (g.pts[to] == -1) { g.pts[to] = 0; g.barA++; }   // hit AI blot
    g.pts[from]--;
    g.pts[to]++;
  } else {
    g.pts[from]--;
    g.homeP++;
  }
  for (int d = 0; d < 2; d++)
    if (!g.used[d] && g.dice[d] == die) { g.used[d] = 1; break; }
}

// does player have any legal move with remaining dice?
static bool bgAnyMove(Backgammon &g) {
  int rem[2], n = 0;
  for (int d = 0; d < 2; d++) if (!g.used[d]) rem[n++] = g.dice[d];
  if (g.dice[0] == g.dice[1]) {
    // doubles: 4 moves total; used flags approximate (2 dice slots)
    // treat as 2 here (AI same); acceptable simplification
  }
  for (int i = 0; i < n; i++) {
    int die = rem[i];
    if (g.barP > 0) { if (bgCanMove(g, 24, die)) return true; continue; }
    for (int p = 0; p < 24; p++)
      if (g.pts[p] > 0 && bgCanMove(g, p, die)) return true;
  }
  return false;
}

static void bgDraw(Backgammon &g, bool full) {
  char buf[64];
  if (full) {
    gfx->fillScreen(RGB565(0x0c, 0x14, 0x0c));
    gfx->setTextColor(TERM_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(4, 7); gfx->print("BACKGAMMON");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->print("< > sel point  SEL move w/die  BK exit");
  }
  gfx->setTextSize(1);
  // dice display
  gfx->fillRect(0, 20, SCREEN_W, 14, RGB565(0x0c, 0x14, 0x0c));
  gfx->setTextColor(g.playerTurn ? TERM_BRIGHT : TERM_DIM);
  gfx->setCursor(4, 22);
  snprintf(buf, sizeof(buf), "%s  dice %d %d%s   off Y%d A%d bar Y%d A%d",
           g.playerTurn ? "YOU" : "AI", g.dice[0], g.dice[1],
           (g.used[0] || g.used[1]) ? "" : "",
           g.homeP, g.homeA, g.barP, g.barA);
  gfx->print(buf);
  // board: draw 24 points as 2 rows of 12 (left half 0-11, right 12-23)
  gfx->fillRect(0, 36, SCREEN_W, SCREEN_H - 50, RGB565(0x0c, 0x14, 0x0c));
  // points 0-11 bottom half? Layout: top row = points 12-23 (player home right)
  for (int p = 0; p < 24; p++) {
    int col = p % 12;
    int px = 8 + col * 26;
    bool top = p >= 12;
    int py = top ? 40 : SCREEN_H - 70;
    int dir = top ? 1 : -1;
    int n = g.pts[p];
    int cnt = abs(n);
    uint16_t colr = n > 0 ? TERM_BRIGHT : TERM_RED;
    // triangle
    for (int i = 0; i < 10; i++) {
      gfx->drawFastHLine(px + 5 - i, py + (top ? i : 0), 10 + 2 * i,
                        (p == g.selPoint) ? TERM_GREEN : TERM_DIM);
    }
    // checkers stacked on triangle tip
    for (int c = 0; c < min(cnt, 5); c++) {
      int cy = top ? py + 10 + c * 7 : py - 7 - c * 7;
      gfx->fillCircle(px + 5, cy, 3, colr);
    }
    if (cnt > 5) {
      gfx->setTextColor(colr);
      gfx->setCursor(px, top ? py + 45 : py - 48);
      gfx->printf("%d", cnt);
    }
    gfx->setTextColor(TERM_DIM);
    gfx->setCursor(px + 2, top ? py - 9 : py + 12);
    gfx->printf("%d", p);
  }
  // bar (middle)
  gfx->drawFastVLine(160, 36, SCREEN_H - 50, TERM_GREEN);
  if (g.barP > 0) {
    gfx->fillCircle(165, 60, 4, TERM_BRIGHT);
    gfx->setTextColor(TERM_BRIGHT);
    gfx->setCursor(172, 57);
    gfx->printf("x%d", g.barP);
  }
  if (g.barA > 0) {
    gfx->fillCircle(165, SCREEN_H - 66, 4, TERM_RED);
    gfx->setTextColor(TERM_RED);
    gfx->setCursor(172, SCREEN_H - 69);
    gfx->printf("x%d", g.barA);
  }
}

static void bgAIMove(Backgammon &g) {
  // simple AI: for each remaining die, make first legal move toward 23
  for (int pass = 0; pass < 2; pass++) {
    for (int d = 0; d < 2; d++) {
      if (g.used[d]) continue;
      int die = g.dice[d];
      bool moved = false;
      if (g.barA > 0) {
        int to = die - 1;   // AI enters at point die-1
        if (to >= 0 && to < 24 && g.pts[to] <= 1) {
          if (g.pts[to] == 1) { g.pts[to] = 0; g.barP++; }
          g.barA--;
          g.pts[to] -= 1;
          g.used[d] = 1;
          moved = true;
        }
      } else {
        // prefer moves that don't leave blots; simple: first legal from low points
        for (int p = 0; p < 24 && !moved; p++) {
          if (g.pts[p] >= 0) continue;
          int to = p + die;
          if (to > 23) {
            // bear off: all AI checkers home (points 0-5)
            bool home = true;
            for (int q = 6; q < 24; q++) if (g.pts[q] < 0) home = false;
            if (home && p - 6 <= die) {   // exact or overshoot from highest
              bool higher = false;
              for (int q = p + 1; q < 6; q++) if (g.pts[q] < 0) higher = true;
              if (p + die - 1 == 23 || !higher) {
                g.pts[p]++;
                g.homeA++;
                g.used[d] = 1;
                moved = true;
              }
            }
          } else if (g.pts[to] <= 1) {
            if (g.pts[to] == 1) { g.pts[to] = 0; g.barP++; }
            g.pts[p]++;
            g.pts[to]--;
            g.used[d] = 1;
            moved = true;
          }
        }
      }
      if (moved) bgDraw(g, false);
    }
  }
}

void backgammonApp() {
  Backgammon g;
  bgSetup(g);
  bgRoll(g);
  g.playerTurn = true;
  g.selPoint = 23;
  g.phase = 0;
  bgDraw(g, true);
  char msgBuf[40] = "";
  int msgTimer = 0;
  while (true) {
    InputEventP e;
    bool have = pdaGetInput(e, 40);
    if (have) {
      if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT) return;
      if (g.phase == 1) {
        if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
            (e.ev == PDA_EV_CHAR && (e.ch == 'n' || e.ch == 'N'))) {
          bgSetup(g); bgRoll(g); g.playerTurn = true; g.selPoint = 23;
          g.phase = 0; bgDraw(g, true);
        }
        continue;
      }
      if (!g.playerTurn) continue;
      if (e.ev == PDA_EV_LEFT)  g.selPoint = (g.selPoint + 23) % 24;
      if (e.ev == PDA_EV_RIGHT) g.selPoint = (g.selPoint + 1) % 24;
      if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
        // move from selPoint with first unused die
        int dUse = -1;
        for (int d = 0; d < 2; d++)
          if (!g.used[d] && bgCanMove(g, g.barP > 0 ? 24 : g.selPoint, g.dice[d])) {
            dUse = d; break;
          }
        if (dUse < 0) {
          strcpy(msgBuf, "No legal move w/ dice");
          msgTimer = 40;
        } else {
          int from = g.barP > 0 ? 24 : g.selPoint;
          bgApplyMove(g, from, g.dice[dUse]);
          bgDraw(g, false);
          // win check
          if (g.homeP >= 15) {
            g.phase = 1;
            gsRecordResult(GS_BACKGAMMON, true);
          } else if (!bgAnyMove(g)) {
            // no more moves: pass turn
            g.playerTurn = false;
            bgRoll(g);
            bgDraw(g, false);
            delay(400);
            bgAIMove(g);
            if (g.homeA >= 15) {
              g.phase = 1;
              gsRecordResult(GS_BACKGAMMON, false);
            } else {
              g.playerTurn = true;
              bgDraw(g, false);
            }
          }
        }
      }
      if (g.barP > 0 && e.ev != PDA_EV_SELECT) {
        g.selPoint = 24;   // bar indicator
      }
    }
    if (msgTimer > 0) {
      msgTimer--;
      gfx->setTextColor(TERM_RED);
      gfx->setTextSize(1);
      gfx->setCursor(60, 22);
      gfx->print(msgBuf);
    } else if (msgTimer == 0 && msgBuf[0]) {
      msgBuf[0] = 0;
      bgDraw(g, false);
    }
    if (g.phase == 1) {
      bgDraw(g, false);
      gfx->fillRect(0, 205, SCREEN_W, 20, RGB565(0x0c, 0x14, 0x0c));
      gfx->setTextColor(TERM_BRIGHT);
      gfx->setTextSize(1);
      gfx->setCursor(30, 210);
      gfx->printf("%s wins!  SEL new", g.homeP >= 15 ? "You" : "AI");
    }
  }
}
