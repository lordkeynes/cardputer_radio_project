/** * Sports app: live + past scores from ESPN's public site API. * (site.api.espn.com — no API key required.) * * League picker -> daily scoreboard (left/right = prev/next day) -> * game detail with per-quarter/period line scores and a box-score table. * * JSON is parsed streaming through an ArduinoJson filter so full ESPN * payloads (which can be hundreds of KB) are never buffered in RAM. */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "sports.h"

extern Arduino_GFX *gfx;

// If the clock has never been set (no GPS fix), sync from NTP over WiFi so
// 'today' means the actual today for the scoreboard.
bool wifiAutoConnect();

static void spEnsureClock() {
  pdaInitTimezone();   // apply saved TZ so 'today' matches the user's day
  time_t now = time(NULL);
  if (now > 1700000000) return;   // clock already sane
  if (WiFi.status() != WL_CONNECTED) {
    wifiAutoConnect();   // boot auto-connect may not have finished
    if (WiFi.status() != WL_CONNECTED) return;
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // wait up to 4 s for NTP
  for (int i = 0; i < 40 && time(NULL) < 1700000000; i++) delay(100);
}

// sport/league path segments for the ESPN site API
struct League { const char *name; const char *path; };
static const League leagues[] = {
  {"NFL",     "football/nfl"},
  {"NCAAF",   "football/college-football"},
  {"NBA",     "basketball/nba"},
  {"WNBA",    "basketball/wnba"},
  {"NCAAM",   "basketball/mens-college-basketball"},
  {"MLB",     "baseball/mlb"},
  {"NHL",     "hockey/nhl"},
  {"EPL",     "soccer/eng.1"},
  {"LaLiga",  "soccer/esp.1"},
  {"SerieA",  "soccer/ita.1"},
  {"Bundes",  "soccer/ger.1"},
  {"Ligue1",  "soccer/fra.1"},
  {"MLS",     "soccer/usa.1"},
  {"UCL",     "soccer/uefa.champions"},
  {"MMA",     "mma/mma"},
};
#define N_LEAGUES (int)(sizeof(leagues)/sizeof(leagues[0]))

#define SP_MAX_GAMES 16
#define SP_MAX_PERIODS 12
#define SP_MAX_STATS 14

struct SpGame {
  char id[32];
  char away[14], home[14];
  int awayScore, homeScore;
  char status[28];
};

struct SpBoxRow { char name[20]; char away[12]; char home[12]; };

struct SpDetail {
  char away[14], home[14];
  int awayScore, homeScore;
  char status[28];
  int awayPer[SP_MAX_PERIODS], homePer[SP_MAX_PERIODS];
  int nPeriods;
  char periodLabel[SP_MAX_PERIODS][4];
  SpBoxRow rows[SP_MAX_STATS];
  int nRows;
  bool ok;
};

static void spDateStr(long offsetDays, char *out, size_t n) {
  time_t now = time(NULL) + offsetDays * 86400L;
  struct tm lt;
  localtime_r(&now, &lt);
  snprintf(out, n, "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static void spDateCompact(long offsetDays, char *out, size_t n) {
  time_t now = time(NULL) + offsetDays * 86400L;
  struct tm lt;
  localtime_r(&now, &lt);
  snprintf(out, n, "%04d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

// ---------- scoreboard fetch ----------
// events[].competitions[].competitors[] + status; sizes kept small via filter
static int spFetchScoreboard(const char *path, const char *dateCompact,
                             SpGame *games, int maxGames) {
  if (WiFi.status() != WL_CONNECTED) {
    wifiAutoConnect();
    if (WiFi.status() != WL_CONNECTED) return -1;
  }
  char url[160];
  snprintf(url, sizeof(url),
           "https://site.api.espn.com/apis/site/v2/sports/%s/scoreboard?dates=%s",
           path, dateCompact);

  JsonDocument filter;
  filter["events"][0]["id"] = true;
  filter["events"][0]["status"]["type"]["shortDetail"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["abbreviation"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["displayName"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["score"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["homeAway"] = true;

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("tdeck-pda/1.0");
  http.setTimeout(9000);
  int code = http.GET();
  int n = 0;
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err =
      deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
    if (!err) {
      ok = true;
      JsonArray evs = doc["events"].as<JsonArray>();
      if (!evs.isNull()) {
        for (JsonObject ev : evs) {
          if (n >= maxGames) break;
          SpGame &g = games[n];
          strlcpy(g.id, ev["id"] | "", sizeof(g.id));
          strlcpy(g.status, ev["status"]["type"]["shortDetail"] | "?", sizeof(g.status));
          g.awayScore = g.homeScore = 0;
          strlcpy(g.away, "?", sizeof(g.away));
          strlcpy(g.home, "?", sizeof(g.home));
          JsonArray comps = ev["competitions"].as<JsonArray>();
          if (comps.size() > 0) {
            JsonArray teams = comps[0]["competitors"].as<JsonArray>();
            for (JsonObject c : teams) {
              const char *dn = c["team"]["displayName"] | "";
              const char *ab = c["team"]["abbreviation"] | "?";
              if (!dn || !dn[0]) dn = ab;
              int score = String((const char*)(c["score"] | "0")).toInt();
              bool isHome = String((const char*)(c["homeAway"] | "away")) == "home";
              if (isHome) { strlcpy(g.home, dn, sizeof(g.home)); g.homeScore = score; }
              else        { strlcpy(g.away, dn, sizeof(g.away)); g.awayScore = score; }
            }
          }
          n++;
        }
      }
    }
  }
  http.end();
  return ok ? n : -1;
}

// count games (+ live count) for a league/date without keeping payloads;
// returns -1 on network error
static int spCountGames(const char *path, const char *dateCompact, int &nLiveOut) {
  nLiveOut = 0;
  if (WiFi.status() != WL_CONNECTED) {
    wifiAutoConnect();
    if (WiFi.status() != WL_CONNECTED) return -1;
  }
  char url[160];
  snprintf(url, sizeof(url),
           "https://site.api.espn.com/apis/site/v2/sports/%s/scoreboard?dates=%s",
           path, dateCompact);
  JsonDocument filter;
  filter["events"][0]["status"]["type"]["state"] = true;
  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("tdeck-pda/1.0");
  http.setTimeout(6000);
  int code = http.GET();
  int n = 0;
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err =
      deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
    if (!err) {
      ok = true;
      JsonArray evs = doc["events"].as<JsonArray>();
      if (!evs.isNull()) {
        for (JsonObject ev : evs) {
          n++;
          const char *st = ev["status"]["type"]["state"] | "";
          if (st && strcmp(st, "in") == 0) nLiveOut++;
        }
      }
    }
  }
  http.end();
  return ok ? n : -1;
}

// background fetch of today's per-league game/live counts so the league
// picker opens instantly and fills counts in as they arrive
static volatile int spCounts[N_LEAGUES];
static volatile int spLive[N_LEAGUES];
static volatile uint32_t spCountsGen = 0;
static volatile bool spCountsBusy = false;

static void spCountsTask(void *pv) {
  char dateCompact[12];
  spDateCompact(0, dateCompact, sizeof(dateCompact));
  for (int i = 0; i < N_LEAGUES; i++) {
    int live = 0;
    spCounts[i] = spCountGames(leagues[i].path, dateCompact, live);
    spLive[i] = live;
    spCountsGen++;
  }
  spCountsBusy = false;
  vTaskDelete(NULL);
}

// ---------- summary / box score fetch ----------
static void spFetchDetail(const char *path, const char *eventId, SpDetail &d) {
  memset(&d, 0, sizeof(d));
  d.ok = false;
  if (WiFi.status() != WL_CONNECTED) {
    wifiAutoConnect();
    if (WiFi.status() != WL_CONNECTED) return;
  }
  char url[192];
  snprintf(url, sizeof(url),
           "https://site.api.espn.com/apis/site/v2/sports/%s/summary?event=%s",
           path, eventId);

  JsonDocument filter;
  // header: line scores per period + scores/status
  filter["header"]["competitions"][0]["status"]["type"]["shortDetail"] = true;
  filter["header"]["competitions"][0]["competitors"][0]["team"]["abbreviation"] = true;
  filter["header"]["competitions"][0]["competitors"][0]["team"]["displayName"] = true;
  filter["header"]["competitions"][0]["competitors"][0]["score"] = true;
  filter["header"]["competitions"][0]["competitors"][0]["homeAway"] = true;
  filter["header"]["competitions"][0]["competitors"][0]["linescores"][0]["value"] = true;
  // box score: team statistics
  filter["boxscore"]["teams"][0]["team"]["abbreviation"] = true;
  filter["boxscore"]["teams"][0]["statistics"][0]["displayName"] = true;
  filter["boxscore"]["teams"][0]["statistics"][0]["displayValue"] = true;

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("tdeck-pda/1.0");
  http.setTimeout(9000);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err =
      deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
    if (err) { http.end(); return; }

    strlcpy(d.status, "final", sizeof(d.status));
    JsonArray comps = doc["header"]["competitions"].as<JsonArray>();
    if (comps.size() > 0) {
      const char *sd = comps[0]["status"]["type"]["shortDetail"];
      if (sd) strlcpy(d.status, sd, sizeof(d.status));
      JsonArray teams = comps[0]["competitors"].as<JsonArray>();
      for (JsonObject c : teams) {
        const char *dn = c["team"]["displayName"] | "";
        const char *ab = c["team"]["abbreviation"] | "?";
        if (!dn || !dn[0]) dn = ab;
        int score = String((const char*)(c["score"] | "0")).toInt();
        bool isHome = String((const char*)(c["homeAway"] | "away")) == "home";
        if (isHome) { strlcpy(d.home, dn, sizeof(d.home)); d.homeScore = score; }
        else        { strlcpy(d.away, dn, sizeof(d.away)); d.awayScore = score; }
        int *dst = isHome ? d.homePer : d.awayPer;
        JsonArray ls = c["linescores"].as<JsonArray>();
        int cnt = 0;
        for (JsonObject p : ls) {
          if (cnt >= SP_MAX_PERIODS) break;
          dst[cnt] = (int)(p["value"] | 0);
          cnt++;
        }
        if (cnt > d.nPeriods) d.nPeriods = cnt;
      }
    }
    for (int i = 0; i < d.nPeriods; i++)
      snprintf(d.periodLabel[i], sizeof(d.periodLabel[i]),
               i < 4 ? "%d" : i == 4 ? "OT" : "%dOT", i < 4 ? i + 1 : i - 3);

    // box score rows
    JsonArray bt = doc["boxscore"]["teams"].as<JsonArray>();
    if (bt.size() >= 2) {
      JsonArray a = bt[0]["statistics"].as<JsonArray>();
      JsonArray b = bt[1]["statistics"].as<JsonArray>();
      if (a.isNull() || b.isNull() || a.size() == 0 || a.size() != b.size()) {
        // fall back: use whichever exists (won't align; skip)
      } else {
        int rows = a.size() < SP_MAX_STATS ? (int)a.size() : SP_MAX_STATS;
        for (int i = 0; i < rows; i++) {
          SpBoxRow &r = d.rows[d.nRows];
          strlcpy(r.name, a[i]["displayName"] | "?", sizeof(r.name));
          strlcpy(r.away, a[i]["displayValue"] | "-", sizeof(r.away));
          strlcpy(r.home, b[i]["displayValue"] | "-", sizeof(r.home));
          d.nRows++;
        }
        // boxscore teams order can differ from header order; align by abbr
        const char *ab0 = bt[0]["team"]["abbreviation"] | "";
        if (strcmp(ab0, d.home) == 0 && strcmp(d.away, d.home) != 0) {
          for (int i = 0; i < d.nRows; i++) {
            char tmp[12];
            strlcpy(tmp, d.rows[i].away, sizeof(tmp));
            strlcpy(d.rows[i].away, d.rows[i].home, sizeof(d.rows[i].away));
            strlcpy(d.rows[i].home, tmp, sizeof(d.rows[i].home));
          }
        }
      }
    }
    d.ok = true;
  }
  http.end();
}

// ---------- screens ----------
static void spDrawMsg(const char *l1, const char *l2) {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(10, 60);
  gfx->print(l1);
  gfx->setCursor(10, 76);
  gfx->print(l2);
  InputEventP e;
  unsigned long t0 = millis();
  while (millis() - t0 < 2500) { if (pdaGetInput(e, 50)) break; }
}

static int spLeaguePicker(int startSel) {
  int sel = startSel;
  bool needsRedraw = true;
  // kick off a background refresh of counts (never blocks the UI)
  if (WiFi.status() != WL_CONNECTED) wifiAutoConnect();
  if (WiFi.status() == WL_CONNECTED && !spCountsBusy) {
    for (int i = 0; i < N_LEAGUES; i++) { spCounts[i] = -2; spLive[i] = 0; }
    spCountsGen = 0;
    spCountsBusy = true;
    xTaskCreate(spCountsTask, "spcnt", 10240, NULL, 1, NULL);
  }
  uint32_t lastGen = spCountsGen;
  while (true) {
    if (spCountsGen != lastGen) { lastGen = spCountsGen; needsRedraw = true; }
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 6);
      gfx->print("Sports");
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("u/d=pick click=go Long=back");
      const int visible = 8;
      int top = 0;
      if (sel >= visible) top = sel - visible + 1;
      for (int i = 0; i < visible && top + i < N_LEAGUES; i++) {
        int y = 30 + i * 20;
        if (top + i == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 18, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, y);
        gfx->print(leagues[top + i].name);
        // right column: games today / live games
        char cb[16];
        int cnt = spCounts[top + i];
        if (cnt == -2) snprintf(cb, sizeof(cb), " ..");
        else if (cnt < 0) snprintf(cb, sizeof(cb), " --");
        else if (spLive[top + i] > 0) snprintf(cb, sizeof(cb), " %d (%d live)", cnt, spLive[top + i]);
        else snprintf(cb, sizeof(cb), " %d gm", cnt);
        int cw = strlen(cb) * 6;
        gfx->setCursor(SCREEN_W - 8 - cw, y);
        gfx->print(cb);
      }
    }
    InputEventP e;
    if (!pdaGetInput(e, 20)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN && sel < N_LEAGUES - 1) { sel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) return sel;
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return -1;
  }
}

static void spGameDetail(const char *path, SpGame &g) {
  spDrawMsg("Fetching box score...", g.away);
  SpDetail d;
  spFetchDetail(path, g.id, d);
  if (!d.ok) {
    spDrawMsg("Could not fetch detail.", "Check WiFi connection.");
    return;
  }
  if (d.away[0] == 0) strlcpy(d.away, g.away, sizeof(d.away));
  if (d.home[0] == 0) strlcpy(d.home, g.home, sizeof(d.home));

  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 6);
      gfx->printf("%s %d - %s %d", d.away, d.awayScore, d.home, d.homeScore);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_ACCENT, BLACK);
      gfx->setCursor(8, 24);
      gfx->print(d.status);

      int y = 40;
      // line scores table (quarters / periods / innings)
      if (d.nPeriods > 0) {
        gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(8, y);
        gfx->print("    TOT");
        for (int p = 0; p < d.nPeriods && p < 8; p++) {
          gfx->setCursor(52 + p * 22, y);
          gfx->print(d.periodLabel[p]);
        }
        y += 14;
        gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(8, y);
        gfx->printf("%-4s%4d", d.away, d.awayScore);
        for (int p = 0; p < d.nPeriods && p < 8; p++) {
          gfx->setCursor(52 + p * 22, y);
          gfx->printf("%d", d.awayPer[p]);
        }
        y += 14;
        gfx->setCursor(8, y);
        gfx->printf("%-4s%4d", d.home, d.homeScore);
        for (int p = 0; p < d.nPeriods && p < 8; p++) {
          gfx->setCursor(52 + p * 22, y);
          gfx->printf("%d", d.homePer[p]);
        }
        y += 22;
      }
      // box score
      if (d.nRows > 0) {
        gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(8, y);
        gfx->print("Stat                away  home");
        y += 13;
        gfx->setTextColor(TERM_BRIGHT, BLACK);
        for (int i = 0; i < d.nRows && y < SCREEN_H - 16; i++) {
          gfx->setCursor(8, y);
          gfx->printf("%-19s %-5s %-5s", d.rows[i].name, d.rows[i].away, d.rows[i].home);
          y += 13;
        }
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("click=back  Long=back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE ||
        e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

void sportsApp() {
  spEnsureClock();
  int leagueSel = -1;
  while (true) {
    leagueSel = spLeaguePicker(leagueSel < 0 ? 0 : leagueSel);
    if (leagueSel < 0) return;   // backed out
    const League &L = leagues[leagueSel];

    long dayOffset = 0;
    int sel = 0, nGames = 0;
    static SpGame games[SP_MAX_GAMES];
    char dateCompact[12], dateStr[16];
    bool fetched = false, fetchErr = false;
    bool needsRedraw = true;

    while (true) {
      if (!fetched && !fetchErr) {
        spDateCompact(dayOffset, dateCompact, sizeof(dateCompact));
        spDrawMsg("Fetching scoreboard...", dateCompact);
        nGames = spFetchScoreboard(L.path, dateCompact, games, SP_MAX_GAMES);
        fetched = true;
        if (nGames < 0) fetchErr = true;
        needsRedraw = true;
      }
      if (needsRedraw) {
        needsRedraw = false;
        spDateStr(dayOffset, dateStr, sizeof(dateStr));
        gfx->fillScreen(BLACK);
        gfx->setTextSize(2);
        gfx->setTextColor(TERM_GREEN, BLACK);
        gfx->setCursor(8, 6);
        gfx->print(L.name);
        gfx->setTextSize(1);
        gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(200, 10);
        gfx->print(dateStr);
        gfx->setCursor(4, SCREEN_H - 20);
        gfx->print("L/R=+-day u/d=game click=box Long=leagues");
        int y = 34;
        if (fetchErr) {
          gfx->setTextColor(TERM_RED, BLACK);
          gfx->setCursor(10, y);
          gfx->print("WiFi not connected.");
          gfx->setTextColor(TERM_DIM, BLACK);
          gfx->setCursor(10, y + 16);
          gfx->print("Connect via Network > WiFi first.");
        } else if (nGames == 0) {
          gfx->setTextColor(TERM_DIM, BLACK);
          gfx->setCursor(10, y);
          gfx->print("No games on this date.");
        } else {
          const int visible = 8;
          int top = 0;
          if (sel >= visible) top = sel - visible + 1;
          for (int i = 0; i < visible && top + i < nGames; i++) {
            int yy = y + i * 20;
            if (top + i == sel) {
              gfx->fillRect(0, yy - 2, SCREEN_W, 18, TERM_SEL_BG);
              gfx->setTextColor(BLACK, TERM_SEL_BG);
            } else gfx->setTextColor(TERM_BRIGHT, BLACK);
            gfx->setCursor(10, yy);
            gfx->printf("%-13s%2d %-13s%2d %s",
                        games[top + i].away, games[top + i].awayScore,
                        games[top + i].home, games[top + i].homeScore,
                        games[top + i].status);
          }
        }
      }
      InputEventP e;
      if (!pdaGetInput(e, 50)) continue;
      if (e.ev == PDA_EV_LEFT)  { dayOffset--; fetched = false; sel = 0; needsRedraw = true; }
      else if (e.ev == PDA_EV_RIGHT) { dayOffset++; fetched = false; sel = 0; needsRedraw = true; }
      else if (e.ev == PDA_EV_UP && sel > 0) { sel--; needsRedraw = true; }
      else if (e.ev == PDA_EV_DOWN && sel < nGames - 1) { sel++; needsRedraw = true; }
      else if ((e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) && nGames > 0) {
        spGameDetail(L.path, games[sel]);
        needsRedraw = true;
      }
      else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) break;  // back to leagues
    }
  }
}
