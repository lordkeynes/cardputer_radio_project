/**
 * PDA extras: to-do list, battery monitor, clock, calendar, screen sleep.
 * All apps follow the main.cpp event loop pattern (getInput + drawTitle).
 */
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "utilities.h"
#include "pda.h"

extern Arduino_GFX *gfx;
extern QueueHandle_t inputQueue;
extern bool sdOk;


// ---------- Battery (ADC on GPIO4, 2:1 divider per T-Deck schematic) ----------
#define BAT_ADC_PIN BOARD_BAT_ADC
#define BAT_VREF 3.3
#define BAT_DIVIDER 2.0
#define BAT_FULL_MV 4200
#define BAT_EMPTY_MV 3300

int batteryPercent() {
  // Average a few reads for stability
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delay(2);
  }
  int mv = (int)((sum / 8) / 4095.0 * BAT_VREF * BAT_DIVIDER * 1000);
  if (mv <= BAT_EMPTY_MV) return 0;
  if (mv >= BAT_FULL_MV) return 100;
  return (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
}

int batteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delay(2);
  }
  return (int)((sum / 8) / 4095.0 * BAT_VREF * BAT_DIVIDER * 1000);
}

// ---------- Screen sleep ----------
static bool screenAwake = true;
static uint32_t lastActivityMs = 0;

void pdaNoteActivity() { lastActivityMs = millis(); }

bool pdaScreenTick() {  // call ~1/s; returns true if state changed
  bool wantAwake = (millis() - lastActivityMs) < SCREEN_SLEEP_MS;
  if (wantAwake != screenAwake) {
    screenAwake = wantAwake;
    digitalWrite(BOARD_TFT_BACKLIGHT, screenAwake ? HIGH : LOW);
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

void pdaApplyGpsTime(int year, int month, int day, int hour, int minute, int second) {
  if (year < 2020 || month < 1 || month > 12) return;
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  time_t tt = mktime(&t);
  if (tt > 1600000000) {  // sanity
    struct timeval tv = { .tv_sec = tt, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    timeSynced = true;
  }
}

bool pdaTimeSynced() { return timeSynced; }

void clockApp() {
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      time_t now = time(NULL);
      struct tm lt;
      localtime_r(&now, &lt);
      char big[16], date[32];
      strftime(big, sizeof(big), "%H:%M", &lt);
      strftime(date, sizeof(date), "%a %b %d %Y", &lt);
      gfx->setTextSize(4);
      gfx->setTextColor(RGB565(0, 255, 160), BLACK);
      gfx->setCursor(40, 70);
      gfx->print(big);
      char secs[8];
      strftime(secs, sizeof(secs), ":%S", &lt);
      gfx->setTextSize(2);
      gfx->setTextColor(RGB565(120, 120, 120), BLACK);
      gfx->setCursor(230, 86);
      gfx->print(secs);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(60, 130);
      gfx->print(date);
      gfx->setTextSize(1);
      gfx->setTextColor(RGB565(150, 150, 150), BLACK);
      gfx->setCursor(60, 160);
      gfx->print(timeSynced ? "time: GPS" : "time: not synced (need GPS fix)");
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->setTextColor(WHITE, BLACK);
      gfx->print("Any key/click = back");
    }
    // Update once a second
    static time_t lastShown = 0;
    time_t now = time(NULL);
    if (now != lastShown) { lastShown = now; needsRedraw = true; }

    InputEventP e;
    if (!pdaGetInput(e, 200)) continue;
    return;  // any input exits clock
  }
}

// ---------- Calendar (month view) ----------
void calendarApp() {
  static int viewYear = -1, viewMonth = -1;  // remember last view
  if (viewYear < 0) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    viewYear = lt.tm_year + 1900;
    viewMonth = lt.tm_mon + 1;
  }
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};
      gfx->setTextSize(2);
      gfx->setTextColor(RGB565(0, 255, 160), BLACK);
      gfx->setCursor(90, 8);
      gfx->printf("%s %d", months[viewMonth - 1], viewYear);

      const char *dows[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
      gfx->setTextSize(1);
      gfx->setTextColor(RGB565(150, 150, 150), BLACK);
      for (int i = 0; i < 7; i++) {
        gfx->setCursor(18 + i * 44, 32);
        gfx->print(dows[i]);
      }
      // days in month
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

      gfx->setTextSize(2);
      for (int d = 1; d <= days; d++) {
        int cell = startDow + d - 1;
        int row = cell / 7, col = cell % 7;
        int x = 10 + col * 44, y = 44 + row * 30;
        bool isToday = (d == todayD && viewMonth == todayM && viewYear == todayY);
        if (isToday) {
          gfx->fillRect(x, y - 2, 40, 26, RGB565(0, 120, 255));
          gfx->setTextColor(BLACK, RGB565(0, 120, 255));
        } else {
          gfx->setTextColor(WHITE, BLACK);
        }
        gfx->setCursor(x + 6, y + 4);
        gfx->print(d);
      }
      gfx->setTextSize(1);
      gfx->setTextColor(RGB565(150, 150, 150), BLACK);
      gfx->setCursor(4, SCREEN_H - 22);
      gfx->print("Up/Down = month  Click/Enter = today  Long-click = back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 100)) continue;
    switch (e.ev) {
      case PDA_EV_UP: viewMonth--; if (viewMonth < 1) { viewMonth = 12; viewYear--; } needsRedraw = true; break;
      case PDA_EV_DOWN: viewMonth++; if (viewMonth > 12) { viewMonth = 1; viewYear++; } needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        viewYear = lt.tm_year + 1900;
        viewMonth = lt.tm_mon + 1;
        needsRedraw = true;
        break;
      }
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK:
      case PDA_EV_LEFT:
        return;
      default: break;
    }
  }
}

