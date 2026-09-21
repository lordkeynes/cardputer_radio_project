/** * Terminal: a small on-device shell. * Keyboard types, Enter runs. Commands operate on the SD card and query * device status. Long-click exits. */
#include <Arduino.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <TinyGPS++.h>
#include "pda.h"
#include "theme.h"
#include "terminal.h"

extern Arduino_GFX *gfx;
extern bool sdOk;
extern TinyGPSPlus gps;
void gpsPoll();
void spkBeep(int ms);

#define TERM_ROWS 18
#define TERM_COLS 48

static char termScreen[TERM_ROWS][TERM_COLS + 1];
static int termCurRow = 0;
static char termLine[TERM_COLS + 1];
static int termLen = 0;



static void termClear() {
  for (int i = 0; i < TERM_ROWS; i++) { memset(termScreen[i], 0, TERM_COLS + 1); termScreen[i][0] = ' '; termScreen[i][1] = 0; }
  termCurRow = 0;
}

static void termPrint(const char *s) {
  // simple scroll buffer: wrap into rows
  while (*s) {
    int len = strlen(termScreen[termCurRow]);
    if (*s == '\n') {
      if (termCurRow < TERM_ROWS - 1) termCurRow++;
      else { memmove(termScreen[0], termScreen[1], (TERM_ROWS - 1) * (TERM_COLS + 1)); termScreen[TERM_ROWS - 1][0] = ' '; termScreen[TERM_ROWS - 1][1] = 0; }
      continue;
    }
    if (len >= TERM_COLS) {
      if (termCurRow < TERM_ROWS - 1) termCurRow++;
      else { memmove(termScreen[0], termScreen[1], (TERM_ROWS - 1) * (TERM_COLS + 1)); termScreen[TERM_ROWS - 1][0] = ' '; termScreen[TERM_ROWS - 1][1] = 0; }
      len = strlen(termScreen[termCurRow]);
    }
    termScreen[termCurRow][len] = *s;
    termScreen[termCurRow][len + 1] = 0;
    s++;
  }
}

static void termPrintln(const char *s) { termPrint(s); termPrint("\n"); }

static void termLs(const char *path) {
  if (!sdOk) { termPrintln("no SD"); return; }
  File root = SD.open(path[0] ? path : "/");
  if (!root) { termPrintln("no such dir"); return; }
  if (!root.isDirectory()) { termPrintln("not a dir"); root.close(); return; }
  File f = root.openNextFile();
  int count = 0, files = 0, dirs = 0;
  while (f) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s %lu", f.name(), f.isDirectory() ? "/" : "", (unsigned long)f.size());
    termPrintln(buf);
    f.isDirectory() ? dirs++ : files++;
    count++;
    f.close();
    f = root.openNextFile();
    if (count > 300) { termPrintln("..."); break; }
  }
  root.close();
  char buf[48];
  snprintf(buf, sizeof(buf), "%d files, %d dirs", files, dirs);
  termPrintln(buf);
}

static void termCat(const char *path) {
  if (!sdOk) { termPrintln("no SD"); return; }
  if (!SD.exists(path)) { termPrintln("no such file"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { termPrintln("open failed"); return; }
  if (f.isDirectory()) { f.close(); termPrintln("is a dir"); return; }
  int printed = 0;
  while (f.available() && printed < 800) {
    char c = f.read();
    termPrint(&c); // slow but fine for a few hundred chars
    printed++;
  }
  f.close();
  termPrint("\n");
}

static void termRm(const char *path) {
  if (!sdOk) { termPrintln("no SD"); return; }
  if (!SD.exists(path)) { termPrintln("no such file"); return; }
  File f = SD.open(path);
  bool isDir = f && f.isDirectory();
  if (f) f.close();
  if (isDir) { if (SD.rmdir(path)) termPrintln("dir removed"); else termPrintln("rmdir failed (not empty?)"); }
  else { if (SD.remove(path)) termPrintln("removed"); else termPrintln("remove failed"); }
}

static void termDf() {
  if (!sdOk) { termPrintln("no SD"); return; }
  char buf[64];
  uint64_t total = SD.cardSize();
  snprintf(buf, sizeof(buf), "card %llu MB", (unsigned long long)(total / (1024UL * 1024UL)));
  termPrintln(buf);
  snprintf(buf, sizeof(buf), "free heap %lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
  termPrintln(buf);
  snprintf(buf, sizeof(buf), "free psram %lu KB", (unsigned long)(ESP.getFreePsram() / 1024));
  termPrintln(buf);
}

static void termHelp() {
  termPrintln("Commands:");
  termPrintln(" ls [dir]      cat <file>  rm <file>");
  termPrintln(" df           batt        gps");
  termPrintln(" ip           scan        beep");
  termPrintln(" date         clear       help");
}

static void termBatt() {
  char buf[48];
  snprintf(buf, sizeof(buf), "%d%% %d mV", batteryPercent(), batteryMillivolts());
  termPrintln(buf);
}

static void termGps() {
  gpsPoll();
  char buf[96];
  if (gps.location.isValid()) {
    snprintf(buf, sizeof(buf), "fix %.5f,%.5f  %d sats  %dm",
             gps.location.lat(), gps.location.lng(),
             gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
             (int)gps.altitude.meters());
  } else {
    snprintf(buf, sizeof(buf), "no fix  chars=%lu", (unsigned long)gps.charsProcessed());
  }
  termPrintln(buf);
}

static void termScan() {
  int n = WiFi.scanNetworks();
  if (n <= 0) { termPrintln("no networks"); return; }
  for (int i = 0; i < n && i < 10; i++) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%d %s %d dBm ch %d %s", i + 1,
             WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
             WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "sec");
    termPrintln(buf);
  }
  WiFi.scanDelete();
}

static void termDate() {
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  char buf[48];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec);
  termPrintln(buf);
}

