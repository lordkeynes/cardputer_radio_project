/**
 * PDA apps: calculator, search, contacts, unit converter, file manager.
 * Plus games: chess (vs simple AI), Go (9x9 vs simple AI).
 */
#include <Arduino.h>
#include <SD.h>
#include <math.h>
#include <Arduino_GFX_Library.h>
#include "utilities.h"
#include "pda.h"
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;
extern bool sdOk;

// promptText is shared (defined in wifiapp.cpp)
bool promptText(const char *label, String &out);

// ============================ Calculator ============================
void calcApp() {
  String expr;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print("Calc");
      // expression area
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->setCursor(8, 40);
      gfx->print(expr.length() ? expr : "_");
      // hint
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 20);
      gfx->print("digits +-*/(). Enter=eval  c=clear  Long=back");
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("Expr: 0-9 . + - * / ( )");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK || e.ev == PDA_EV_LEFT) return;
    if (e.ev == PDA_EV_CHAR) {
      char c = e.ch;
      if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' ||
          c == '*' || c == '/' || c == '(' || c == ')') {
        if (expr.length() < 28) { expr += c; needsRedraw = true; }
      } else if (c == 'c' || c == 'C') {
        expr = ""; needsRedraw = true;
      }
    } else if (e.ev == PDA_EV_DELETE) {
      if (expr.length() > 0) { expr.remove(expr.length() - 1); needsRedraw = true; }
    } else if (e.ev == PDA_EV_NEWLINE || e.ev == PDA_EV_SELECT) {
      // --- tiny recursive-descent parser ---
      const char *p = expr.c_str();
      double result = 0;
      bool ok = expr.length() > 0;
      auto skip = [&]() { while (*p == ' ') p++; };
      // captured lambdas for recursion
      struct Parser {
        const char *p;
        bool ok;
        double parseExpr() { // + -
          double v = parseTerm();
          while (ok) {
            while (*p == ' ') p++;
            if (*p == '+') { p++; v += parseTerm(); }
            else if (*p == '-') { p++; v -= parseTerm(); }
            else return v;
          }
          return v;
        }
        double parseTerm() { // * /
          double v = parseFactor();
          while (ok) {
            while (*p == ' ') p++;
            if (*p == '*') { p++; v *= parseFactor(); }
            else if (*p == '/') { p++; double d = parseFactor(); if (d == 0) ok = false; else v /= d; }
            else return v;
          }
          return v;
        }
        double parseFactor() { // number or (expr), unary minus
          while (*p == ' ') p++;
          if (*p == '-') { p++; return -parseFactor(); }
          if (*p == '+') { p++; return parseFactor(); }
          if (*p == '(') {
            p++;
            double v = parseExpr();
            if (*p == ')') { p++; return v; }
            ok = false; return 0;
          }
          char *end = nullptr;
          double v = strtod(p, &end);
          if (end == p) { ok = false; return 0; }
          p = end;
          return v;
        }
      } parser{p, ok};
      result = parser.parseExpr();
      while (*parser.p == ' ') parser.p++;
      if (*parser.p != 0) parser.ok = false;
      ok = parser.ok;
      // show result
      gfx->fillRect(0, 100, SCREEN_W, 40, BLACK);
      gfx->setTextSize(3);
      if (ok) {
        gfx->setTextColor(TERM_GREEN, BLACK);
        gfx->setCursor(8, 110);
        if (fabs(result) < 1e12) gfx->print(result, 6);
        else gfx->print("overflow");
      } else {
        gfx->setTextColor(TERM_RED, BLACK);
        gfx->setCursor(8, 110);
        gfx->print("syntax error");
      }
    }
  }
}

