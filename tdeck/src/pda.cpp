/**
 * PDA extras: to-do list, battery monitor, clock, calendar, screen sleep.
 * All apps follow the main.cpp event loop pattern (getInput + drawTitle).
 */
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <sys/time.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include "utilities.h"
#include "pda.h"
#include "theme.h"
#include "settings.h"

extern Arduino_GFX *gfx;
extern QueueHandle_t inputQueue;
extern bool sdOk;


// ---------- Battery (ADC on GPIO4, 2:1 divider per T-Deck schematic) ----------
#define BAT_ADC_PIN BOARD_BAT_ADC
#define BAT_VREF 3.3
#define BAT_DIVIDER 2.0
#define BAT_FULL_MV 4200
#define BAT_EMPTY_MV 3300

static int batteryReadMv() {
  // Average a few fast reads for stability (no delay() — this runs in the
  // title bar of every screen)
  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delayMicroseconds(200);
  }
  return (int)((sum / 4) / 4095.0 * BAT_VREF * BAT_DIVIDER * 1000);
}

int batteryPercent() {
  int mv = batteryReadMv();
  if (mv <= BAT_EMPTY_MV) return 0;
  if (mv >= BAT_FULL_MV) return 100;
  return (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
}

int batteryMillivolts() {
  return batteryReadMv();
}

// ---------- Screen sleep ----------
static bool screenAwake = true;
static uint32_t lastActivityMs = 0;

void pdaNoteActivity() { lastActivityMs = millis(); }

bool pdaScreenTick() {  // call ~1/s; returns true if state changed
  unsigned long timeout = settingsSleepMs();
  bool wantAwake = (timeout == 0) || (millis() - lastActivityMs) < timeout;
  if (wantAwake != screenAwake) {
    screenAwake = wantAwake;
    digitalWrite(BOARD_TFT_BACKLIGHT, screenAwake ? HIGH : LOW);
    kbSetBacklight(screenAwake ? 128 : 0);   // keyboard backlight follows screen
    return true;
  }
  return false;
}

bool pdaScreenAwake() { return screenAwake; }

// Wake on any input: the input tasks call this before queueing events.
void pdaWake() {
  if (!screenAwake) {
    screenAwake = true;
    digitalWrite(BOARD_TFT_BACKLIGHT, HIGH);
  }
  lastActivityMs = millis();
}

// ---------- Clock ----------
// Time source priority: GPS (if fixed) -> compile time fallback.
// GPS time is applied by pdaApplyGpsTime() from the map/GPS poll loop.

static bool timeSynced = false;
static const char *pdaTz = "EST5EDT,M3.2.0,M11.1.0";   // default: US Eastern

static long pdaDaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static time_t pdaTimegm(const struct tm *t) {
  return (time_t)pdaDaysFromCivil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday) * 86400L
       + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

void pdaSetTimezone(const char *posixTz) {
  if (!posixTz || !posixTz[0]) return;
  pdaTz = posixTz;
  setenv("TZ", pdaTz, 1);
  tzset();
}

const char *pdaGetTimezone() { return pdaTz; }
void pdaInitTimezone() { pdaSetTimezone(pdaTz); }

void pdaApplyGpsTime(int year, int month, int day, int hour, int minute, int second) {
  if (year < 2020 || month < 1 || month > 12) return;
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  // GPS reports UTC; convert as UTC so the TZ offset applies on read
  time_t tt = pdaTimegm(&t);
  if (tt > 1600000000) {  // sanity
    struct timeval tv = { .tv_sec = tt, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    timeSynced = true;
  }
}

bool pdaTimeSynced() { return timeSynced; }

// ---------- Clock / Stopwatch / Timer ----------
static void beep(int ms) {
  // Simple square-wave beep via the I2S speaker path in main.cpp
  extern void spkBeep(int ms);
  spkBeep(ms);
}

void clockApp() {
  enum Mode { MODE_CLOCK, MODE_STOPWATCH, MODE_TIMER, MODE_COUNT };
  static int mode = MODE_CLOCK;
  bool needsRedraw = true;

  // stopwatch state
  static uint32_t swStartMs = 0;
  static bool swRunning = false;
  static uint32_t swElapsedMs = 0;

  // timer state
  static int timerSetSec = 300;   // default 5 min
  static uint32_t timerEndMs = 0;
  static bool timerRunning = false;
  bool timerEditing = false;

  const char *modeNames[] = {"Clock", "Stopwatch", "Timer"};

  while (true) {
    uint32_t nowMs = millis();
    // timer expiry
    if (timerRunning && timerEndMs != 0 && (int32_t)(nowMs - timerEndMs) >= 0) {
      timerRunning = false;
      timerEndMs = 0;
      beep(800);
      needsRedraw = true;
    }
    // 1Hz update in clock mode; 10Hz while stopwatch/timer run
    static uint32_t lastUpd = 0;
    uint32_t updInterval = (mode != MODE_CLOCK && (swRunning || timerRunning)) ? 100 : 500;
    if (nowMs - lastUpd >= updInterval) { lastUpd = nowMs; needsRedraw = true; }

    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      // mode tabs
      gfx->setTextSize(1);
      for (int m = 0; m < MODE_COUNT; m++) {
        int x = 8 + m * 70;
        if (m == mode) {
          gfx->fillRect(x, 0, 66, 16, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else {
          gfx->setTextColor(TERM_DIM, BLACK);
        }
        gfx->setCursor(x + 8, 4);
        gfx->print(modeNames[m]);
      }

      if (mode == MODE_CLOCK) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        char big[16], date[32];
        strftime(big, sizeof(big), "%H:%M", &lt);
        strftime(date, sizeof(date), "%a %b %d %Y", &lt);
        gfx->setTextSize(4);
        gfx->setTextColor(TERM_GREEN, BLACK);
        gfx->setCursor(40, 80);
        gfx->print(big);
        char secs[8];
        strftime(secs, sizeof(secs), ":%S", &lt);
        gfx->setTextSize(2);
        gfx->setTextColor(RGB565(120, 120, 120), BLACK);
        gfx->setCursor(230, 96);
        gfx->print(secs);
        gfx->setTextSize(2);
        gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(60, 140);
        gfx->print(date);
        gfx->setTextSize(1);
        gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(60, 170);
        gfx->print(timeSynced ? "time: GPS" : "time: not synced (need GPS fix)");
      } else if (mode == MODE_STOPWATCH) {
        uint32_t el = swElapsedMs + (swRunning ? (nowMs - swStartMs) : 0);
        uint32_t cs = (el / 10) % 100;
        uint32_t s = (el / 1000) % 60;
        uint32_t m = (el / 60000) % 60;
        uint32_t h = el / 3600000;
        gfx->setTextSize(4);
        gfx->setTextColor(TERM_GREEN, BLACK);
        gfx->setCursor(30, 90);
        gfx->printf("%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
        gfx->setTextSize(2);
        gfx->setTextColor(RGB565(120, 120, 120), BLACK);
        gfx->setCursor(262, 106);
        gfx->printf(".%02u", (unsigned)cs);
        gfx->setTextSize(1);
        gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(8, 170);
        gfx->print(swRunning ? "Click = stop" : (swElapsedMs ? "Click = resume   r = reset" : "Click = start"));
      } else {  // TIMER
        int remain = timerRunning ? (int)((timerEndMs - nowMs) / 1000) + 1 : timerSetSec;
        if (remain < 0) remain = 0;
        if (!timerRunning && timerEditing) {
          gfx->setTextSize(4);
          gfx->setTextColor(TERM_ACCENT, BLACK);
          gfx->setCursor(60, 90);
          gfx->printf("%02d:%02d", timerSetSec / 60, timerSetSec % 60);
          gfx->setTextSize(1);
          gfx->setTextColor(WHITE, BLACK);
          gfx->setCursor(8, 170);
          gfx->print("Edit: +/- min  u/d +10s  Click = start");
        } else {
          gfx->setTextSize(4);
          gfx->setTextColor(timerRunning ? RGB565(255, 120, 0) : TERM_GREEN, BLACK);
          gfx->setCursor(60, 90);
          gfx->printf("%02d:%02d", remain / 60, remain % 60);
          gfx->setTextSize(1);
          gfx->setTextColor(WHITE, BLACK);
          gfx->setCursor(8, 170);
          gfx->print(timerRunning ? "Click = stop   long = cancel" : "Click = edit/start");
        }
      }
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 12);
      gfx->print("U/D = mode  Long-click = back");
    }

    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    switch (e.ev) {
      case PDA_EV_UP:
        if (mode == MODE_TIMER && timerEditing) {
          timerSetSec += 10; needsRedraw = true;
        } else { mode = (mode + MODE_COUNT - 1) % MODE_COUNT; needsRedraw = true; }
        break;
      case PDA_EV_DOWN:
        if (mode == MODE_TIMER && timerEditing) {
          if (timerSetSec >= 20) timerSetSec -= 10;
          needsRedraw = true;
        } else { mode = (mode + 1) % MODE_COUNT; needsRedraw = true; }
        break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE:
        if (mode == MODE_STOPWATCH) {
          if (swRunning) { swElapsedMs += millis() - swStartMs; swRunning = false; }
          else { swStartMs = millis(); swRunning = true; }
          needsRedraw = true;
        } else if (mode == MODE_TIMER) {
          if (timerRunning) { timerRunning = false; timerEndMs = 0; }
          else if (timerEditing) {
            timerEditing = false;
            if (timerSetSec > 0) { timerEndMs = millis() + (uint32_t)timerSetSec * 1000; timerRunning = true; }
          } else {
            timerEditing = true;
          }
          needsRedraw = true;
        }
        break;
      case PDA_EV_CHAR:
        if (mode == MODE_STOPWATCH && (e.ch == 'r' || e.ch == 'R') && !swRunning) {
          swElapsedMs = 0; needsRedraw = true;
        } else if (mode == MODE_TIMER && timerEditing) {
          if (e.ch == '+' || e.ch == '=') { timerSetSec += 60; needsRedraw = true; }
          else if (e.ch == '-' && timerSetSec >= 60) { timerSetSec -= 60; needsRedraw = true; }
        }
        break;
      case PDA_EV_LONGSELECT:
        if (mode == MODE_TIMER && timerRunning) {
          timerRunning = false; timerEndMs = 0; needsRedraw = true;
        } else {
          return;
        }
        break;
      case PDA_EV_BACK:
      case PDA_EV_LEFT:
        return;
      default: break;
    }
  }
}

// ---------- Calendar (month view) ----------
void calendarApp() {
  static int viewYear = -1, viewMonth = -1;
  // Day cursor + view, initialized to today so trackball movement is by day.
  int selDay = 0;
  {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    viewYear = lt.tm_year + 1900;
    viewMonth = lt.tm_mon + 1;
    selDay = lt.tm_mday;
  }
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->printf("%s %d", months[viewMonth - 1], viewYear);

      const char *dows[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      for (int i = 0; i < 7; i++) {
        gfx->setCursor(24 + i * 42, 30);
        gfx->print(dows[i]);
      }
      static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
      int days = dim[viewMonth - 1];
      bool leap = (viewYear % 4 == 0 && viewYear % 100 != 0) || viewYear % 400 == 0;
      if (viewMonth == 2 && leap) days = 29;
      struct tm first = {};
      first.tm_year = viewYear - 1900;
      first.tm_mon = viewMonth - 1;
      first.tm_mday = 1;
      mktime(&first);
      int startDow = first.tm_wday;
      int todayD = -1, todayM = -1, todayY = -1;
      time_t now = time(NULL);
      struct tm lt;
      localtime_r(&now, &lt);
      todayD = lt.tm_mday; todayM = lt.tm_mon + 1; todayY = lt.tm_year + 1900;

      gfx->setTextSize(1);
      for (int d = 1; d <= days; d++) {
        int cell = startDow + d - 1;
        int row = cell / 7, col = cell % 7;
        int x = 18 + col * 42, y = 42 + row * 22;
        bool isToday = (d == todayD && viewMonth == todayM && viewYear == todayY);
        bool isSel = (d == selDay);
        int nDue = todoCountForDate(viewYear, viewMonth, d);
        if (isSel) {
          gfx->fillRect(x, y - 2, 38, 18, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else if (isToday) {
          gfx->drawRect(x, y - 2, 38, 18, TERM_GREEN);
          gfx->setTextColor(TERM_GREEN, BLACK);
        } else {
          gfx->setTextColor(WHITE, BLACK);
        }
        gfx->setCursor(x + 4, y + 2);
        gfx->print(d);
        if (nDue > 0) {
          gfx->fillCircle(x + 30, y + 12, 2, isSel ? BLACK : RGB565(255, 200, 0));
        }
      }
      // Task list for selected day (below grid, y from 180)
      gfx->setTextSize(1);
      if (selDay > 0) {
        static TodoTask tasks[TODO_MAX_TASKS];
        int n = todoLoadTasks(tasks, TODO_MAX_TASKS);
        int shown = 0;
        gfx->setTextColor(TERM_ACCENT, BLACK);
        gfx->setCursor(8, 180);
        gfx->printf("%d %s:", selDay, months[viewMonth - 1]);
        for (int i = 0; i < n && shown < 3; i++) {
          if (tasks[i].dueYear == viewYear && tasks[i].dueMonth == viewMonth &&
              tasks[i].dueDay == selDay) {
            gfx->setCursor(8, 192 + shown * 12);
            gfx->setTextColor(tasks[i].done ? RGB565(120, 120, 120) : WHITE, BLACK);
            gfx->print(tasks[i].done ? "[x] " : "[ ] ");
            for (int c = 0; c < TODO_MAX_LEN && tasks[i].text[c]; c++) {
              gfx->print(tasks[i].text[c]);
              if (c >= 46) break;   // keep text inside the 320px screen
            }
            shown++;
          }
        }
        if (shown == 0) {
          gfx->setCursor(8, 192);
          gfx->setTextColor(TERM_DIM, BLACK);
          gfx->print("(no tasks)");
        }
      }
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 12);
      gfx->print("TB=day l/r=week m/p=mo y/Y=yr t=today");
    }
    InputEventP e;
    if (!pdaGetInput(e, 100)) continue;
    // helpers for month navigation
    auto daysIn = [&](int y, int m) {
      static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
      int d = dim[m - 1];
      bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
      if (m == 2 && leap) d = 29;
      return d;
    };
    auto prevMonth = [&]() {
      viewMonth--; if (viewMonth < 1) { viewMonth = 12; viewYear--; }
    };
    auto nextMonth = [&]() {
      viewMonth++; if (viewMonth > 12) { viewMonth = 1; viewYear++; }
    };
    auto jumpDay = [&](int delta) {   // move cursor by days, crossing months
      selDay += delta;
      while (selDay < 1) {
        prevMonth();
        selDay += daysIn(viewYear, viewMonth);
      }
      while (selDay > daysIn(viewYear, viewMonth)) {
        selDay -= daysIn(viewYear, viewMonth);
        nextMonth();
      }
      needsRedraw = true;
    };
    switch (e.ev) {
      case PDA_EV_UP: jumpDay(-1); break;
      case PDA_EV_DOWN: jumpDay(1); break;
      case PDA_EV_LEFT: jumpDay(-7); break;
      case PDA_EV_RIGHT: jumpDay(7); break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        viewYear = lt.tm_year + 1900;
        viewMonth = lt.tm_mon + 1;
        selDay = lt.tm_mday;
        needsRedraw = true;
        break;
      }
      case PDA_EV_CHAR:
        if (e.ch == 'm') {
          nextMonth();
          if (selDay > daysIn(viewYear, viewMonth)) selDay = daysIn(viewYear, viewMonth);
          needsRedraw = true;
        } else if (e.ch == 'p') {
          prevMonth();
          if (selDay > daysIn(viewYear, viewMonth)) selDay = daysIn(viewYear, viewMonth);
          needsRedraw = true;
        }
        else if (e.ch == 'y') { viewYear++; needsRedraw = true; }
        else if (e.ch == 'Y') { viewYear--; needsRedraw = true; }
        else if (e.ch == 't') {
          time_t now = time(NULL);
          struct tm lt;
          localtime_r(&now, &lt);
          viewYear = lt.tm_year + 1900;
          viewMonth = lt.tm_mon + 1;
          selDay = lt.tm_mday;
          needsRedraw = true;
        }
        else if (e.ch == 'w') { jumpDay(7); }
        else if (e.ch == 'b') { jumpDay(-7); }
        else if (e.ch == '+' || e.ch == '=') { jumpDay(1); }
        else if (e.ch == '-') { jumpDay(-1); }
        // clamp cursor into new month range if month/year changed
        {
          int dim = daysIn(viewYear, viewMonth);
          if (selDay > dim) selDay = dim;
          if (selDay < 1) selDay = 1;
        }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK:
        return;
      default: break;
    }
  }
}

// ---------- To-do list ----------
// Stored as a plain text file on SD: one task per line, "[x] " prefix = done.
#define TODO_FILE "/todo/todo.txt"

// ---------- Shared to-do storage ----------
// Line format: "[ ] text @YYYY-MM-DD" (date optional)
int todoLoadTasks(TodoTask *tasks, int maxN) {
  int n = 0;
  if (!sdOk || !SD.exists(TODO_FILE)) return 0;
  File f = SD.open(TODO_FILE, FILE_READ);
  if (!f) return 0;
  while (f.available() && n < maxN) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    bool isDone = line.startsWith("[x] ");
    String text = line.substring(isDone ? 4 : 0);
    int dueY = 0, dueM = 0, dueD = 0;
    int at = text.indexOf(" @");
    if (at > 0 && text.length() - at >= 11) {
      String dateStr = text.substring(at + 2, at + 12);
      if (dateStr.length() == 10 && dateStr[4] == '-' && dateStr[7] == '-') {
        dueY = dateStr.substring(0, 4).toInt();
        dueM = dateStr.substring(5, 7).toInt();
        dueD = dateStr.substring(8, 10).toInt();
        if (dueY >= 2020 && dueM >= 1 && dueM <= 12 && dueD >= 1 && dueD <= 31) {
          text = text.substring(0, at);
        } else { dueY = dueM = dueD = 0; }
      }
    }
    text.trim();
    if (text.length() == 0) continue;
    strncpy(tasks[n].text, text.c_str(), TODO_MAX_LEN - 1);
    tasks[n].text[TODO_MAX_LEN - 1] = 0;
    tasks[n].done = isDone;
    tasks[n].dueYear = dueY; tasks[n].dueMonth = dueM; tasks[n].dueDay = dueD;
    n++;
  }
  f.close();
  return n;
}