static void termBeep() { spkBeep(80); termPrintln("beep"); }

// strip leading '/', build full path for SD commands
static void splitCmd(char *line, char **cmd, char **arg) {
  *cmd = line;
  *arg = NULL;
  for (char *p = line; *p; p++) {
    if (*p == ' ') { *p = 0; *arg = p + 1; break; }
  }
}

static void termRun(const char *input) {
  char buf[TERM_COLS + 1];
  strncpy(buf, input, TERM_COLS);
  buf[TERM_COLS] = 0;
  char *cmd = NULL, *arg = NULL;
  splitCmd(buf, &cmd, &arg);
  if (!cmd || !cmd[0]) return;
  if (!strcmp(cmd, "ls")) termLs(arg);
  else if (!strcmp(cmd, "cat")) termCat(arg);
  else if (!strcmp(cmd, "rm")) termRm(arg);
  else if (!strcmp(cmd, "df")) termDf();
  else if (!strcmp(cmd, "batt")) termBatt();
  else if (!strcmp(cmd, "gps")) termGps();
  else if (!strcmp(cmd, "ip")) {
    if (WiFi.status() == WL_CONNECTED) {
      char b[48];
      snprintf(b, sizeof(b), "ip %s", WiFi.localIP().toString().c_str());
      termPrintln(b);
    } else termPrintln("wifi off/not connected");
  }
  else if (!strcmp(cmd, "scan")) termScan();
  else if (!strcmp(cmd, "date")) termDate();
  else if (!strcmp(cmd, "beep")) termBeep();
  else if (!strcmp(cmd, "clear")) termClear();
  else if (!strcmp(cmd, "help")) termHelp();
  else { termPrint("unknown: "); termPrintln(cmd); }
}

void terminalApp() {
  termClear();
  termPrintln("T-Deck shell. type 'help'");
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      for (int i = 0; i < TERM_ROWS; i++) {
        gfx->setCursor(4, 4 + i * 11);
        gfx->print(termScreen[i]);
      }
      // prompt line
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(4, 4 + TERM_ROWS * 11 + 4);
      gfx->print("> ");
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->print(termLine);
      // cursor block
      int cx = 4 + 12 + termLen * 6;
      gfx->fillRect(cx, 4 + TERM_ROWS * 11 + 4, 6, 9, TERM_GREEN);
    }
    InputEventP e;
    if (!pdaGetInput(e, 20)) continue;
    if (e.ev == PDA_EV_CHAR && termLen < TERM_COLS) {
      termLine[termLen++] = e.ch;
      termLine[termLen] = 0;
      needsRedraw = true;
    } else if (e.ev == PDA_EV_SPACE && termLen < TERM_COLS) {
      termLine[termLen++] = ' ';
      termLine[termLen] = 0;
      needsRedraw = true;
    } else if (e.ev == PDA_EV_DELETE && termLen > 0) {
      termLine[--termLen] = 0;
      needsRedraw = true;
    } else if (e.ev == PDA_EV_NEWLINE) {
      // echo + run
      char echo[TERM_COLS + 2];
      snprintf(echo, sizeof(echo), "> %s", termLine);
      termPrintln(echo);
      termRun(termLine);
      termLen = 0;
      termLine[0] = 0;
      needsRedraw = true;
    } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      return;
    }
  }
}
