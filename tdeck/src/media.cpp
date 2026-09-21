/**
 * More apps: ebook TXT reader, RGB565 image viewer, wardrive logger.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include <TinyGPS++.h>
#include "utilities.h"
#include "pda.h"
#include "theme.h"
#include "media.h"

extern Arduino_GFX *gfx;
extern bool sdOk;

// ============================ Ebook TXT reader ============================
#define BOOK_LINE 60
#define BOOK_ROWS 16
void ebookApp() {
  // pick a .txt from /books
  if (!sdOk) return;
  String names[24];
  int n = 0;
  if (!SD.exists("/books")) SD.mkdir("/books");
  File root = SD.open("/books");
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f && n < 24) {
      String nm = f.name();
      if (!f.isDirectory() && nm.endsWith(".txt")) names[n++] = nm;
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }
  if (n == 0) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(8, 40);
    gfx->print("No .txt books in /books on SD.");
    gfx->setCursor(8, 60);
    gfx->print("Put .txt books there (e.g. gutenberg.org), or");
    gfx->setCursor(8, 72);
    gfx->print("convert EPUBs: tools/epub2txt.py book.epub");
    gfx->setCursor(8, 84);
    gfx->print("  <sd>/books  (run on your PC).");
    InputEventP w; bool done = false;
    unsigned long t0 = millis();
    while (!done && millis() - t0 < 5000) { if (pdaGetInput(w, 50)) done = true; }
    return;
  }
  // book picker
  int sel = 0;
  bool picking = true;
  while (picking) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(8, 8);
    gfx->print("Books");
    for (int i = 0; i < 10 && i < n; i++) {
      int y = 34 + i * 18;
      if (i == sel) { gfx->fillRect(0, y - 2, SCREEN_W, 16, TERM_SEL_BG); gfx->setTextColor(BLACK, TERM_SEL_BG); }
      else gfx->setTextColor(WHITE, BLACK);
      gfx->setTextSize(1);
      gfx->setCursor(8, y);
      gfx->print(names[i]);
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) sel--;
    else if (e.ev == PDA_EV_DOWN && sel < n - 1) sel++;
    else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) picking = false;
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
  // read the book (up to 400 KB) — keep the big buffer out of static DRAM:
  // try PSRAM first, fall back to heap.
  String path = "/books/" + names[sel];
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  size_t bookCap = 400001;
  char *book = (char *)heap_caps_malloc(bookCap, MALLOC_CAP_SPIRAM);
  if (!book) book = (char *)malloc(bookCap);
  if (!book) { f.close(); return; }
  int len = 0;
  while (f.available() && len < (int)bookCap - 1) {
    int c = f.read();
    if (c == '\r') continue;
    book[len++] = (char)c;
  }
  f.close();
  book[len] = 0;

  // pagination: record the start offset of each page
  // a page = up to BOOK_ROWS lines of up to BOOK_LINE chars (wrap at spaces)
  int pageStarts[1024];
  int nPages = 0;
  pageStarts[nPages++] = 0;
  int pos = 0;
  while (pos < len && nPages < 1024) {
    // build one page
    int rows = 0;
    while (rows < BOOK_ROWS && pos < len) {
      if (book[pos] == '\n') { pos++; rows++; continue; }
      // find next wrap point
      int lineEnd = pos;
      int lastSpace = -1;
      while (lineEnd < len && book[lineEnd] != '\n' && lineEnd - pos < BOOK_LINE) {
        if (book[lineEnd] == ' ') lastSpace = lineEnd;
        lineEnd++;
      }
      if (book[lineEnd] == '\n') { pos = lineEnd + 1; }
      else if (lineEnd - pos >= BOOK_LINE && lastSpace > pos) { pos = lastSpace + 1; }
      else { pos = lineEnd; if (pos < len && book[pos] != '\n') { /* consumed whole word */ } }
      rows++;
    }
    if (pos < len) pageStarts[nPages++] = pos;
  }

  int page = 0;
  bool needsRedraw = true;
  bool reading = true;
  while (reading) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      int pos = pageStarts[page];
      int rows = 0;
      int y = 4;
      while (rows < BOOK_ROWS && pos < (page + 1 < nPages ? pageStarts[page + 1] : len)) {
        if (book[pos] == '\n') { pos++; rows++; y += 14; continue; }
        // one wrapped line
        int lineEnd = pos;
        int lastSpace = -1;
        while (lineEnd < len && book[lineEnd] != '\n' && lineEnd - pos < BOOK_LINE) {
          if (book[lineEnd] == ' ') lastSpace = lineEnd;
          lineEnd++;
        }
        int end = lineEnd;
        if (book[lineEnd] != '\n' && lineEnd - pos >= BOOK_LINE && lastSpace > pos) end = lastSpace;
        gfx->setCursor(4, y);
        for (int i = pos; i < end; i++) gfx->print(book[i]);
        pos = (book[lineEnd] == '\n') ? lineEnd : (end > pos ? end : lineEnd);
        if (pos < len && book[pos] == ' ') pos++;
        y += 14;
        rows++;
      }
      // footer
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->printf("%s  pg %d/%d  L/R=page", names[sel].c_str(), page + 1, nPages);
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_RIGHT || e.ev == PDA_EV_DOWN) { if (page < nPages - 1) page++; needsRedraw = true; }
    else if (e.ev == PDA_EV_LEFT || e.ev == PDA_EV_UP) { if (page > 0) page--; needsRedraw = true; }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) reading = false;
  }
  free(book);
}

