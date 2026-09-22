/**
 * Email app: read-only IMAP client.
 * Config lives on SD at /config/email.txt (one line: "host:port user pass").
 * Lists the newest messages in INBOX; click a message to read its body
 * (text part only; quoted-printable soft breaks handled).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "pda.h"
#include "theme.h"
#include "email.h"

extern Arduino_GFX *gfx;
extern bool sdOk;

#define EM_MAX_MSGS 12
#define EM_BUF 4000
static char emBuf[EM_BUF];

struct EmMsg {
  char from[48];
  char subj[72];
  char date[20];
  int uid;
};

static char emHost[64] = {0};
static int  emPort = 993;
static char emUser[48] = {0};
static char emPass[48] = {0};
static bool emCfgLoaded = false;
static bool emCfgOk = false;

static void emLoadConfig() {
  if (emCfgLoaded) return;
  emCfgLoaded = true;
  emCfgOk = false;
  if (!sdOk || !SD.exists("/config/email.txt")) return;
  File f = SD.open("/config/email.txt", FILE_READ);
  if (!f) return;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  int sp1 = line.indexOf(' ');
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return;
  String hp = line.substring(0, sp1);
  int colon = hp.indexOf(':');
  if (colon >= 0) {
    hp.substring(0, colon).toCharArray(emHost, sizeof(emHost));
    emPort = hp.substring(colon + 1).toInt();
  } else {
    hp.toCharArray(emHost, sizeof(emHost));
  }
  line.substring(sp1 + 1, sp2).toCharArray(emUser, sizeof(emUser));
  line.substring(sp2 + 1).toCharArray(emPass, sizeof(emPass));
  emCfgOk = emHost[0] && emUser[0];
}

static void emHeader() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(4, 7);
  gfx->print("Email");
  gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
}

static void emWaitBack(const char *hint) {
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(4, SCREEN_H - 12);
  gfx->print(hint);
  while (true) {
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) return;
  }
}

static void emMsgScreen(const char *line1, const char *line2,
                        const char *line3) {
  emHeader();
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(10, 50);
  gfx->print(line1);
  if (line2) { gfx->setCursor(10, 64); gfx->print(line2); }
  if (line3) { gfx->setCursor(10, 78); gfx->print(line3); }
  emWaitBack("Long=back");
}

static WiFiClientSecure emClient;
static bool emConnected = false;

static void emDisconnect() {
  if (emConnected) {
    emClient.print("A999 LOGOUT\r\n");
    delay(50);
  }
  emClient.stop();
  emConnected = false;
}

// Read lines until the tagged one; untagged lines are appended (bounded)
// to *out so the caller can parse STATUS data out of them.
static bool emReadResponse(const char *tag, String *out) {
  String line;
  unsigned long start = millis();
  while (millis() - start < 15000UL) {
    if (!emClient.connected()) return false;
    if (!emClient.available()) { delay(2); continue; }
    line = emClient.readStringUntil('\n');
    if (line.startsWith(tag)) return line.indexOf(" OK") >= 0;
    if (out && out->length() < 900) *out += line + "\n";
  }
  return false;
}

// Send cmd; capture the {n} literal that follows into emBuf.
static int emFetchLiteral(const char *cmd, const char *tag) {
  emClient.print(cmd);
  String line;
  unsigned long start = millis();
  int len = -1;
  while (millis() - start < 15000UL && len < 0) {
    if (!emClient.connected()) return 0;
    if (!emClient.available()) { delay(2); continue; }
    line = emClient.readStringUntil('\n');
    int ob = line.lastIndexOf('{');
    int cb = line.lastIndexOf('}');
    if (ob >= 0 && cb > ob) len = line.substring(ob + 1, cb).toInt();
  }
  if (len <= 0) return 0;
  if (len >= EM_BUF) len = EM_BUF - 1;
  int got = 0;
  start = millis();
  while (got < len && millis() - start < 10000UL) {
    if (emClient.available()) emBuf[got++] = emClient.read();
    else delay(1);
  }
  emBuf[got] = 0;
  // drain the trailing ")" line and the tagged completion
  emReadResponse(tag, NULL);
  return got;
}

static bool emConnect() {
  if (emConnected && emClient.connected()) return true;
  emDisconnect();
  if (!emClient.connect(emHost, emPort, 10000UL)) return false;
  if (!emReadResponse("* OK", NULL) && !emClient.connected()) return false;
  emClient.print("A1 LOGIN \"" + String(emUser) + "\" \"" + String(emPass) + "\"\r\n");
  if (!emReadResponse("A1", NULL)) { emDisconnect(); return false; }
  emConnected = true;
  return true;
}

static void emParseHeaderFields(EmMsg &m) {
  char *line = strtok(emBuf, "\r\n");
  char lastKey[16] = {0};
  while (line) {
    if (line[0] == ' ' || line[0] == '\t') {
      if (lastKey[0] && strcasecmp(lastKey, "subject") == 0)
        strncat(m.subj, line, sizeof(m.subj) - 1 - strlen(m.subj));
    } else if (strncasecmp(line, "From: ", 6) == 0) {
      strncpy(m.from, line + 6, sizeof(m.from) - 1);
      strcpy(lastKey, "from");
    } else if (strncasecmp(line, "Subject: ", 9) == 0) {
      strncpy(m.subj, line + 9, sizeof(m.subj) - 1);
      strcpy(lastKey, "subject");
    } else if (strncasecmp(line, "Date: ", 6) == 0) {
      const char *d = line + 6;
      int off = (d[5] == ',') ? 7 : 0;
      strncpy(m.date, d + off, 17);
      m.date[17] = 0;
      strcpy(lastKey, "date");
    } else {
      lastKey[0] = 0;
    }
    line = strtok(NULL, "\r\n");
  }
}

static int emFetchList(EmMsg *msgs, int maxN) {
  String out;
  emClient.print("A2 STATUS INBOX (MESSAGES)\r\n");
  if (!emReadResponse("A2", &out)) return 0;
  int p = out.indexOf("MESSAGES (");
  if (p < 0) return 0;
  long total = out.substring(p + 10).toInt();
  if (total <= 0) return 0;
  emClient.print("A3 EXAMINE INBOX\r\n");
  if (!emReadResponse("A3", NULL)) return 0;
  int n = (total < maxN) ? (int)total : maxN;
  for (int i = 0; i < n; i++) {
    long seq = total - i;   // highest sequence = newest
    memset(&msgs[i], 0, sizeof(EmMsg));
    msgs[i].uid = (int)seq;
    String cmd = "A4 FETCH " + String(seq) +
                 " (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n";
    int got = emFetchLiteral(cmd.c_str(), "A4");
    if (got > 0) emParseHeaderFields(msgs[i]);
  }
  return n;
}

static int emFetchBody(int seq) {
  String cmd = "B1 FETCH " + String(seq) + " (BODY.PEEK[TEXT])\r\n";
  return emFetchLiteral(cmd.c_str(), "B1");
}

static void emShowText(const char *text) {
  int y = 58;
  const char *p = text;
  char line[56];
  int li = 0;
  while (*p && y <= SCREEN_H - 18) {
    if (*p == '=' && p[1] == '\n') { p += 2; continue; }  // QP soft break
    if (*p == '\r') { p++; continue; }
    if (*p == '\n' || li >= 54) {
      line[li] = 0;
      gfx->setCursor(8, y);
      gfx->print(line);
      y += 12;
      li = 0;
    } else if (li < 55) {
      line[li++] = *p;
    }
    p++;
  }
  if (li > 0 && y <= SCREEN_H - 18) {
    line[li] = 0;
    gfx->setCursor(8, y);
    gfx->print(line);
  }
}

static void emMsgList(EmMsg *msgs, int n, int sel) {
  emHeader();
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(8, 24);
  gfx->print("u/d=pick click=read Long=back");
  for (int i = 0; i < n; i++) {
    int y = 44 + i * 16;
    if (i == sel) {
      gfx->fillRect(0, y - 2, SCREEN_W, 15, TERM_SEL_BG);
      gfx->setTextColor(BLACK, TERM_SEL_BG);
    } else gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setCursor(6, y);
    gfx->print(msgs[i].subj);
    gfx->setTextColor(TERM_DIM, i == sel ? TERM_SEL_BG : BLACK);
    gfx->setCursor(6, y + 7);
    gfx->print(msgs[i].from);
    gfx->setCursor(SCREEN_W - 116, y + 7);
    gfx->print(msgs[i].date);
  }
}

void emailApp() {
  static EmMsg msgs[EM_MAX_MSGS];
  emLoadConfig();
  if (!emCfgOk) {
    emMsgScreen("No email config.",
                "Create /config/email.txt",
                "one line: host:port user pass");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    emMsgScreen("WiFi not connected.", NULL, NULL);
    return;
  }
  emClient.setInsecure();   // accept self-signed / avoid CA storage
  if (!emConnect()) {
    emMsgScreen("IMAP connect failed.", "Check host/user/pass.", NULL);
    return;
  }
  int n = emFetchList(msgs, EM_MAX_MSGS);
  int sel = 0;
  bool full = true;
  while (true) {
    if (full) { emMsgList(msgs, n, sel); full = false; }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_UP && sel > 0) { sel--; full = true; }
    else if (e.ev == PDA_EV_DOWN && sel < n - 1) { sel++; full = true; }
    else if ((e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) && n > 0) {
      emHeader();
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 24);
      gfx->print("Reading... Long=back");
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->setCursor(8, 36);
      gfx->print(msgs[sel].subj);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 46);
      gfx->print(msgs[sel].from);
      gfx->print("  ");
      gfx->print(msgs[sel].date);
      int got = emFetchBody(msgs[sel].uid);
      gfx->fillRect(0, 56, SCREEN_W, SCREEN_H - 56, BLACK);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      if (got > 0) emShowText(emBuf);
      else {
        gfx->setCursor(8, 60);
        gfx->print("(empty or unsupported body)");
      }
      emWaitBack("Long=back");
      full = true;
    }
    else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      emDisconnect();
      return;
    }
  }
}