// ============================ Search ============================
void searchApp() {
  String query;
  bool needsRedraw = true;
  // results
  String rFiles[16]; String rLines[16];
  int nResults = 0;
  int sel = 0;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print("Search");
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->setCursor(8, 30);
      gfx->print("Find: ");
      gfx->print(query.length() ? query : "(type, Enter)");
      for (int i = 0; i < 8 && i < nResults; i++) {
        int y = 50 + i * 20;
        int idx = i + (sel / 8) * 8;
        if (idx >= nResults) break;
        bool isSel = (idx == sel);
        if (isSel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 18, TERM_SEL_BG);
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(8, y);
        gfx->print(rFiles[idx]);
        gfx->setCursor(8, y + 9);
        gfx->setTextColor(isSel ? BLACK : TERM_DIM, isSel ? TERM_SEL_BG : BLACK);
        gfx->print(rLines[idx]);
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("Enter=search  U/D pick  Long=open/back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    switch (e.ev) {
      case PDA_EV_CHAR: {
        if (query.length() < 24 && ((e.ch >= 'a' && e.ch <= 'z') || (e.ch >= 'A' && e.ch <= 'Z') ||
            (e.ch >= '0' && e.ch <= '9'))) {
          query += e.ch; needsRedraw = true;
        }
        break;
      }
      case PDA_EV_DELETE: if (query.length()) { query.remove(query.length()-1); needsRedraw = true; } break;
      case PDA_EV_NEWLINE: {
        // search all notes + todo
        nResults = 0; sel = 0;
        String q = query; q.toLowerCase();
        if (sdOk && query.length() > 0) {
          String dirs[2] = {"/notes", "/todo"};
          const char *exts[2] = {".txt", ".txt"};
          for (int d = 0; d < 2 && nResults < 16; d++) {
            File root = SD.open(dirs[d]);
            if (!root || !root.isDirectory()) continue;
            File f = root.openNextFile();
            while (f && nResults < 16) {
              String name = String(f.name());
              if (!f.isDirectory() && name.endsWith(exts[d])) {
                String content = "";
                while (f.available() && content.length() < 4096) content += (char)f.read();
                f.close();
                String low = content; low.toLowerCase();
                int at = 0;
                while (nResults < 16) {
                  at = low.indexOf(query, at);
                  if (at < 0) break;
                  // extract line containing match
                  int ls = low.lastIndexOf('\n', at);
                  if (ls < 0) ls = 0;
                  int le = low.indexOf('\n', at);
                  if (le < 0) le = content.length();
                  String line = content.substring(ls, le);
                  line.trim();
                  if (line.length() > 40) line = line.substring(0, 40);
                  rFiles[nResults] = dirs[d] + "/" + name;
                  rLines[nResults] = line;
                  nResults++;
                  at += query.length();
                }
              } else f.close();
              f = root.openNextFile();
            }
            root.close();
          }
        }
        needsRedraw = true;
        break;
      }
      case PDA_EV_UP: if (sel > 0) { sel--; needsRedraw = true; } break;
      case PDA_EV_DOWN: if (sel < nResults - 1) { sel++; needsRedraw = true; } break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK:
      case PDA_EV_LEFT: return;
      default: break;
    }
  }
}