// ============================ Image viewer ============================
// Displays raw RGB565 .bin images (320x240 or smaller), little-endian.
// Convert images on PC: python3 tools/img2rgb565.py in.png out.bin
void imageApp() {
  if (!sdOk) return;
  String names[24];
  int n = 0;
  if (!SD.exists("/images")) SD.mkdir("/images");
  File root = SD.open("/images");
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f && n < 24) {
      String nm = f.name();
      if (!f.isDirectory() && nm.endsWith(".bin")) names[n++] = nm;
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }
  if (n == 0) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(8, 40);
    gfx->print("No .bin images in /images on SD.");
    gfx->setCursor(8, 60);
    gfx->print("Convert with tools/img2rgb565.py first.");
    InputEventP w; bool done = false;
    unsigned long t0 = millis();
    while (!done && millis() - t0 < 5000) { if (pdaGetInput(w, 50)) done = true; }
    return;
  }
  int sel = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      String path = "/images/" + names[sel];
      File f = SD.open(path, FILE_READ);
      if (f) {
        static uint16_t line[320];
        int y = 0;
        while (f.available() >= 640 && y < SCREEN_H) {
          f.read((uint8_t *)line, 640);
          gfx->draw16bitRGBBitmap(0, y, line, 320, 1);
          y++;
        }
        f.close();
      }
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->printf("%d/%d  L/R=browse  Long=exit", sel + 1, n);
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_RIGHT && sel < n - 1) { sel++; needsRedraw = true; }
    else if (e.ev == PDA_EV_LEFT && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

// ============================ Wardrive logger ============================
// Passive survey: scan networks, log SSID/BSSID/RSSI + GPS to /wardrive/wifi.csv
// (WiGLE-adjacent format). Purely passive - no probing, no attack tools.
void wardriveApp() {
  if (!sdOk) return;
  if (!SD.exists("/wardrive")) SD.mkdir("/wardrive");
  bool logging = false;
  uint32_t lastScanMs = 0;
  uint32_t loggedCount = 0;
  static char seenBssids[200][18];  // dedup within session
  int nSeen = 0;
  bool needsRedraw = true;

  // shared GPS access from main.cpp
  extern TinyGPSPlus gps;
  extern void gpsPoll();

  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print("Wardrive");
      gfx->setTextSize(1);
      gfx->setTextColor(logging ? TERM_GREEN : TERM_DIM, BLACK);
      gfx->setCursor(8, 34);
      gfx->print(logging ? "LOGGING (passive)" : "paused");
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(8, 50);
      gfx->printf("networks seen: %d", nSeen);
      gfx->setCursor(8, 62);
      gfx->printf("rows logged:   %lu", (unsigned long)loggedCount);
      gfx->setCursor(8, 78);
      bool hasFix = gps.location.isValid();
      gfx->setTextColor(hasFix ? TERM_GREEN : TERM_RED, BLACK);
      gfx->printf("GPS: %s  sats: %d", hasFix ? "fix" : "no fix",
                  gps.satellites.isValid() ? gps.satellites.value() : 0);
      if (hasFix) {
        gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(8, 90);
        gfx->printf("%.5f, %.5f", gps.location.lat(), gps.location.lng());
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 20);
      gfx->print("Click=start/stop  CSV->/wardrive/wifi.csv");
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("Long-click=back   (passive survey only)");
    }

    if (logging && millis() - lastScanMs > 8000) {
      lastScanMs = millis();
      gpsPoll();
      int found = WiFi.scanNetworks(false, false, false, 300);
      File f = SD.open("/wardrive/wifi.csv", FILE_APPEND);
      for (int i = 0; i < found; i++) {
        String ssid = WiFi.SSID(i);
        String bssid = WiFi.BSSIDstr(i);
        bssid.replace(":", "");
        bssid.toLowerCase();
        // dedup: same BSSID within session logged once (first/best seen)
        bool seen = false;
        for (int j = 0; j < nSeen; j++) {
          if (bssid.equals(seenBssids[j])) { seen = true; break; }
        }
        if (seen) continue;
        if (nSeen < 200) strcpy(seenBssids[nSeen++], bssid.c_str());
        bool hasFix = gps.location.isValid();
        if (f) {
          // WiGLE-style: lat,lon updated,SSID,BSSID, RSSI
          f.printf("%.6f,%.6f,%s,%s,%d,%s\n",
                   hasFix ? gps.location.lat() : 0.0,
                   hasFix ? gps.location.lng() : 0.0,
                   ssid.length() ? ssid.c_str() : "<hidden>",
                   WiFi.BSSIDstr(i).c_str(),
                   WiFi.RSSI(i),
                   WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SECURE");
          loggedCount++;
        }
      }
      WiFi.scanDelete();
      if (f) f.close();
      needsRedraw = true;
    }

    InputEventP e;
    if (!pdaGetInput(e, 100)) continue;
    if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      logging = !logging;
      lastScanMs = 0;
      needsRedraw = true;
    } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      WiFi.scanDelete();
      return;
    }
  }
}
