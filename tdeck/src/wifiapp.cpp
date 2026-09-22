/**
 * WiFi app: scan networks, join with on-device password entry, store
 * credentials on SD (/wifi/known.txt), auto-connect to known networks at boot.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "utilities.h"
#include "pda.h"
#include "theme.h"

extern Arduino_GFX *gfx;
extern bool sdOk;

#define WIFI_DIR "/wifi"
#define WIFI_FILE WIFI_DIR "/known.txt"
#define MAX_KNOWN 8

static bool wifiUp = false;

struct KnownNet {
  String ssid, pass;
};

static int loadKnown(KnownNet *out, int maxN) {
  int n = 0;
  if (!sdOk || !SD.exists(WIFI_FILE)) return 0;
  File f = SD.open(WIFI_FILE, FILE_READ);
  if (!f) return 0;
  String line;
  while (f.available() && n < maxN) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int tab = line.indexOf('\t');
    if (tab < 0) continue;
    out[n].ssid = line.substring(0, tab);
    out[n].pass = line.substring(tab + 1);
    n++;
  }
  f.close();
  return n;
}

static void saveKnown(KnownNet *nets, int n) {
  if (!sdOk) return;
  if (!SD.exists(WIFI_DIR)) SD.mkdir(WIFI_DIR);
  File f = SD.open(WIFI_FILE, FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < n; i++) {
    f.print(nets[i].ssid);
    f.print('\t');
    f.print(nets[i].pass);
    f.print('\n');
  }
  f.close();
}

bool wifiAutoConnect() {
  KnownNet known[MAX_KNOWN];
  int n = loadKnown(known, MAX_KNOWN);
  if (n == 0) return false;
  // Persistent mode + persistent config so the radio keeps credentials
  // across power cycles and reconnects on its own after dropouts.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  // Fast path: NVS already knows the last network; reconnect in seconds.
  if (WiFi.begin() == WL_CONNECTED || WiFi.status() == WL_CONNECTED) {
    wifiUp = true;
    Serial.printf("[wifi] reconnected to %s (%s)\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return true;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    for (int i = 0; i < n; i++) {
      WiFi.begin(known[i].ssid.c_str(), known[i].pass.c_str());
      uint32_t start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
        delay(100);
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifiUp = true;
        Serial.printf("[wifi] connected to %s (%s)\n",
                      known[i].ssid.c_str(), WiFi.localIP().toString().c_str());
        return true;
      }
      WiFi.disconnect();
      delay(200);
    }
  }
  return false;
}

bool wifiConnected() { return wifiUp && WiFi.status() == WL_CONNECTED; }

// Simple text prompt at the bottom of the screen; returns entered string.
bool promptText(const char *label, String &out) {
  out = "";
  gfx->fillRect(0, SCREEN_H - 40, SCREEN_W, 40, BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_ACCENT, BLACK);
  gfx->setCursor(8, SCREEN_H - 36);
  gfx->print(label);
  gfx->print(": ");
  while (true) {
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_NEWLINE || e.ev == PDA_EV_SELECT) return out.length() > 0;
    if (e.ev == PDA_EV_SPACE) {
      if (out.length() < 63) { out += ' '; gfx->print(' '); }
    } else if (e.ev == PDA_EV_CHAR) {
      if (out.length() < 63) { out += e.ch; gfx->print(e.ch); }
    } else if (e.ev == PDA_EV_DELETE) {
      if (out.length() > 0) {
        out.remove(out.length() - 1);
        gfx->print('\b'); gfx->print(' '); gfx->print('\b');
      }
    } else if (e.ev == PDA_EV_BACK || e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_LEFT) {
      return false;
    }
  }
}

void wifiApp() {
  const int visible = 8;
  int sel = 0;
  int nScan = 0;
  static String ssids[16];
  static int32_t rssis[16];
  bool needsRedraw = true;
  bool rescanning = false;
  while (true) {
    if (needsRedraw || rescanning) {
      needsRedraw = false;
      rescanning = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print("WiFi");
      gfx->setTextSize(1);
      if (wifiConnected()) {
        gfx->setTextColor(TERM_GREEN, BLACK);
        gfx->setCursor(8, 26);
        gfx->printf("IP: %s", WiFi.localIP().toString().c_str());
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 26);
      gfx->print("Scanning...");
      nScan = 0;
      int found = WiFi.scanNetworks();
      for (int i = 0; i < found && nScan < 16; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        bool dup = false;
        for (int j = 0; j < nScan; j++) if (ssids[j] == s) { dup = true; break; }
        if (dup) continue;
        ssids[nScan] = s;
        rssis[nScan] = WiFi.RSSI(i);
        nScan++;
      }
      WiFi.scanDelete();
      gfx->fillRect(0, 26, SCREEN_W, 12, BLACK);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(8, 26);
      gfx->printf("%d networks - click to join", nScan);
      const int rowH = 18;
      int top = 0;
      if (nScan > visible && sel >= visible) top = sel - visible + 1;
      for (int i = 0; i < visible && top + i < nScan; i++) {
        int idx = top + i;
        int y = 40 + i * rowH;
        if (idx == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, rowH - 2, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else {
          gfx->setTextColor(WHITE, BLACK);
        }
        gfx->setCursor(8, y);
        gfx->print(ssids[idx]);
        gfx->setTextColor(TERM_DIM,
                          (idx == sel) ? TERM_SEL_BG : BLACK);
        gfx->setCursor(240, y);
        gfx->printf("%d dBm", (int)rssis[idx]);
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 20);
      gfx->print("Up/Down pick  r rescan  Click join  Long-click back");
      if (nScan == 0) {
        gfx->setTextColor(TERM_ACCENT, BLACK);
        gfx->setCursor(8, 44);
        gfx->print("No networks found - press r");
      }
    }
    InputEventP e;
    if (!pdaGetInput(e, 100)) continue;
    switch (e.ev) {
      case PDA_EV_UP: if (sel > 0) { sel--; needsRedraw = true; } break;
      case PDA_EV_DOWN: if (sel < nScan - 1) { sel++; needsRedraw = true; } break;
      case PDA_EV_CHAR:
        if (e.ch == 'r' || e.ch == 'R') rescanning = true;
        break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        if (sel < 0 || sel >= nScan) break;
        String ssid = ssids[sel];
        String pass;
        gfx->fillRect(0, SCREEN_H - 40, SCREEN_W, 40, BLACK);
        gfx->setTextSize(1);
        gfx->setTextColor(TERM_ACCENT, BLACK);
        gfx->setCursor(8, SCREEN_H - 36);
        gfx->printf("Joining %s ...", ssid.c_str());
        // Try known credentials first
        KnownNet known[MAX_KNOWN];
        int nKnown = loadKnown(known, MAX_KNOWN);
        bool haveKey = false;
        for (int i = 0; i < nKnown; i++) {
          if (known[i].ssid == ssid) { pass = known[i].pass; haveKey = true; break; }
        }
        if (!haveKey) {
          if (!promptText("Password", pass)) break;
        }
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(100);
        gfx->fillRect(0, SCREEN_H - 40, SCREEN_W, 40, BLACK);
        gfx->setCursor(8, SCREEN_H - 36);
        if (WiFi.status() == WL_CONNECTED) {
          wifiUp = true;
          gfx->setTextColor(TERM_GREEN, BLACK);
          gfx->printf("Connected: %s", WiFi.localIP().toString().c_str());
          // Save/update credentials
          int slot = -1;
          for (int i = 0; i < nKnown; i++) if (known[i].ssid == ssid) { slot = i; break; }
          if (slot < 0 && nKnown < MAX_KNOWN) slot = nKnown++;
          if (slot >= 0) {
            known[slot].ssid = ssid;
            known[slot].pass = pass;
            saveKnown(known, nKnown);
          }
        } else {
          gfx->setTextColor(TERM_RED, BLACK);
          gfx->print("Failed to connect");
        }
        delay(1200);
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

// ============================ WiFi bands (graphical) ============================
// Channel spectrum view: 2.4 GHz channels 1-14 on the x axis, signal
// strength on y. Each AP is a rounded bump; overlapping nets form ridges.
void wifiBandsApp() {
  int n = WiFi.scanNetworks();
  const int N_CH = 14;
  int strongest[N_CH];
  for (int i = 0; i < N_CH; i++) strongest[i] = -100;
  int nSec = 0, nOpen = 0;
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      int ch = WiFi.channel(i);
      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) nOpen++;
      else nSec++;
      int rssi = WiFi.RSSI(i);
      int idx = ch - 1;
      if (idx >= 0 && idx < N_CH && rssi > strongest[idx]) strongest[idx] = rssi;
    }
  }

  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(8, 6);
  gfx->print("WiFi bands");
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(230, 10);
  gfx->printf("%d nets", n > 0 ? n : 0);

  // plot area
  const int PX = 12, PY = 34, PW = 296, PH = 130;
  gfx->drawRect(PX, PY, PW, PH, TERM_DIM);

  // y grid: -30 to -100 dBm
  for (int db = -40; db >= -100; db -= 20) {
    int y = PY + PH - (int)((float)(db + 100) / 70.0 * PH);
    if (y <= PY + PH && y > PY) {
      gfx->drawFastHLine(PX + 1, y, PW - 2, RGB565(30, 60, 40));
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(PX - 2, y - 4);
      gfx->printf("%d", db);
    }
  }

  // per-channel bars with signal color
  int colW = PW / N_CH;
  for (int ch = 0; ch < N_CH; ch++) {
    int cx = PX + ch * colW;
    int s = strongest[ch];
    if (s <= -100) continue;
    int h = (int)((float)(s + 100) / 70.0 * PH);
    if (h <= 0) continue;
    uint16_t col = (s > -60) ? TERM_ACCENT : (s > -75) ? TERM_GREEN : TERM_DIM;
    // draw a gaussian-ish bump centered in the channel cell
    for (int dy = 0; dy < h; dy++) {
      int y = PY + PH - 1 - dy;
      float frac = 1.0f - (float)dy / h;
      int half = (int)(colW * 0.6f * frac);
      if (half > 0)
        gfx->drawFastHLine(cx + colW / 2 - half, y, half * 2, col);
    }
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(cx + colW / 2 - 3, PY + PH + 4);
    gfx->printf("%d", ch + 1);
  }
  // channel number for empty channels too
  for (int ch = 0; ch < N_CH; ch++) {
    if (strongest[ch] <= -100) {
      int cx = PX + ch * colW;
      gfx->setTextColor(RGB565(40, 40, 40), BLACK);
      gfx->setCursor(cx + colW / 2 - 3, PY + PH + 4);
      gfx->printf("%d", ch + 1);
    }
  }

  // legend
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(12, PY + PH + 20);
  gfx->printf("2.4 GHz  %d secured, %d open", nSec, nOpen);
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("click = rescan   Long = back");

  while (true) {
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) { wifiBandsApp(); return; }
    if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) { WiFi.scanDelete(); return; }
  }
}

// Saved-network management for the Settings app: list /wifi/known.txt
// entries, let the user forget individual networks.
int wifiKnownList(String *ssids, int maxN) {
  KnownNet nets[MAX_KNOWN];
  int n = loadKnown(nets, MAX_KNOWN);
  if (n > maxN) n = maxN;
  for (int i = 0; i < n; i++) ssids[i] = nets[i].ssid;
  return n;
}

void wifiForget(int idx) {
  KnownNet nets[MAX_KNOWN];
  int n = loadKnown(nets, MAX_KNOWN);
  if (idx < 0 || idx >= n) return;
  for (int i = idx; i < n - 1; i++) nets[i] = nets[i + 1];
  saveKnown(nets, n - 1);
}