// ============================ Contacts ============================
#define CONTACTS_FILE "/contacts/contacts.txt"
void contactsApp() {
  bool needsRedraw = true;
  int sel = 0;
  // Load contacts: "Name<TAB>Phone<TAB>Email" per line
  static String names[32], phones[32], emails[32];
  int n = 0;
  if (sdOk && SD.exists(CONTACTS_FILE)) {
    File f = SD.open(CONTACTS_FILE, FILE_READ);
    while (f.available() && n < 32) {
      String line = f.readStringUntil('\n'); line.trim();
      if (!line.length()) continue;
      int t1 = line.indexOf('\t');
      int t2 = line.indexOf('\t', t1 + 1);
      names[n] = t1 > 0 ? line.substring(0, t1) : line;
      phones[n] = t1 > 0 && t2 > t1 ? line.substring(t1 + 1, t2) : "";
      emails[n] = t2 > 0 ? line.substring(t2 + 1) : "";
      n++;
    }
    f.close();
  }
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print("Contacts");
      const int visible = 8;
      int top = (sel >= visible) ? sel - visible + 1 : 0;
      for (int i = 0; i < visible && top + i < n; i++) {
        int y = 34 + i * 22;
        int idx = top + i;
        if (idx == sel) { gfx->fillRect(0, y - 2, SCREEN_W, 20, TERM_SEL_BG); gfx->setTextColor(BLACK, TERM_SEL_BG); }
        else gfx->setTextColor(WHITE, BLACK);
        gfx->setTextSize(1);
        gfx->setCursor(8, y);
        gfx->print(names[idx]);
        gfx->setCursor(180, y);
        gfx->print(phones[idx]);
      }
      if (n == 0) {
        gfx->setTextSize(1); gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(8, 34); gfx->print("(none - press n to add)");
      }
      gfx->setTextSize(1); gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("n=add d=del Click=edit Long=back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    switch (e.ev) {
      case PDA_EV_UP: if (sel > 0) sel--; needsRedraw = true; break;
      case PDA_EV_DOWN: if (sel < n - 1) sel++; needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        if (n > 0) {
          // view/edit contact
          String out;
          gfx->fillRect(0, SCREEN_H - 40, SCREEN_W, 40, BLACK);
          gfx->setTextSize(1); gfx->setTextColor(TERM_ACCENT, BLACK);
          gfx->setCursor(8, SCREEN_H - 36);
          gfx->print(names[sel]); gfx->print(" | "); gfx->print(phones[sel]);
          gfx->setCursor(8, SCREEN_H - 24);
          gfx->print(emails[sel]);
          InputEventP w; bool done = false;
          while (!done) { if (pdaGetInput(w, 50)) done = true; }
          needsRedraw = true;
        }
        break;
      }
      case PDA_EV_CHAR:
        if (e.ch == 'n' || e.ch == 'N') {
          String name, phone, email;
          if (!promptText("Name", name)) break;
          if (!promptText("Phone", phone)) phone = "";
          if (!promptText("Email", email)) email = "";
          if (n < 32) {
            names[n] = name; phones[n] = phone; emails[n] = email; n++;
            sel = n - 1;
            if (sdOk) {
              if (!SD.exists("/contacts")) SD.mkdir("/contacts");
              File f = SD.open(CONTACTS_FILE, FILE_APPEND);
              if (f) { f.print(name); f.print('\t'); f.print(phone); f.print('\t'); f.print(email); f.print('\n'); f.close(); }
            }
          }
          needsRedraw = true;
        } else if (e.ch == 'd' || e.ch == 'D') {
          if (n > 0) {
            for (int i = sel; i < n - 1; i++) { names[i]=names[i+1]; phones[i]=phones[i+1]; emails[i]=emails[i+1]; }
            n--;
            if (sel >= n && sel > 0) sel--;
            if (sdOk) {
              File f = SD.open(CONTACTS_FILE, FILE_WRITE);
              if (f) { for (int i = 0; i < n; i++) { f.print(names[i]); f.print('\t'); f.print(phones[i]); f.print('\t'); f.print(emails[i]); f.print('\n'); } f.close(); }
            }
            needsRedraw = true;
          }
        }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK:
      case PDA_EV_LEFT: return;
      default: break;
    }
  }
}

// ============================ Unit converter ============================
void convertApp() {
  // categories: base unit in parentheses
  const char *cats[] = {"Length", "Mass", "Temp", "Volume", "Speed", "Area", "Data"};
  const char *units[][5] = {
    {"mm", "cm", "m", "km", "in"},
    {"mg", "g", "kg", "t", "lb"},
    {"C", "F", "K", "", ""},
    {"ml", "l", "m3", "gal", "cup"},
    {"m/s", "km/h", "mph", "kn", "ft/s"},
    {"cm2", "m2", "km2", "acre", "ha"},
    {"B", "KB", "MB", "GB", "Mb"}
  };
  // factors to base (m / kg / liter / m/s / m2 / byte)
  const double fac[][5] = {
    {0.001, 0.01, 1.0, 1000.0, 0.0254},
    {0.000001, 0.001, 1.0, 1000.0, 0.453592},
    {0,0,0,0,0},
    {0.001, 1.0, 1000.0, 3.78541, 0.236588},
    {1.0, 0.277778, 0.44704, 0.514444, 0.3048},
    {0.0001, 1.0, 1000000.0, 4046.86, 10000.0},
    {1.0, 1024.0, 1048576.0, 1073741824.0, 0.125}
  };
  const int nCats = 7;
  int cat = 0, from = 2, to = 0;
  double value = 1.0;
  String input;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print(cats[cat]);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(10, 50);
      gfx->printf("%.4g %s", value, units[cat][from]);
      gfx->setCursor(10, 90);
      gfx->setTextColor(TERM_ACCENT, BLACK);
      if (cat == 2) {
        double c = (from == 0) ? value : (from == 1) ? (value - 32) * 5 / 9 : value - 273.15;
        double out = (to == 0) ? c : (to == 1) ? c * 9 / 5 + 32 : c + 273.15;
        gfx->printf("= %.4g %s", out, units[cat][to]);
      } else {
        double base = value * fac[cat][from];
        gfx->printf("= %.6g %s", base / fac[cat][to], units[cat][to]);
      }
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, 140);
      gfx->print("U/D category  L/R unit  digits then Enter");
      gfx->setCursor(4, 152);
      gfx->printf("value: %s_", input.c_str());
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("f=from-unit toggle  t=to-unit toggle  Long=back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    switch (e.ev) {
      case PDA_EV_UP: cat = (cat + nCats - 1) % nCats; from = (cat == 2) ? 0 : 2; to = (cat == 2) ? 1 : 0; needsRedraw = true; break;
      case PDA_EV_DOWN: cat = (cat + 1) % nCats; from = (cat == 2) ? 0 : 2; to = (cat == 2) ? 1 : 0; needsRedraw = true; break;
      case PDA_EV_LEFT: from = (from + 4) % 5; needsRedraw = true; break;
      case PDA_EV_RIGHT: from = (from + 1) % 5; needsRedraw = true; break;
      case PDA_EV_CHAR: {
        if ((e.ch >= '0' && e.ch <= '9') || e.ch == '.') {
          if (input.length() < 12) { input += e.ch; needsRedraw = true; }
        } else if (e.ch == 'f' || e.ch == 'F') { to = from; from = to; needsRedraw = true; }  // swap handled below
        else if (e.ch == 't' || e.ch == 'T') { to = (to + 1) % 5; needsRedraw = true; }
        break;
      }
      case PDA_EV_DELETE: if (input.length()) { input.remove(input.length()-1); needsRedraw = true; } break;
      case PDA_EV_NEWLINE: {
        if (input.length()) { value = input.toDouble(); needsRedraw = true; }
        break;
      }
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK: return;
      default: break;
    }
  }
}