bool todoSaveTasks(TodoTask *tasks, int n) {
  if (!sdOk) return false;
  if (!SD.exists("/todo")) SD.mkdir("/todo");
  File f = SD.open(TODO_FILE, FILE_WRITE);
  if (!f) return false;
  for (int i = 0; i < n; i++) {
    f.print(tasks[i].done ? "[x] " : "[ ] ");
    f.print(tasks[i].text);
    if (tasks[i].dueYear > 0) {
      f.printf(" @%04d-%02d-%02d", tasks[i].dueYear, tasks[i].dueMonth, tasks[i].dueDay);
    }
    f.print('\n');
  }
  f.close();
  return true;
}

int todoCountForDate(int y, int m, int d) {
  static TodoTask tasks[TODO_MAX_TASKS];
  int n = todoLoadTasks(tasks, TODO_MAX_TASKS);
  int c = 0;
  for (int i = 0; i < n; i++) {
    if (tasks[i].dueYear == y && tasks[i].dueMonth == m && tasks[i].dueDay == d) c++;
  }
  return c;
}

// ---------- To-do due-date picker ----------
static bool todoPickDateGrid(int &y0, int &m0, int &d0) {
  int y = y0, m = m0, d = d0;
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  if (y == 0) { y = lt.tm_year + 1900; m = lt.tm_mon + 1; d = lt.tm_mday; }
  const char *mon[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                         "Jul","Aug","Sep","Oct","Nov","Dec"};
  auto dimOf = [](int yy, int mm) {
    if (mm == 2) return (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 29 : 28;
    return (mm == 4 || mm == 6 || mm == 9 || mm == 11) ? 30 : 31;
  };
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 6);
      gfx->printf("Pick date  %s %d", mon[m - 1], y);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 30);
      gfx->print("Su Mo Tu We Th Fr Sa");
      int dim = dimOf(y, m);
      struct tm first = {};
      first.tm_year = y - 1900; first.tm_mon = m - 1; first.tm_mday = 1;
      first.tm_isdst = -1;
      mktime(&first);
      int startCol = first.tm_wday;
      for (int day = 1; day <= dim; day++) {
        int col = (startCol + day - 1) % 7;
        int row = (startCol + day - 1) / 7;
        int x = 8 + col * 24, yy = 44 + row * 20;
        if (day == d) {
          gfx->fillRect(x - 2, yy - 2, 22, 16, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(x, yy);
        gfx->printf("%2d", day);
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("Arrows=move +/-=month click=ok Long=cancel");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    int dim = dimOf(y, m);
    if (e.ev == PDA_EV_LEFT)  { if (d > 1) d--; else { m--; if (m < 1) { m = 12; y--; } d = dimOf(y, m); } needsRedraw = true; }
    else if (e.ev == PDA_EV_RIGHT) { if (d < dim) d++; else { m++; if (m > 12) { m = 1; y++; } d = 1; } needsRedraw = true; }
    else if (e.ev == PDA_EV_UP)    { if (d > 7) d -= 7; else d = 1; needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN)  { d += 7; if (d > dim) d = dim; needsRedraw = true; }
    else if (e.ev == PDA_EV_CHAR && (e.ch == '+' || e.ch == '=')) { m++; if (m > 12) { m = 1; y++; } if (d > dimOf(y, m)) d = dimOf(y, m); needsRedraw = true; }
    else if (e.ev == PDA_EV_CHAR && e.ch == '-') { m--; if (m < 1) { m = 12; y--; } if (d > dimOf(y, m)) d = dimOf(y, m); needsRedraw = true; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) { y0 = y; m0 = m; d0 = d; return true; }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return false;
  }
}

static void todoPickDueDate(TodoTask &t) {
  const char *const opts[] = {
    "Today", "Tomorrow", "In 3 days", "Next week", "In 2 weeks",
    "In a month", "Pick date...", "Clear date"
  };
  const int NOPTS = 8;
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  if (lt.tm_year + 1900 < 2020) { lt.tm_year = 100; lt.tm_mon = 0; lt.tm_mday = 1; lt.tm_hour = 12; }
  int sel = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 6);
      gfx->print("Due date");
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 28);
      gfx->print(t.text);
      for (int i = 0; i < NOPTS; i++) {
        int y = 46 + i * 20;
        if (i == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 18, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(TERM_BRIGHT, BLACK);
        gfx->setCursor(10, y);
        gfx->print(opts[i]);
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("u/d=pick click=set Long=cancel");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_DOWN && sel < NOPTS - 1) { sel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (sel == 7) {  // clear date
        t.dueYear = t.dueMonth = t.dueDay = 0;
        return;
      }
      if (sel == 6) {  // calendar picker
        int py = t.dueYear, pm = t.dueMonth, pd = t.dueDay;
        if (py == 0) { py = lt.tm_year + 1900; pm = lt.tm_mon + 1; pd = lt.tm_mday; }
        if (todoPickDateGrid(py, pm, pd)) {
          t.dueYear = py; t.dueMonth = pm; t.dueDay = pd;
        }
        return;
      }
      static const int addDays[6] = {0, 1, 3, 7, 14, 30};
      struct tm tmp = lt;
      tmp.tm_mday += addDays[sel];
      tmp.tm_hour = 12;
      time_t tt = mktime(&tmp);
      localtime_r(&tt, &tmp);
      t.dueYear = tmp.tm_year + 1900;
      t.dueMonth = tmp.tm_mon + 1;
      t.dueDay = tmp.tm_mday;
      return;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

void todoApp() {
  static TodoTask tasks[TODO_MAX_TASKS];
  int nTasks = todoLoadTasks(tasks, TODO_MAX_TASKS);
  bool dirty = false;
  int sel = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print("To-do");
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 20);
      gfx->print("Click=done n=new d=del t=due date Long=back");
      const int visible = 8;
      int top = 0;
      if (nTasks > visible && sel >= visible) top = sel - visible + 1;
      for (int i = 0; i < visible && top + i < nTasks; i++) {
        int y = 34 + i * 22;
        int idx = top + i;
        if (idx == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 20, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else {
          gfx->setTextColor(WHITE, BLACK);
        }
        gfx->setTextSize(1);
        gfx->setCursor(8, y);
        gfx->print(tasks[idx].done ? "[x] " : "[ ] ");
        gfx->print(tasks[idx].text);
        if (tasks[idx].dueYear > 0) {
          gfx->printf(" @%04d-%02d-%02d", tasks[idx].dueYear, tasks[idx].dueMonth, tasks[idx].dueDay);
        }
      }
      if (nTasks == 0) {
        gfx->setTextSize(1);
        gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(8, 34);
        gfx->print("(no tasks - press n)");
      }
    }
    InputEventP e;
    if (!pdaGetInput(e, 100)) continue;
    switch (e.ev) {
      case PDA_EV_UP: if (sel > 0) sel--; needsRedraw = true; break;
      case PDA_EV_DOWN: if (sel < nTasks - 1) sel++; needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE:
        if (nTasks > 0) { tasks[sel].done = !tasks[sel].done; dirty = true; needsRedraw = true; }
        break;
      case PDA_EV_CHAR:
        if (e.ch == 'n' || e.ch == 'N') {
          char task[TODO_MAX_LEN] = {0};
          int tl = 0;
          gfx->fillRect(0, SCREEN_H - 40, SCREEN_W, 20, BLACK);
          gfx->setTextSize(1);
          gfx->setTextColor(TERM_ACCENT, BLACK);
          gfx->setCursor(8, SCREEN_H - 36);
          gfx->print("Task: ");
          bool collecting = true;
          while (collecting) {
            InputEventP ke;
            if (!pdaGetInput(ke, 50)) continue;
            if (ke.ev == PDA_EV_NEWLINE || ke.ev == PDA_EV_SELECT) collecting = false;
            else if (ke.ev == PDA_EV_CHAR && tl < TODO_MAX_LEN - 1) {
              task[tl++] = ke.ch;
              gfx->print(ke.ch);
            } else if (ke.ev == PDA_EV_SPACE && tl < TODO_MAX_LEN - 1) {
              task[tl++] = ' ';
              gfx->print(' ');
            } else if (ke.ev == PDA_EV_DELETE && tl > 0) {
              tl--;
              int cx = 8 + 6 * 6 + tl * 6;
              gfx->fillRect(cx, SCREEN_H - 36, 6, 10, BLACK);
            } else if (ke.ev == PDA_EV_LONGSELECT || ke.ev == PDA_EV_BACK) {
              collecting = false; tl = 0;
            }
          }
          if (tl > 0 && nTasks < TODO_MAX_TASKS) {
            memcpy(tasks[nTasks].text, task, tl);
            tasks[nTasks].text[tl] = 0;
            tasks[nTasks].done = false;
            tasks[nTasks].dueYear = tasks[nTasks].dueMonth = tasks[nTasks].dueDay = 0;
            nTasks++;
            sel = nTasks - 1;
            dirty = true;
          }
          needsRedraw = true;
        } else if (e.ch == 'd' || e.ch == 'D') {
          if (nTasks > 0) {
            for (int i = sel; i < nTasks - 1; i++) tasks[i] = tasks[i + 1];
            nTasks--;
            if (sel >= nTasks && sel > 0) sel--;
            dirty = true;
            needsRedraw = true;
          }
        } else if (e.ch == 't' || e.ch == 'T') {
          if (nTasks > 0) {
            todoPickDueDate(tasks[sel]);
            dirty = true;
            needsRedraw = true;
          }
        }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK:
      case PDA_EV_LEFT:
        goto exit_save;
      default: break;
    }
  }
exit_save:
  if (dirty) todoSaveTasks(tasks, nTasks);
}