// ---------- To-do list ----------
// Stored as a plain text file on SD: one task per line, "[x] " prefix = done.
#define TODO_FILE "/todo/todo.txt"

void todoApp() {
  static char todoBuf[4096];
  bool dirty = false;
  // Load
  int len = 0;
  if (sdOk && SD.exists(TODO_FILE)) {
    File f = SD.open(TODO_FILE, FILE_READ);
    if (f) {
      len = f.read((uint8_t *)todoBuf, sizeof(todoBuf) - 1);
      f.close();
    }
  }
  todoBuf[len] = 0;

  // Parse into lines
  enum { MAX_TASKS = 32, MAX_TASK_LEN = 60 };
  static char tasks[MAX_TASKS][MAX_TASK_LEN];
  bool done[MAX_TASKS];
  int nTasks = 0;
  int pos = 0;
  while (pos < len && nTasks < MAX_TASKS) {
    char *lineEnd = strchr(todoBuf + pos, '\n');
    int lineLen = lineEnd ? (lineEnd - (todoBuf + pos)) : (len - pos);
    if (lineLen > 0) {
      bool isDone = (lineLen >= 4 && strncmp(todoBuf + pos, "[x] ", 4) == 0);
      done[nTasks] = isDone;
      int start = pos + (isDone ? 4 : 0);
      int copyLen = lineLen - (isDone ? 4 : 0);
      if (copyLen >= MAX_TASK_LEN) copyLen = MAX_TASK_LEN - 1;
      memcpy(tasks[nTasks], todoBuf + start, copyLen);
      tasks[nTasks][copyLen] = 0;
      nTasks++;
    }
    pos += lineLen + (lineEnd ? 1 : 0);
  }

  int sel = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(RGB565(0, 255, 160), BLACK);
      gfx->setCursor(8, 8);
      gfx->print("To-do");
      gfx->setTextSize(1);
      gfx->setTextColor(RGB565(150, 150, 150), BLACK);
      gfx->setCursor(4, SCREEN_H - 20);
      gfx->print("Click = toggle done  n = new task  d = delete  Long-click = back");
      const int visible = 8;
      int top = 0;
      if (nTasks > visible && sel >= visible) top = sel - visible + 1;
      for (int i = 0; i < visible && top + i < nTasks; i++) {
        int y = 34 + i * 22;
        int idx = top + i;
        if (idx == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 20, RGB565(0, 120, 255));
          gfx->setTextColor(BLACK, RGB565(0, 120, 255));
        } else {
          gfx->setTextColor(WHITE, BLACK);
        }
        gfx->setTextSize(1);
        gfx->setCursor(8, y);
        gfx->print(done[idx] ? "[x] " : "[ ] ");
        gfx->print(tasks[idx]);
      }
      if (nTasks == 0) {
        gfx->setTextSize(1);
        gfx->setTextColor(RGB565(150, 150, 150), BLACK);
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
        if (nTasks > 0) { done[sel] = !done[sel]; dirty = true; needsRedraw = true; }
        break;
      case PDA_EV_CHAR:
        if (e.ch == 'n' || e.ch == 'N') {
          // Append new task via keyboard capture
          char task[MAX_TASK_LEN] = {0};
          int tl = 0;
          gfx->fillRect(0, SCREEN_H - 40, SCREEN_W, 20, BLACK);
          gfx->setTextSize(1);
          gfx->setTextColor(RGB565(255, 255, 0), BLACK);
          gfx->setCursor(8, SCREEN_H - 36);
          gfx->print("Task: ");
          bool collecting = true;
          while (collecting) {
            InputEventP ke;
            if (!pdaGetInput(ke, 50)) continue;
            if (ke.ev == PDA_EV_NEWLINE || ke.ev == PDA_EV_SELECT) collecting = false;
            else if (ke.ev == PDA_EV_CHAR && tl < MAX_TASK_LEN - 1) {
              task[tl++] = ke.ch;
              gfx->print(ke.ch);
            } else if (ke.ev == PDA_EV_DELETE && tl > 0) {
              tl--;
              gfx->print('\b');
            } else if (ke.ev == PDA_EV_LONGSELECT || ke.ev == PDA_EV_BACK) {
              collecting = false; tl = 0;
            }
          }
          if (tl > 0 && nTasks < MAX_TASKS) {
            memcpy(tasks[nTasks], task, tl);
            tasks[nTasks][tl] = 0;
            done[nTasks] = false;
            nTasks++;
            sel = nTasks - 1;
            dirty = true;
          }
          needsRedraw = true;
        } else if (e.ch == 'd' || e.ch == 'D') {
          if (nTasks > 0) {
            for (int i = sel; i < nTasks - 1; i++) {
              strcpy(tasks[i], tasks[i + 1]);
              done[i] = done[i + 1];
            }
            nTasks--;
            if (sel >= nTasks && sel > 0) sel--;
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
  if (dirty && sdOk) {
    if (!SD.exists("/todo")) SD.mkdir("/todo");
    File f = SD.open(TODO_FILE, FILE_WRITE);
    if (f) {
      for (int i = 0; i < nTasks; i++) {
        f.print(done[i] ? "[x] " : "[ ] ");
        f.print(tasks[i]);
        f.print('\n');
      }
      f.close();
    }
  }
}