// ============================ File manager ============================
void filesApp() {
  String path = "/";
  String names[24];
  bool isDir[24];
  int n = 0;
  int sel = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      gfx->fillScreen(BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(8, 8);
      gfx->print(path);
      n = 0;
      File root = SD.open(path);
      if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f && n < 24) {
          names[n] = f.name();
          isDir[n] = f.isDirectory();
          f.close();
          n++;
          f = root.openNextFile();
        }
        root.close();
      }
      const int visible = 13;
      int top = (sel >= visible) ? sel - visible + 1 : 0;
      for (int i = 0; i < visible && top + i < n; i++) {
        int y = 24 + i * 14;
        int idx = top + i;
        if (idx == sel) { gfx->fillRect(0, y - 2, SCREEN_W, 13, TERM_SEL_BG); gfx->setTextColor(BLACK, TERM_SEL_BG); }
        else gfx->setTextColor(isDir[idx] ? TERM_CYAN : WHITE, BLACK);
        gfx->setCursor(8, y);
        gfx->print(isDir[idx] ? "[D] " : "    ");
        gfx->print(names[idx]);
      }
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->print("Click=open del  d=delete  b=up-dir  Long=back");
    }
    InputEventP e;
    if (!pdaGetInput(e, 50)) continue;
    switch (e.ev) {
      case PDA_EV_UP: if (sel > 0) sel--; needsRedraw = true; break;
      case PDA_EV_DOWN: if (sel < n - 1) sel++; needsRedraw = true; break;
      case PDA_EV_SELECT:
      case PDA_EV_NEWLINE: {
        if (sel < n && isDir[sel]) {
          path += (path.endsWith("/") ? "" : "/") + names[sel];
          sel = 0; needsRedraw = true;
        }
        break;
      }
      case PDA_EV_CHAR:
        if (e.ch == 'b' || e.ch == 'B') {
          int slash = path.lastIndexOf('/');
          if (slash > 0) { path = path.substring(0, slash); if (path.length() == 0) path = "/"; sel = 0; needsRedraw = true; }
        } else if (e.ch == 'd' || e.ch == 'D') {
          if (sel < n && !isDir[sel]) {
            String full = path + (path.endsWith("/") ? "" : "/") + names[sel];
            SD.remove(full);
            sel = 0; needsRedraw = true;
          }
        }
        break;
      case PDA_EV_LONGSELECT:
      case PDA_EV_BACK:
      case PDA_EV_LEFT: return;
      default: break;
    }
  }
}
